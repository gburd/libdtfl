/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/server.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/server.c: the live DDIR server registry and lifecycle.
 *
 * server.rs hosts interpreted DDIR programs in one running timely
 * computation and lets them share results by name.  "install" renders a
 * DDIR program against a live registry of named traces; "import" resolves a
 * Source::Trace to a registered trace, "export" arranges a collection by
 * key and publishes its trace.  feed stages input updates, tick closes the
 * epoch and runs to quiescence, drop evicts a program (gated on its traces
 * having no live importer), peek snapshots a trace, list reports the
 * registry.
 *
 * The upstream Server is multi-worker (commands ride a timely Sequencer to
 * every worker; feed and printing happen on worker 0).  This C server runs
 * a single in-process standalone worker and dispatches every command on the
 * calling thread, which is enough to replay the example sessions.  The
 * worker-sharding of generated sources and the exchange-to-worker-0 in peek
 * collapse to the identity on one worker.  Multi-worker (a Sequencer over
 * __dtfl_execute's worker group) is a follow-up; see server.h.
 *
 * Upstream's tick also calls set_logical/physical_compaction on each
 * published trace to shed history.  libdtfl's public trace handle has no
 * compaction setter, so this server does not compact: traces keep their
 * full history, which keeps peek's closed-past snapshot exact.  That costs
 * memory on a long run but changes no result; a compaction knob is a
 * follow-up.
 */
#include "ddir_int.h"
#include "backend_int.h"

#include "../base/base_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/server.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The host (outer) timestamp shared across all installed programs. */
typedef uint64_t outer_time;

/* Where an installed entry came from.  Only PROGRAM is writable by feed;
 * CLOCK additionally has its single row advanced each tick. */
typedef enum origin {
	ORIGIN_PROGRAM,
	ORIGIN_GENERATED,
	ORIGIN_CLOCK
} origin;

/* Everything the server holds for one installed program. */
typedef struct installed {
	char *name;		    /* owned registry key */
	dtfl_ddir_program *prog;    /* owned clone, outlives the dataflow */
	dtfl_input_session **inputs; /* positional input index -> handle */
	size_t *input_pos;	    /* the positional index of inputs[i] */
	size_t n_inputs;
	char **imports;		    /* canonical names this program imports */
	size_t n_imports;
	char **exports;		    /* trace names this program publishes */
	size_t n_exports;
	size_t dataflow_id;	    /* for drop_dataflow on teardown */
	dtfl_probe *probe;	    /* per-program probe; tick waits on it */
	origin origin;
} installed;

/* A published trace: a name and its shareable handle. */
typedef struct reg_trace {
	char *name;		 /* owned */
	dtfl_trace *trace;	 /* owned handle */
	size_t importers;	 /* number of installed programs importing it */
} reg_trace;

struct dtfl_ddir_server {
	dtfl_worker *worker;
	dtfl_time_class *outer_tc; /* u64 */
	dtfl_diff *i64;
	reg_trace *traces;
	size_t n_traces, cap_traces;
	installed *programs;
	size_t n_programs, cap_programs;
	outer_time epoch;	 /* the current open epoch */
	FILE *out;		 /* peek/list/status sink */
};

/* ------------------------------------------------------------------ */
/* Small registry helpers                                              */
/* ------------------------------------------------------------------ */

static reg_trace *
find_trace(dtfl_ddir_server *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->n_traces; i++)
		if (strcmp(s->traces[i].name, name) == 0)
			return &s->traces[i];
	return NULL;
}

static installed *
find_program(dtfl_ddir_server *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->n_programs; i++)
		if (strcmp(s->programs[i].name, name) == 0)
			return &s->programs[i];
	return NULL;
}

static void
register_trace(dtfl_ddir_server *s, const char *name, dtfl_trace *trace)
{
	reg_trace *t;

	if (s->n_traces == s->cap_traces) {
		s->cap_traces = s->cap_traces ? s->cap_traces * 2 : 8;
		s->traces = __dtfl_realloc(s->traces,
		    s->cap_traces * sizeof(reg_trace));
	}
	t = &s->traces[s->n_traces++];
	t->name = __dtfl_strdup(name);
	t->trace = trace;
	t->importers = 0;
}

static void
unregister_trace(dtfl_ddir_server *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->n_traces; i++)
		if (strcmp(s->traces[i].name, name) == 0) {
			__dtfl_free(s->traces[i].name);
			dtfl_trace_free(s->traces[i].trace);
			s->traces[i] = s->traces[--s->n_traces];
			return;
		}
}

static installed *
alloc_program(dtfl_ddir_server *s)
{
	installed *p;

	if (s->n_programs == s->cap_programs) {
		s->cap_programs = s->cap_programs ? s->cap_programs * 2 : 8;
		s->programs = __dtfl_realloc(s->programs,
		    s->cap_programs * sizeof(installed));
	}
	p = &s->programs[s->n_programs++];
	memset(p, 0, sizeof *p);
	return p;
}

/* ------------------------------------------------------------------ */
/* Recipes: generated / named sources                                 */
/* ------------------------------------------------------------------ */

/* A content-addressed generated source. */
typedef enum recipe_kind { RECIPE_NONE, RECIPE_RANDOM, RECIPE_IOTA } recipe_kind;

typedef struct recipe {
	recipe_kind kind;
	uint64_t nodes, edges, seed, n;
	size_t arity;
} recipe;

/* Parse one "k=v" pair of a random: recipe.  Returns false on a bad key or
 * a non-numeric value. */
static bool
parse_kv(const char *kv, recipe *r)
{
	const char *eq = strchr(kv, '=');
	char key[16];
	size_t klen;
	char *end;
	unsigned long long val;

	if (eq == NULL)
		return false;
	klen = (size_t)(eq - kv);
	if (klen == 0 || klen >= sizeof key)
		return false;
	memcpy(key, kv, klen);
	key[klen] = '\0';
	val = strtoull(eq + 1, &end, 10);
	if (end == eq + 1 || *end != '\0')
		return false;
	if (strcmp(key, "nodes") == 0)
		r->nodes = val;
	else if (strcmp(key, "edges") == 0)
		r->edges = val;
	else if (strcmp(key, "arity") == 0)
		r->arity = (size_t)val;
	else if (strcmp(key, "seed") == 0)
		r->seed = val;
	else
		return false;
	return true;
}

/* Parse a recipe name, or RECIPE_NONE if it is an ordinary trace name. */
static recipe
recipe_parse(const char *name)
{
	recipe r;

	memset(&r, 0, sizeof r);
	r.arity = 2;
	if (strncmp(name, "random:", 7) == 0) {
		const char *p = name + 7;
		bool have_nodes = false, have_edges = false;
		char buf[256];
		size_t len = strlen(p), start = 0, i;

		if (len >= sizeof buf) {
			r.kind = RECIPE_NONE;
			return r;
		}
		memcpy(buf, p, len + 1);
		for (i = 0; i <= len; i++) {
			if (buf[i] == ',' || buf[i] == '\0') {
				char save = buf[i];

				buf[i] = '\0';
				if (!parse_kv(buf + start, &r)) {
					r.kind = RECIPE_NONE;
					return r;
				}
				if (strncmp(buf + start, "nodes=", 6) == 0)
					have_nodes = true;
				else if (strncmp(buf + start, "edges=", 6) == 0)
					have_edges = true;
				start = i + 1;
				if (save == '\0')
					break;
			}
		}
		if (!have_nodes || !have_edges) {
			r.kind = RECIPE_NONE;
			return r;
		}
		r.kind = RECIPE_RANDOM;
		return r;
	}
	if (strncmp(name, "iota:", 5) == 0) {
		char *end;
		unsigned long long n = strtoull(name + 5, &end, 10);

		if (end == name + 5 || *end != '\0') {
			r.kind = RECIPE_NONE;
			return r;
		}
		r.kind = RECIPE_IOTA;
		r.n = n;
		return r;
	}
	r.kind = RECIPE_NONE;
	return r;
}

/* The canonical name of a recipe: fixed key order, defaults filled, so
 * reorderings and omitted defaults address the same source.  Caller frees. */
static char *
recipe_canonical(const recipe *r)
{
	char buf[256];

	if (r->kind == RECIPE_RANDOM)
		snprintf(buf, sizeof buf,
		    "random:nodes=%llu,edges=%llu,arity=%zu,seed=%llu",
		    (unsigned long long)r->nodes, (unsigned long long)r->edges,
		    r->arity, (unsigned long long)r->seed);
	else
		snprintf(buf, sizeof buf, "iota:%llu", (unsigned long long)r->n);
	return __dtfl_strdup(buf);
}

/* Map a source name to its canonical form: a recipe canonicalizes, any
 * other name is returned unchanged.  Caller frees. */
static char *
canonical_source_name(const char *name)
{
	recipe r = recipe_parse(name);

	if (r.kind == RECIPE_NONE)
		return __dtfl_strdup(name);
	return recipe_canonical(&r);
}

/* The number of rows a recipe contains. */
static uint64_t
recipe_rows_len(const recipe *r)
{
	return r->kind == RECIPE_RANDOM ? r->edges : r->n;
}

/* The generated (key, val) row at index `e` (owned). */
static void
recipe_row(const recipe *r, uint64_t e, dd_value **key, dd_value **val)
{
	if (r->kind == RECIPE_RANDOM) {
		dd_gen_row(r->seed, e, r->nodes, r->arity, key, val);
	} else {
		dd_value **items = __dtfl_alloc(sizeof *items);

		items[0] = dd_value_int((int64_t)e);
		*key = dd_value_tuple(items, 1);
		*val = dd_value_unit();
	}
}

/* ------------------------------------------------------------------ */
/* Pair encoding helpers                                               */
/* ------------------------------------------------------------------ */

/* Encode a (key, val) pair record into a fresh buffer; caller frees. */
static uint8_t *
encode_pair(const dd_value *k, const dd_value *v, size_t *out_len)
{
	uint8_t *buf = NULL;
	size_t len = 0, cap = 0;

	dd_value_encode(k, &buf, &len, &cap);
	dd_value_encode(v, &buf, &len, &cap);
	*out_len = len;
	return buf;
}

/* The kv_fn for as_collection: re-concatenate the stored key and value into
 * a single dd_pair_type record (the server's collection element). */
static void
pair_kv_fn(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	uint8_t stackbuf[256];
	uint8_t *buf = stackbuf;
	size_t total = klen + vlen;

	(void)ctx;
	if (total > sizeof stackbuf)
		buf = __dtfl_alloc(total);
	memcpy(buf, key, klen);
	memcpy(buf + klen, val, vlen);
	dtfl_arranged_emit_record(out, buf, total);
	if (buf != stackbuf)
		__dtfl_free(buf);
}

/* ------------------------------------------------------------------ */
/* install: render a program over imported traces and positional inputs */
/* ------------------------------------------------------------------ */

/* The generated-source builder: one input, arranged by key, published. */
struct gen_build {
	dtfl_ddir_server *s;
	dtfl_input_session *input;
	dtfl_probe *probe;
	dtfl_trace *trace;
};

static void
dd_server_gen_build(dtfl_scope *scope, void *arg)
{
	struct gen_build *gb = arg;
	dtfl_input_session *h = dtfl_input_session_new(dd_pair_type(),
	    gb->s->i64);
	dtfl_collection *col = dtfl_input_session_to_collection(h, scope);
	dtfl_collection *probed = dtfl_collection_probe_with(col, gb->probe);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(probed,
	    dd_value_type(), dd_value_type(), "ServerExport");

	gb->input = h;
	gb->trace = dtfl_arranged_trace(arr);
	dtfl_arranged_free(arr);
	dtfl_collection_free(probed);
	dtfl_collection_free(col);
}

/* Context carried through the install dataflow build. */
typedef struct install_ctx {
	dtfl_ddir_server *s;
	const dtfl_ddir_program *program;
	dtfl_probe *probe;
	/* Outputs filled by the build: published (name, trace) per export and
	 * input (pos, handle) per positional input. */
	char **pub_names;
	dtfl_trace **pub_traces;
	size_t n_pub;
	dtfl_input_session **inputs;
	size_t *input_pos;
	size_t n_inputs;
	int err;
} install_ctx;

/* The iterative-scope body: enter each root import collection, render the
 * program, and leave each export back to the outer scope. */
typedef struct install_iter {
	install_ctx *ic;
	dtfl_collection *const *outer_cols; /* one per root import */
	size_t n_imports;
	dtfl_collection **leaving;	    /* one per export (owned) */
	size_t n_exports;
	int err;
} install_iter;

static void
install_iter_fn(dtfl_scope *inner, void *arg)
{
	install_iter *ii = arg;
	const dd_scope *root = &ii->ic->program->root;
	size_t ne = root->exports.len;
	dtfl_collection **entered;
	dtfl_collection **exports;
	size_t i;

	entered = ii->n_imports
	    ? __dtfl_alloc(ii->n_imports * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < ii->n_imports; i++)
		entered[i] = dtfl_collection_enter(
		    (dtfl_collection *)ii->outer_cols[i], inner);
	exports = ne ? __dtfl_alloc(ne * sizeof(dtfl_collection *)) : NULL;
	ii->err = dd_render_program_bound(ii->ic->program, inner, entered,
	    ii->n_imports, exports);
	ii->n_exports = ne;
	ii->leaving = ne ? __dtfl_alloc(ne * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < ne; i++)
		ii->leaving[i] = NULL;
	if (ii->err == 0)
		for (i = 0; i < ne; i++) {
			ii->leaving[i] = dtfl_collection_leave(exports[i]);
			dtfl_collection_free(exports[i]);
		}
	else
		for (i = 0; i < ne; i++)
			if (exports[i] != NULL)
				dtfl_collection_free(exports[i]);
	for (i = 0; i < ii->n_imports; i++)
		dtfl_collection_free(entered[i]);
	__dtfl_free(entered);
	__dtfl_free(exports);
}

static void
install_build_fn(dtfl_scope *scope, void *arg)
{
	install_ctx *ic = arg;
	dtfl_ddir_server *s = ic->s;
	const dd_scope *root = &ic->program->root;
	dtfl_time_class *inner_tc = dtfl_ddir_time_class();
	dtfl_collection **outer_cols;
	install_iter ii;
	size_t i, in_i = 0;

	/* One outer (host-time) collection per root import: a new input for
	 * Source::Input, an imported trace for Source::Trace. */
	outer_cols = root->imports.len
	    ? __dtfl_alloc(root->imports.len * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < root->imports.len; i++) {
		dd_import *imp = root->imports.data[i];

		if (imp->from.kind == DD_SRC_INPUT) {
			dtfl_input_session *h = dtfl_input_session_new(
			    dd_pair_type(), s->i64);

			ic->inputs[in_i] = h;
			ic->input_pos[in_i] = imp->from.u.input;
			in_i++;
			outer_cols[i] = dtfl_input_session_to_collection(h,
			    scope);
		} else if (imp->from.kind == DD_SRC_TRACE) {
			char *canon = canonical_source_name(imp->from.u.trace);
			reg_trace *rt = find_trace(s, canon);
			dtfl_arranged *imported;

			__dtfl_free(canon);
			/* Resolved and installed before the build (below). */
			DTFL_ASSERT(rt != NULL);
			imported = dtfl_trace_import(rt->trace, scope,
			    "ServerImport");
			outer_cols[i] = dtfl_arranged_as_collection(imported,
			    dd_pair_type(), pair_kv_fn, DTFL_NOCTX);
			dtfl_arranged_free(imported);
		} else {
			__dtfl_panic("install: root import from a parent scope");
		}
	}
	ic->n_inputs = in_i;

	memset(&ii, 0, sizeof ii);
	ii.ic = ic;
	ii.outer_cols = outer_cols;
	ii.n_imports = root->imports.len;
	dtfl_scope_scoped(scope, "iterate", inner_tc, install_iter_fn, &ii);
	ic->err = ii.err;

	if (ic->err == 0) {
		/* Publish each export: probe, arrange by key, keep the trace. */
		for (i = 0; i < ii.n_exports; i++) {
			dd_export *e = root->exports.data[i];
			dtfl_collection *probed = dtfl_collection_probe_with(
			    ii.leaving[i], ic->probe);
			dtfl_arranged *arr = dtfl_collection_arrange_by_key(
			    probed, dd_value_type(), dd_value_type(),
			    "ServerExport");

			ic->pub_names[i] = __dtfl_strdup(e->name);
			ic->pub_traces[i] = dtfl_arranged_trace(arr);
			ic->n_pub++;
			dtfl_arranged_free(arr);
			dtfl_collection_free(probed);
			dtfl_collection_free(ii.leaving[i]);
		}
	} else {
		for (i = 0; i < ii.n_exports; i++)
			if (ii.leaving != NULL && ii.leaving[i] != NULL)
				dtfl_collection_free(ii.leaving[i]);
	}
	__dtfl_free(ii.leaving);
	for (i = 0; i < root->imports.len; i++)
		dtfl_collection_free(outer_cols[i]);
	__dtfl_free(outer_cols);
	dtfl_time_class_free(inner_tc);
}

/* Install a generated source under its canonical `name`: a one-input
 * dataflow pre-filled with the recipe's rows at time 0, published as a
 * trace.  Content-addressed; not writable; dropped like any program. */
static void
install_generated(dtfl_ddir_server *s, const char *name, const recipe *r)
{
	dtfl_probe *probe = dtfl_probe_new();
	size_t dataflow_id = dtfl_worker_next_dataflow_index(s->worker);
	struct gen_build gb;
	installed *p;
	uint64_t e;

	gb.s = s;
	gb.probe = probe;
	gb.input = NULL;
	gb.trace = NULL;
	dtfl_worker_dataflow(s->worker, name, s->outer_tc, dd_server_gen_build,
	    &gb);

	for (e = 0; e < recipe_rows_len(r); e++) {
		dd_value *k, *v;
		uint8_t *buf;
		size_t blen;

		recipe_row(r, e, &k, &v);
		buf = encode_pair(k, v, &blen);
		dtfl_input_session_update_at(gb.input, buf, blen,
		    &(uint64_t){ 0 }, &(int64_t){ 1 });
		__dtfl_free(buf);
		dd_value_free(k);
		dd_value_free(v);
	}
	dtfl_input_session_advance_to(gb.input, &s->epoch);
	dtfl_input_session_flush(gb.input);

	register_trace(s, name, gb.trace);
	p = alloc_program(s);
	p->name = __dtfl_strdup(name);
	p->inputs = __dtfl_alloc(sizeof(dtfl_input_session *));
	p->inputs[0] = gb.input;
	p->input_pos = __dtfl_alloc(sizeof(size_t));
	p->input_pos[0] = 0;
	p->n_inputs = 1;
	p->exports = __dtfl_alloc(sizeof(char *));
	p->exports[0] = __dtfl_strdup(name);
	p->n_exports = 1;
	p->dataflow_id = dataflow_id;
	p->probe = probe;
	p->origin = ORIGIN_GENERATED;
}

/* The single clock row for epoch `t`: (Tuple[t] ; ()).  Caller owns. */
static void
clock_row(outer_time t, dd_value **key, dd_value **val)
{
	dd_value **items = __dtfl_alloc(sizeof *items);

	items[0] = dd_value_int((int64_t)t);
	*key = dd_value_tuple(items, 1);
	*val = dd_value_unit();
}

/* Install the `clock` source: a single row holding the current epoch, which
 * tick advances by one. */
static void
install_clock(dtfl_ddir_server *s)
{
	dtfl_probe *probe = dtfl_probe_new();
	size_t dataflow_id = dtfl_worker_next_dataflow_index(s->worker);
	struct gen_build gb;
	installed *p;
	dd_value *k, *v;
	uint8_t *buf;
	size_t blen;

	gb.s = s;
	gb.probe = probe;
	gb.input = NULL;
	gb.trace = NULL;
	dtfl_worker_dataflow(s->worker, "clock", s->outer_tc,
	    dd_server_gen_build, &gb);

	clock_row(s->epoch, &k, &v);
	buf = encode_pair(k, v, &blen);
	dtfl_input_session_update_at(gb.input, buf, blen, &s->epoch,
	    &(int64_t){ 1 });
	__dtfl_free(buf);
	dd_value_free(k);
	dd_value_free(v);
	dtfl_input_session_advance_to(gb.input, &s->epoch);
	dtfl_input_session_flush(gb.input);

	register_trace(s, "clock", gb.trace);
	p = alloc_program(s);
	p->name = __dtfl_strdup("clock");
	p->inputs = __dtfl_alloc(sizeof(dtfl_input_session *));
	p->inputs[0] = gb.input;
	p->input_pos = __dtfl_alloc(sizeof(size_t));
	p->input_pos[0] = 0;
	p->n_inputs = 1;
	p->exports = __dtfl_alloc(sizeof(char *));
	p->exports[0] = __dtfl_strdup("clock");
	p->n_exports = 1;
	p->dataflow_id = dataflow_id;
	p->probe = probe;
	p->origin = ORIGIN_CLOCK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

dtfl_ddir_server *
dtfl_ddir_server_new(FILE *out)
{
	dtfl_ddir_server *s = __dtfl_calloc(1, sizeof *s);

	s->worker = dtfl_worker_new_standalone();
	s->outer_tc = dtfl_time_u64();
	s->i64 = dtfl_diff_i64();
	s->epoch = 0;
	s->out = out ? out : stdout;
	return s;
}

uint64_t
dtfl_ddir_server_epoch(const dtfl_ddir_server *s)
{
	return s->epoch;
}

bool
dtfl_ddir_server_has_trace(const dtfl_ddir_server *s, const char *name)
{
	return find_trace((dtfl_ddir_server *)s, name) != NULL;
}

int
dtfl_ddir_server_install(dtfl_ddir_server *s, const char *name,
    const dtfl_ddir_program *program)
{
	const dd_scope *root;
	install_ctx ic;
	size_t i, n_in = 0, dataflow_id;
	installed *p;

	if (s == NULL || name == NULL || program == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_ddir_server_install: NULL argument");
	root = &program->root;
	if (find_program(s, name) != NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "a program named \"%s\" is already installed", name);

	/* Resolve trace imports: install generated sources on demand; a name
	 * that is neither registered nor a recipe is an error. */
	for (i = 0; i < root->imports.len; i++) {
		dd_import *imp = root->imports.data[i];

		if (imp->from.kind == DD_SRC_INPUT) {
			n_in++;
			continue;
		}
		if (imp->from.kind == DD_SRC_TRACE) {
			char *canon = canonical_source_name(imp->from.u.trace);

			if (find_trace(s, canon) == NULL) {
				recipe r = recipe_parse(canon);

				if (strcmp(canon, "clock") == 0) {
					install_clock(s);
				} else if (r.kind != RECIPE_NONE) {
					install_generated(s, canon, &r);
				} else {
					int rc = __dtfl_err(DTFL_EINVAL,
					    "program \"%s\" imports unknown "
					    "trace \"%s\"; install its "
					    "producer first", name,
					    imp->from.u.trace);
					__dtfl_free(canon);
					return rc;
				}
			}
			__dtfl_free(canon);
		}
	}
	/* No export name may already be published. */
	for (i = 0; i < root->exports.len; i++) {
		dd_export *e = root->exports.data[i];

		if (find_trace(s, e->name) != NULL)
			return __dtfl_err(DTFL_EINVAL,
			    "export name \"%s\" is already published; choose "
			    "another name or drop its producer", e->name);
	}

	dataflow_id = dtfl_worker_next_dataflow_index(s->worker);

	memset(&ic, 0, sizeof ic);
	ic.s = s;
	/* The rendered dataflow holds pointers into the program's AST (every
	 * linear op reads its Term each step), so the server owns a clone that
	 * outlives the dataflow; the caller's program may be freed at once. */
	ic.program = dd_program_clone(program);
	ic.probe = dtfl_probe_new();
	ic.pub_names = root->exports.len
	    ? __dtfl_calloc(root->exports.len, sizeof(char *)) : NULL;
	ic.pub_traces = root->exports.len
	    ? __dtfl_calloc(root->exports.len, sizeof(dtfl_trace *)) : NULL;
	ic.inputs = n_in ? __dtfl_calloc(n_in, sizeof(dtfl_input_session *))
	    : NULL;
	ic.input_pos = n_in ? __dtfl_calloc(n_in, sizeof(size_t)) : NULL;

	dtfl_worker_dataflow(s->worker, name, s->outer_tc, install_build_fn,
	    &ic);

	if (ic.err != 0) {
		for (i = 0; i < ic.n_pub; i++) {
			__dtfl_free(ic.pub_names[i]);
			dtfl_trace_free(ic.pub_traces[i]);
		}
		for (i = 0; i < ic.n_inputs; i++)
			dtfl_input_session_close(ic.inputs[i]);
		__dtfl_free(ic.pub_names);
		__dtfl_free(ic.pub_traces);
		__dtfl_free(ic.inputs);
		__dtfl_free(ic.input_pos);
		dtfl_probe_free(ic.probe);
		dd_program_free((dtfl_ddir_program *)ic.program);
		return ic.err;
	}

	/* Register the published traces and bump importer counts. */
	for (i = 0; i < ic.n_pub; i++) {
		register_trace(s, ic.pub_names[i], ic.pub_traces[i]);
		__dtfl_free(ic.pub_names[i]);
	}
	__dtfl_free(ic.pub_names);
	__dtfl_free(ic.pub_traces);

	p = alloc_program(s);
	p->name = __dtfl_strdup(name);
	p->prog = (dtfl_ddir_program *)ic.program;
	p->inputs = ic.inputs;
	p->input_pos = ic.input_pos;
	p->n_inputs = ic.n_inputs;
	p->probe = ic.probe;
	p->dataflow_id = dataflow_id;
	p->origin = ORIGIN_PROGRAM;
	/* Record import and export names for drop bookkeeping. */
	p->imports = root->imports.len
	    ? __dtfl_calloc(root->imports.len, sizeof(char *)) : NULL;
	for (i = 0; i < root->imports.len; i++) {
		dd_import *imp = root->imports.data[i];

		if (imp->from.kind == DD_SRC_TRACE) {
			char *canon = canonical_source_name(imp->from.u.trace);
			reg_trace *rt = find_trace(s, canon);

			p->imports[p->n_imports++] = canon;
			if (rt != NULL)
				rt->importers++;
		}
	}
	p->exports = root->exports.len
	    ? __dtfl_calloc(root->exports.len, sizeof(char *)) : NULL;
	for (i = 0; i < root->exports.len; i++) {
		dd_export *e = root->exports.data[i];

		p->exports[p->n_exports++] = __dtfl_strdup(e->name);
	}

	/* Advance new inputs to the current epoch so they are consistent with
	 * the traces they may already see. */
	for (i = 0; i < p->n_inputs; i++) {
		dtfl_input_session_advance_to(p->inputs[i], &s->epoch);
		dtfl_input_session_flush(p->inputs[i]);
	}
	return 0;
}

int
dtfl_ddir_server_feed(dtfl_ddir_server *s, const char *prog, size_t input,
    const dtfl_ddir_value *key, const dtfl_ddir_value *val, bool has_time,
    uint64_t time, int64_t diff)
{
	outer_time t = has_time ? time : s->epoch;
	char *canon;
	installed *p;
	dtfl_input_session *h = NULL;
	dd_value *kv, *vv;
	uint8_t *buf;
	size_t blen, i;

	if (s == NULL || prog == NULL || key == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_ddir_server_feed: NULL argument");
	if (t < s->epoch)
		return __dtfl_err(DTFL_EINVAL,
		    "cannot feed at time %llu < current epoch %llu",
		    (unsigned long long)t, (unsigned long long)s->epoch);
	canon = canonical_source_name(prog);
	p = find_program(s, canon);
	__dtfl_free(canon);
	if (p == NULL)
		return __dtfl_err(DTFL_EINVAL, "no program \"%s\"", prog);
	if (p->origin != ORIGIN_PROGRAM)
		return __dtfl_err(DTFL_EINVAL,
		    "\"%s\" is a %s source and is not writable", prog,
		    p->origin == ORIGIN_CLOCK ? "clock" : "generated");
	for (i = 0; i < p->n_inputs; i++)
		if (p->input_pos[i] == input) {
			h = p->inputs[i];
			break;
		}
	if (h == NULL)
		return __dtfl_err(DTFL_EINVAL, "program \"%s\" has no input %zu",
		    prog, input);

	kv = (dd_value *)key;
	vv = val != NULL ? (dd_value *)val : NULL;
	if (vv == NULL) {
		dd_value *unit = dd_value_unit();

		buf = encode_pair(kv, unit, &blen);
		dd_value_free(unit);
	} else {
		buf = encode_pair(kv, vv, &blen);
	}
	dtfl_input_session_update_at(h, buf, blen, &t, &diff);
	__dtfl_free(buf);
	return 0;
}

/* A probe is behind if any live program's probe is less than the frontier. */
static bool
any_program_behind(dtfl_ddir_server *s, outer_time frontier)
{
	size_t i;

	for (i = 0; i < s->n_programs; i++)
		if (dtfl_probe_less_than(s->programs[i].probe, &frontier))
			return true;
	return false;
}

void
dtfl_ddir_server_tick(dtfl_ddir_server *s)
{
	outer_time cur = s->epoch;
	outer_time next = cur + 1;
	size_t i, j;

	for (i = 0; i < s->n_programs; i++) {
		installed *p = &s->programs[i];

		/* The clock's single row advances by one each tick: retract the
		 * current epoch, add the next (an O(1) change). */
		if (p->origin == ORIGIN_CLOCK && p->n_inputs > 0) {
			dd_value *k0, *v0, *k1, *v1;
			uint8_t *b;
			size_t bl;

			clock_row(cur, &k0, &v0);
			b = encode_pair(k0, v0, &bl);
			dtfl_input_session_update_at(p->inputs[0], b, bl,
			    &next, &(int64_t){ -1 });
			__dtfl_free(b);
			dd_value_free(k0);
			dd_value_free(v0);
			clock_row(next, &k1, &v1);
			b = encode_pair(k1, v1, &bl);
			dtfl_input_session_update_at(p->inputs[0], b, bl,
			    &next, &(int64_t){ 1 });
			__dtfl_free(b);
			dd_value_free(k1);
			dd_value_free(v1);
		}
		for (j = 0; j < p->n_inputs; j++) {
			dtfl_input_session_advance_to(p->inputs[j], &next);
			dtfl_input_session_flush(p->inputs[j]);
		}
	}
	s->epoch = next;

	/* Wait for every live program to catch up to the new epoch. */
	while (any_program_behind(s, s->epoch))
		dtfl_worker_step(s->worker);
}

int
dtfl_ddir_server_drop(dtfl_ddir_server *s, const char *name)
{
	char *canon;
	installed *p;
	size_t idx, i, live;

	if (s == NULL || name == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_ddir_server_drop: NULL argument");
	canon = canonical_source_name(name);
	p = find_program(s, canon);
	if (p == NULL) {
		int rc = __dtfl_err(DTFL_EINVAL, "no program \"%s\"", name);

		__dtfl_free(canon);
		return rc;
	}
	/* The gate: refuse if any published trace still has a live importer. */
	for (i = 0; i < p->n_exports; i++) {
		reg_trace *rt = find_trace(s, p->exports[i]);

		live = rt != NULL ? rt->importers : 0;
		if (live > 0) {
			int rc = __dtfl_err(DTFL_EINVAL,
			    "cannot drop \"%s\": its trace \"%s\" has %zu live "
			    "importer(s); drop them first", name, p->exports[i],
			    live);

			__dtfl_free(canon);
			return rc;
		}
	}

	/* Decrement importer counts, unregister published traces, close
	 * inputs, remove the dataflow. */
	for (i = 0; i < p->n_imports; i++) {
		reg_trace *rt = find_trace(s, p->imports[i]);

		if (rt != NULL && rt->importers > 0)
			rt->importers--;
	}
	for (i = 0; i < p->n_exports; i++)
		unregister_trace(s, p->exports[i]);
	for (i = 0; i < p->n_inputs; i++)
		dtfl_input_session_close(p->inputs[i]);
	dtfl_probe_free(p->probe);
	dtfl_worker_drop_dataflow(s->worker, p->dataflow_id);

	/* Free the program's bookkeeping. */
	__dtfl_free(p->inputs);
	__dtfl_free(p->input_pos);
	for (i = 0; i < p->n_imports; i++)
		__dtfl_free(p->imports[i]);
	__dtfl_free(p->imports);
	for (i = 0; i < p->n_exports; i++)
		__dtfl_free(p->exports[i]);
	__dtfl_free(p->exports);
	__dtfl_free(p->name);
	dd_program_free(p->prog);

	idx = (size_t)(p - s->programs);
	s->programs[idx] = s->programs[--s->n_programs];
	__dtfl_free(canon);
	return 0;
}

/* ------------------------------------------------------------------ */
/* peek: snapshot a trace, consolidate, print                          */
/* ------------------------------------------------------------------ */

/* An accumulated (key, val) net multiplicity. */
typedef struct peek_row {
	dd_value *key, *val;
	int64_t diff;
} peek_row;

typedef struct peek_acc {
	peek_row *rows;
	size_t n, cap;
	outer_time epoch;
	const dd_value *key_filter;
} peek_acc;

static void
peek_add(peek_acc *a, dd_value *k, dd_value *v, int64_t d)
{
	size_t i;

	for (i = 0; i < a->n; i++)
		if (dd_value_cmp(a->rows[i].key, k) == 0 &&
		    dd_value_cmp(a->rows[i].val, v) == 0) {
			a->rows[i].diff += d;
			dd_value_free(k);
			dd_value_free(v);
			return;
		}
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 16;
		a->rows = __dtfl_realloc(a->rows, a->cap * sizeof(peek_row));
	}
	a->rows[a->n].key = k;
	a->rows[a->n].val = v;
	a->rows[a->n].diff = d;
	a->n++;
}

static void
peek_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	peek_acc *a = ctx;
	const uint8_t *p = rec;
	const uint8_t *end = p + len;
	dd_value *k, *v;
	int64_t d;
	outer_time t;

	memcpy(&t, time, sizeof t);
	/* The snapshot as of `epoch`: the closed past (t < epoch). */
	if (t >= a->epoch)
		return;
	k = dd_value_decode(&p, end);
	v = dd_value_decode(&p, end);
	memcpy(&d, diff, sizeof d);
	if (a->key_filter != NULL && dd_value_cmp(k, a->key_filter) != 0) {
		dd_value_free(k);
		dd_value_free(v);
		return;
	}
	peek_add(a, k, v, d);
}

/* The transient peek dataflow: import the trace, filter, inspect. */
typedef struct peek_build {
	dtfl_ddir_server *s;
	dtfl_trace *trace;
	peek_acc *acc;
	dtfl_probe *probe;
} peek_build;

static void
peek_build_fn(dtfl_scope *scope, void *arg)
{
	peek_build *pb = arg;
	dtfl_arranged *imported = dtfl_trace_import(pb->trace, scope,
	    "PeekImport");
	dtfl_collection *coll = dtfl_arranged_as_collection(imported,
	    dd_pair_type(), pair_kv_fn, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(coll, peek_sink,
	    DTFL_CTX(pb->acc, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, pb->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(coll);
	dtfl_arranged_free(imported);
}

static int
peek_cmp(const void *a, const void *b)
{
	const peek_row *ra = a, *rb = b;
	int c = dd_value_cmp(ra->key, rb->key);

	return c != 0 ? c : dd_value_cmp(ra->val, rb->val);
}

int
dtfl_ddir_server_peek(dtfl_ddir_server *s, const char *trace,
    const dtfl_ddir_value *key)
{
	char *canon;
	reg_trace *rt;
	peek_acc acc;
	peek_build pb;
	size_t peek_id, i, kept = 0;
	char kbuf[512], vbuf[512];

	if (s == NULL || trace == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_ddir_server_peek: NULL argument");
	canon = canonical_source_name(trace);
	rt = find_trace(s, canon);
	if (rt == NULL) {
		int rc = __dtfl_err(DTFL_EINVAL, "no trace \"%s\"", canon);

		__dtfl_free(canon);
		return rc;
	}

	memset(&acc, 0, sizeof acc);
	acc.epoch = s->epoch;
	acc.key_filter = (const dd_value *)key;

	pb.s = s;
	pb.trace = rt->trace;
	pb.acc = &acc;
	pb.probe = dtfl_probe_new();
	peek_id = dtfl_worker_next_dataflow_index(s->worker);
	dtfl_worker_dataflow(s->worker, "peek", s->outer_tc, peek_build_fn,
	    &pb);

	/* Drain the transient dataflow up to the current epoch, then drop. */
	while (dtfl_probe_less_than(pb.probe, &s->epoch))
		dtfl_worker_step(s->worker);
	dtfl_worker_drop_dataflow(s->worker, peek_id);
	dtfl_probe_free(pb.probe);

	for (i = 0; i < acc.n; i++)
		if (acc.rows[i].diff != 0)
			kept++;
	if (key != NULL) {
		dtfl_ddir_value_format(key, kbuf, sizeof kbuf);
		fprintf(s->out, "peek \"%s\" key=%s (%zu rows):\n", canon, kbuf,
		    kept);
	} else {
		fprintf(s->out, "peek \"%s\" (%zu rows):\n", canon, kept);
	}
	if (acc.n > 1)
		qsort(acc.rows, acc.n, sizeof(peek_row), peek_cmp);
	for (i = 0; i < acc.n; i++) {
		if (acc.rows[i].diff == 0) {
			dd_value_free(acc.rows[i].key);
			dd_value_free(acc.rows[i].val);
			continue;
		}
		dtfl_ddir_value_format(acc.rows[i].key, kbuf, sizeof kbuf);
		dtfl_ddir_value_format(acc.rows[i].val, vbuf, sizeof vbuf);
		fprintf(s->out, "  (%s, %s)  x%lld\n", kbuf, vbuf,
		    (long long)acc.rows[i].diff);
		dd_value_free(acc.rows[i].key);
		dd_value_free(acc.rows[i].val);
	}
	__dtfl_free(acc.rows);
	__dtfl_free(canon);
	return 0;
}

/* ------------------------------------------------------------------ */
/* list                                                                */
/* ------------------------------------------------------------------ */

static int
cmp_strp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void
dtfl_ddir_server_list(const dtfl_ddir_server *s)
{
	const char **names;
	size_t i;

	fprintf(s->out, "epoch: %llu\n", (unsigned long long)s->epoch);
	fprintf(s->out, "traces (%zu):\n", s->n_traces);
	names = s->n_traces ? __dtfl_alloc(s->n_traces * sizeof(char *)) : NULL;
	for (i = 0; i < s->n_traces; i++)
		names[i] = s->traces[i].name;
	if (s->n_traces > 1)
		qsort(names, s->n_traces, sizeof(char *), cmp_strp);
	for (i = 0; i < s->n_traces; i++) {
		reg_trace *rt = find_trace((dtfl_ddir_server *)s, names[i]);

		fprintf(s->out, "  %s (importers: %zu)\n", names[i],
		    rt != NULL ? rt->importers : 0);
	}
	__dtfl_free(names);

	fprintf(s->out, "programs (%zu):\n", s->n_programs);
	names = s->n_programs
	    ? __dtfl_alloc(s->n_programs * sizeof(char *)) : NULL;
	for (i = 0; i < s->n_programs; i++)
		names[i] = s->programs[i].name;
	if (s->n_programs > 1)
		qsort(names, s->n_programs, sizeof(char *), cmp_strp);
	for (i = 0; i < s->n_programs; i++) {
		installed *p = find_program((dtfl_ddir_server *)s, names[i]);
		const char *tag = "";

		if (p->origin == ORIGIN_GENERATED)
			tag = " [generated]";
		else if (p->origin == ORIGIN_CLOCK)
			tag = " [clock]";
		fprintf(s->out, "  %s%s (inputs: %zu, imports: %zu, exports: "
		    "%zu)\n", names[i], tag, p->n_inputs, p->n_imports,
		    p->n_exports);
	}
	__dtfl_free(names);
}

void
dtfl_ddir_server_free(dtfl_ddir_server *s)
{
	size_t i, j;

	if (s == NULL)
		return;
	for (i = 0; i < s->n_programs; i++) {
		installed *p = &s->programs[i];

		for (j = 0; j < p->n_inputs; j++)
			dtfl_input_session_close(p->inputs[j]);
		__dtfl_free(p->inputs);
		__dtfl_free(p->input_pos);
		for (j = 0; j < p->n_imports; j++)
			__dtfl_free(p->imports[j]);
		__dtfl_free(p->imports);
		for (j = 0; j < p->n_exports; j++)
			__dtfl_free(p->exports[j]);
		__dtfl_free(p->exports);
		dtfl_probe_free(p->probe);
		__dtfl_free(p->name);
		dd_program_free(p->prog);
	}
	__dtfl_free(s->programs);
	for (i = 0; i < s->n_traces; i++) {
		__dtfl_free(s->traces[i].name);
		dtfl_trace_free(s->traces[i].trace);
	}
	__dtfl_free(s->traces);
	/* Drain any remaining dataflows before freeing the worker. */
	while (dtfl_worker_step(s->worker))
		;
	dtfl_time_class_free(s->outer_tc);
	dtfl_diff_free(s->i64);
	dtfl_worker_free(s->worker);
	__dtfl_free(s);
}
