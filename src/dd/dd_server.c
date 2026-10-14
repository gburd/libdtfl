/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   server/src/lib.rs
 *   server/src/bin/dd_server.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/dd_server.c: the shared-dataflow plugin server.
 *
 * lib.rs defines the common layout the plugins agree on: a RootTime (a u64
 * host epoch), a shared TraceHandle, a TraceHandler mapping names to boxed
 * trace handles, and an Environment tuple (scope, handler, probe, timer,
 * args) passed to each plugin's build function.  dd_server.rs runs the
 * worker, serializes commands, and on "load" opens a shared library and
 * calls its build symbol with that environment, giving each loaded dataflow
 * access to the one shared handler so arrangements import by name.
 *
 * This port keeps the shared TraceHandler, the per-load dataflow, the common
 * probe and the host epoch, but a plugin is a statically registered
 * dtfl_dataflow_plugin (name plus build callback) rather than a dlopened
 * shared object: library code may not dlopen (AGENTS.md section 5), and
 * static registration keeps the public dtfl_* API the only coupling between
 * server and plugin.  The server runs one standalone in-process worker and
 * dispatches each command on the calling thread, as the DDIR server does
 * (src/ddir/server.c); a load builds a dataflow and steps the worker to
 * quiescence, so there is no accept loop.
 *
 * Compaction: upstream advances each trace's logical/physical compaction as
 * it runs; libdtfl's public trace handle has no compaction setter, so this
 * server keeps a trace's full history.  That costs memory on a long run but
 * changes no result, and keeps peek's closed-past snapshot exact.  A
 * compaction knob is a follow-up, as noted in src/ddir/server.c.
 */
#include "dd/dd_int.h"

#include "base/base_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dd_server.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* The trace handler (TraceHandler)                                    */
/* ------------------------------------------------------------------ */

/* One registered shared trace: a name, the handle plugins import, and the
 * full record type (key || value) for formatting and comparison. */
typedef struct handler_entry {
	char *name;		/* owned registry key */
	dtfl_trace *trace;	/* owned handle, imported by plugins */
	const dtfl_type *type;	/* retained record type, or NULL */
} handler_entry;

struct dtfl_trace_handler {
	handler_entry *entries;
	size_t n, cap;
};

static handler_entry *
handler_find(dtfl_trace_handler *h, const char *name)
{
	size_t i;

	for (i = 0; i < h->n; i++)
		if (strcmp(h->entries[i].name, name) == 0)
			return &h->entries[i];
	return NULL;
}

dtfl_trace *
dtfl_trace_handler_get(dtfl_trace_handler *h, const char *name)
{
	handler_entry *e;

	if (h == NULL || name == NULL) {
		__dtfl_err(DTFL_EINVAL, "dtfl_trace_handler_get: NULL argument");
		return NULL;
	}
	e = handler_find(h, name);
	if (e == NULL) {
		__dtfl_err(DTFL_EINVAL, "failed to find handle: \"%s\"", name);
		return NULL;
	}
	return e->trace;
}

void
dtfl_trace_handler_set(dtfl_trace_handler *h, const char *name,
    dtfl_trace *trace, const dtfl_type *record_type)
{
	handler_entry *e;

	if (h == NULL || name == NULL)
		return;
	e = handler_find(h, name);
	if (e != NULL) {
		/* Replace: drop the old handle (TraceHandler::set insert). */
		dtfl_trace_free(e->trace);
		if (e->type != NULL)
			dtfl_type_free(e->type);
		e->trace = trace;
		e->type = record_type != NULL ?
		    dtfl_type_retain(record_type) : NULL;
		return;
	}
	if (h->n == h->cap) {
		h->cap = h->cap ? h->cap * 2 : 8;
		h->entries = __dtfl_realloc(h->entries,
		    h->cap * sizeof(handler_entry));
	}
	e = &h->entries[h->n++];
	e->name = __dtfl_strdup(name);
	e->trace = trace;
	e->type = record_type != NULL ? dtfl_type_retain(record_type) : NULL;
}

bool
dtfl_trace_handler_has(const dtfl_trace_handler *h, const char *name)
{
	size_t i;

	if (h == NULL || name == NULL)
		return false;
	for (i = 0; i < h->n; i++)
		if (strcmp(h->entries[i].name, name) == 0)
			return true;
	return false;
}

/* ------------------------------------------------------------------ */
/* The environment (Environment)                                       */
/* ------------------------------------------------------------------ */

struct dtfl_dd_env {
	dtfl_scope *scope;
	dtfl_trace_handler *handles;
	dtfl_probe *probe;
	uint64_t elapsed_ns;
	size_t argc;
	const char *const *argv;
	/* Inputs the plugin registers for the server to drive. */
	dtfl_input_session **inputs;
	size_t n_inputs, cap_inputs;
};

dtfl_scope *
dtfl_dd_env_scope(dtfl_dd_env *e)
{
	return e->scope;
}

dtfl_trace_handler *
dtfl_dd_env_handles(dtfl_dd_env *e)
{
	return e->handles;
}

dtfl_probe *
dtfl_dd_env_probe(dtfl_dd_env *e)
{
	return e->probe;
}

uint64_t
dtfl_dd_env_elapsed_ns(const dtfl_dd_env *e)
{
	return e->elapsed_ns;
}

size_t
dtfl_dd_env_argc(const dtfl_dd_env *e)
{
	return e->argc;
}

const char *
dtfl_dd_env_arg(const dtfl_dd_env *e, size_t i)
{
	return i < e->argc ? e->argv[i] : NULL;
}

void
dtfl_dd_env_add_input(dtfl_dd_env *e, dtfl_input_session *in)
{
	if (e == NULL || in == NULL)
		return;
	if (e->n_inputs == e->cap_inputs) {
		e->cap_inputs = e->cap_inputs ? e->cap_inputs * 2 : 4;
		e->inputs = __dtfl_realloc(e->inputs,
		    e->cap_inputs * sizeof(dtfl_input_session *));
	}
	e->inputs[e->n_inputs++] = in;
}

/* ------------------------------------------------------------------ */
/* The server                                                          */
/* ------------------------------------------------------------------ */

/* A registered plugin: the static-registration stand-in for a library. */
typedef struct reg_plugin {
	char *name;		/* owned copy of plugin->name */
	dtfl_dd_build_fn build;
} reg_plugin;

/* One loaded instance: its dataflow and the inputs it registered. */
typedef struct loaded {
	char *instance;			/* owned instance name */
	dtfl_input_session **inputs;	/* inputs the build registered */
	size_t n_inputs;
	size_t dataflow_id;		/* for drop_dataflow on teardown */
	dtfl_probe *probe;		/* per-instance probe; tick waits on it */
} loaded;

struct dtfl_dd_server {
	dtfl_worker *worker;
	dtfl_time_class *outer_tc;	/* u64 RootTime */
	dtfl_trace_handler handler;	/* the shared TraceHandler */
	reg_plugin *plugins;
	size_t n_plugins, cap_plugins;
	loaded *instances;
	size_t n_instances, cap_instances;
	uint64_t epoch;			/* the current open epoch */
	uint64_t start_ns;		/* server start, for elapsed_ns */
	FILE *out;
};

/* Monotonic nanoseconds since an arbitrary epoch; the timer of lib.rs. */
static uint64_t
now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static reg_plugin *
find_plugin(dtfl_dd_server *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->n_plugins; i++)
		if (strcmp(s->plugins[i].name, name) == 0)
			return &s->plugins[i];
	return NULL;
}

static loaded *
find_instance(dtfl_dd_server *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->n_instances; i++)
		if (strcmp(s->instances[i].instance, name) == 0)
			return &s->instances[i];
	return NULL;
}

dtfl_dd_server *
dtfl_dd_server_new(FILE *out)
{
	dtfl_dd_server *s = __dtfl_calloc(1, sizeof *s);

	s->worker = dtfl_worker_new_standalone();
	s->outer_tc = dtfl_time_u64();
	s->epoch = 0;
	s->start_ns = now_ns();
	s->out = out != NULL ? out : stdout;
	return s;
}

void
dtfl_dd_server_free(dtfl_dd_server *s)
{
	size_t i, j;

	if (s == NULL)
		return;
	/* Drop every instance's dataflow, then its inputs and probe. */
	for (i = 0; i < s->n_instances; i++) {
		loaded *l = &s->instances[i];

		dtfl_worker_drop_dataflow(s->worker, l->dataflow_id);
		for (j = 0; j < l->n_inputs; j++)
			dtfl_input_session_close(l->inputs[j]);
		__dtfl_free(l->inputs);
		dtfl_probe_free(l->probe);
		__dtfl_free(l->instance);
	}
	__dtfl_free(s->instances);
	/* Step the worker until the dropped dataflows have drained. */
	while (dtfl_worker_step(s->worker))
		;
	for (i = 0; i < s->handler.n; i++) {
		__dtfl_free(s->handler.entries[i].name);
		dtfl_trace_free(s->handler.entries[i].trace);
		if (s->handler.entries[i].type != NULL)
			dtfl_type_free(s->handler.entries[i].type);
	}
	__dtfl_free(s->handler.entries);
	for (i = 0; i < s->n_plugins; i++)
		__dtfl_free(s->plugins[i].name);
	__dtfl_free(s->plugins);
	dtfl_worker_free(s->worker);
	dtfl_time_class_free(s->outer_tc);
	__dtfl_free(s);
}

uint64_t
dtfl_dd_server_epoch(const dtfl_dd_server *s)
{
	return s->epoch;
}

dtfl_trace_handler *
dtfl_dd_server_handles(dtfl_dd_server *s)
{
	return &s->handler;
}

void
dtfl_dd_server_register(dtfl_dd_server *s, const dtfl_dataflow_plugin *plugin)
{
	reg_plugin *rp;

	if (s == NULL || plugin == NULL || plugin->name == NULL)
		return;
	rp = find_plugin(s, plugin->name);
	if (rp != NULL) {
		rp->build = plugin->build;
		return;
	}
	if (s->n_plugins == s->cap_plugins) {
		s->cap_plugins = s->cap_plugins ? s->cap_plugins * 2 : 8;
		s->plugins = __dtfl_realloc(s->plugins,
		    s->cap_plugins * sizeof(reg_plugin));
	}
	rp = &s->plugins[s->n_plugins++];
	rp->name = __dtfl_strdup(plugin->name);
	rp->build = plugin->build;
}

/* Build context carried into the dataflow builder. */
typedef struct load_build {
	dtfl_dd_server *s;
	reg_plugin *plugin;
	dtfl_probe *probe;
	size_t argc;
	const char *const *argv;
	dtfl_dd_env env;	/* filled in by the builder, read back after */
	int err;
} load_build;

static void
load_build_fn(dtfl_scope *scope, void *arg)
{
	load_build *lb = arg;
	dtfl_dd_env *e = &lb->env;

	memset(e, 0, sizeof *e);
	e->scope = scope;
	e->handles = &lb->s->handler;
	e->probe = lb->probe;
	e->elapsed_ns = now_ns() - lb->s->start_ns;
	e->argc = lb->argc;
	e->argv = lb->argv;
	lb->err = lb->plugin->build(e);
}

int
dtfl_dd_server_load(dtfl_dd_server *s, const char *instance, const char *plugin,
    size_t argc, const char *const *argv)
{
	reg_plugin *rp;
	load_build lb;
	loaded *l;
	size_t dataflow_id;

	if (s == NULL || instance == NULL || plugin == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_dd_server_load: NULL argument");
	if (find_instance(s, instance) != NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "instance \"%s\" already loaded", instance);
	rp = find_plugin(s, plugin);
	if (rp == NULL)
		return __dtfl_err(DTFL_EINVAL, "no plugin \"%s\"", plugin);

	memset(&lb, 0, sizeof lb);
	lb.s = s;
	lb.plugin = rp;
	lb.probe = dtfl_probe_new();
	lb.argc = argc;
	lb.argv = argv;
	dataflow_id = dtfl_worker_next_dataflow_index(s->worker);
	dtfl_worker_dataflow(s->worker, instance, s->outer_tc, load_build_fn,
	    &lb);

	if (lb.err != 0) {
		size_t i;

		/* The build failed: tear the half-built dataflow down and
		 * close any inputs it registered. */
		dtfl_worker_drop_dataflow(s->worker, dataflow_id);
		for (i = 0; i < lb.env.n_inputs; i++)
			dtfl_input_session_close(lb.env.inputs[i]);
		__dtfl_free(lb.env.inputs);
		dtfl_probe_free(lb.probe);
		while (dtfl_worker_step(s->worker))
			;
		return lb.err;
	}

	/* Advance new inputs to the current epoch, so they are consistent
	 * with the traces they may already import. */
	{
		size_t i;

		for (i = 0; i < lb.env.n_inputs; i++) {
			dtfl_input_session_advance_to(lb.env.inputs[i],
			    &s->epoch);
			dtfl_input_session_flush(lb.env.inputs[i]);
		}
	}

	if (s->n_instances == s->cap_instances) {
		s->cap_instances = s->cap_instances ? s->cap_instances * 2 : 8;
		s->instances = __dtfl_realloc(s->instances,
		    s->cap_instances * sizeof(loaded));
	}
	l = &s->instances[s->n_instances++];
	l->instance = __dtfl_strdup(instance);
	l->inputs = lb.env.inputs;
	l->n_inputs = lb.env.n_inputs;
	l->dataflow_id = dataflow_id;
	l->probe = lb.probe;

	/* Run the fresh dataflow to the current epoch. */
	while (dtfl_probe_less_than(lb.probe, &s->epoch))
		dtfl_worker_step(s->worker);
	return 0;
}

/* A probe is behind if any live instance's probe is less than the frontier. */
static bool
any_instance_behind(dtfl_dd_server *s, uint64_t frontier)
{
	size_t i;

	for (i = 0; i < s->n_instances; i++)
		if (dtfl_probe_less_than(s->instances[i].probe, &frontier))
			return true;
	return false;
}

void
dtfl_dd_server_tick(dtfl_dd_server *s)
{
	uint64_t next = s->epoch + 1;
	size_t i, j;

	for (i = 0; i < s->n_instances; i++) {
		loaded *l = &s->instances[i];

		for (j = 0; j < l->n_inputs; j++) {
			dtfl_input_session_advance_to(l->inputs[j], &next);
			dtfl_input_session_flush(l->inputs[j]);
		}
	}
	s->epoch = next;
	while (any_instance_behind(s, s->epoch))
		dtfl_worker_step(s->worker);
}

int
dtfl_dd_server_drop(dtfl_dd_server *s, const char *instance)
{
	loaded *l;
	size_t idx, j;

	if (s == NULL || instance == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_dd_server_drop: NULL argument");
	l = find_instance(s, instance);
	if (l == NULL)
		return __dtfl_err(DTFL_EINVAL, "no instance \"%s\"", instance);
	idx = (size_t)(l - s->instances);

	dtfl_worker_drop_dataflow(s->worker, l->dataflow_id);
	for (j = 0; j < l->n_inputs; j++)
		dtfl_input_session_close(l->inputs[j]);
	__dtfl_free(l->inputs);
	dtfl_probe_free(l->probe);
	__dtfl_free(l->instance);
	s->instances[idx] = s->instances[--s->n_instances];

	/* Drain the dropped dataflow. */
	while (dtfl_worker_step(s->worker))
		if (!any_instance_behind(s, s->epoch))
			break;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Peek: read a shared trace's consolidated contents                   */
/* ------------------------------------------------------------------ */

/* One accumulated (record, diff) row of a peek snapshot. */
typedef struct peek_row {
	uint8_t *rec;
	size_t len;
	int64_t diff;
} peek_row;

typedef struct peek_acc {
	const dtfl_type *type;
	uint64_t epoch;
	peek_row *rows;
	size_t n, cap;
} peek_acc;

static void
peek_add(peek_acc *a, const void *rec, size_t len, int64_t diff)
{
	size_t i;

	for (i = 0; i < a->n; i++)
		if (a->rows[i].len == len &&
		    memcmp(a->rows[i].rec, rec, len) == 0) {
			a->rows[i].diff += diff;
			return;
		}
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 16;
		a->rows = __dtfl_realloc(a->rows, a->cap * sizeof(peek_row));
	}
	a->rows[a->n].rec = __dtfl_alloc(len ? len : 1);
	memcpy(a->rows[a->n].rec, rec, len);
	a->rows[a->n].len = len;
	a->rows[a->n].diff = diff;
	a->n++;
}

/* Inspect sink: accumulate updates at or before the current epoch. */
static void
peek_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	peek_acc *a = ctx;

	if (time[0] <= a->epoch)
		peek_add(a, rec, len, *(const int64_t *)diff);
}

typedef struct peek_build {
	dtfl_trace *trace;
	peek_acc *acc;
	dtfl_probe *probe;
} peek_build;

/* Reconstruct the stored record key || val.  An arrange_by_key trace stores
 * the record split into a key and a value; as_collection with a NULL fn
 * would emit only the key, so this fn concatenates both back into the
 * original fixed-width record (D1), which peek then formats by its type. */
static void
peek_kv(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    dtfl_arranged_emit *out)
{
	uint8_t buf[512];

	(void)ctx;
	if (klen + vlen > sizeof buf)
		return;
	if (klen)
		memcpy(buf, key, klen);
	if (vlen)
		memcpy(buf + klen, val, vlen);
	dtfl_arranged_emit_record(out, buf, klen + vlen);
}

static void
peek_build_fn(dtfl_scope *scope, void *arg)
{
	peek_build *pb = arg;
	dtfl_arranged *imported = dtfl_trace_import(pb->trace, scope,
	    "PeekImport");
	dtfl_collection *coll;
	dtfl_collection *ins;
	dtfl_collection *probed;

	/* peek_kv reconstructs key || val; as_collection's out_type only
	 * labels the stream, so the arrangement's own type suffices here.
	 * The accumulator formats with the registered record type, set by
	 * the caller from the handler entry. */
	coll = dtfl_arranged_as_collection(imported,
	    dtfl_arranged_type(imported), peek_kv, DTFL_NOCTX);
	ins = dtfl_collection_inspect(coll, peek_sink, DTFL_CTX(pb->acc, NULL));
	probed = dtfl_collection_probe_with(ins, pb->probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(coll);
	dtfl_arranged_free(imported);
}

static int
peek_cmp(const void *a, const void *b)
{
	const peek_row *ra = a, *rb = b;
	size_t n = ra->len < rb->len ? ra->len : rb->len;
	int c = n ? memcmp(ra->rec, rb->rec, n) : 0;

	if (c != 0)
		return c;
	if (ra->len != rb->len)
		return ra->len < rb->len ? -1 : 1;
	return 0;
}

int
dtfl_dd_server_peek(dtfl_dd_server *s, const char *trace)
{
	handler_entry *e;
	peek_acc acc;
	peek_build pb;
	size_t peek_id, i, kept = 0;

	if (s == NULL || trace == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_dd_server_peek: NULL argument");
	e = handler_find(&s->handler, trace);
	if (e == NULL)
		return __dtfl_err(DTFL_EINVAL, "no trace \"%s\"", trace);

	memset(&acc, 0, sizeof acc);
	acc.epoch = s->epoch;
	acc.type = e->type;	/* the registered record type, for formatting */

	pb.trace = e->trace;
	pb.acc = &acc;
	pb.probe = dtfl_probe_new();
	peek_id = dtfl_worker_next_dataflow_index(s->worker);
	dtfl_worker_dataflow(s->worker, "peek", s->outer_tc, peek_build_fn,
	    &pb);

	/*
	 * Drain the transient dataflow, then drop it.  A new listener on a
	 * derived trace seeds its frontier from the last data batch's upper
	 * (agent.c new_listener), which can lag the current epoch when the
	 * trace stopped changing before the epoch.  All of the trace's data is
	 * pushed to the listener at once, so stepping until the peek frontier
	 * reaches the epoch -- or stops advancing once some data has arrived --
	 * drains everything the trace holds.  The step cap bounds a stuck
	 * import so a hang is impossible (a hang here would be a bug).
	 */
	{
		size_t fn;
		const uint64_t *fr;
		uint64_t last = 0;
		bool have_last = false;
		unsigned stalls = 0;
		unsigned steps = 0;
		const unsigned cap = 100000;

		while (dtfl_probe_less_than(pb.probe, &s->epoch) &&
		    steps < cap) {
			dtfl_worker_step(s->worker);
			steps++;
			fr = dtfl_probe_frontier(pb.probe, &fn);
			if (fn == 0)
				break;	/* frontier empty: trace drained */
			if (!have_last || fr[0] > last) {
				last = fr[0];
				have_last = true;
				stalls = 0;
			} else if (++stalls >= 4) {
				/* The frontier has not moved in several steps:
				 * the import has replayed all it holds. */
				break;
			}
		}
	}
	dtfl_worker_drop_dataflow(s->worker, peek_id);
	while (dtfl_worker_step(s->worker))
		if (!any_instance_behind(s, s->epoch))
			break;
	dtfl_probe_free(pb.probe);

	/* Drop rows that cancelled, sort the survivors, print them. */
	for (i = 0; i < acc.n; i++)
		if (acc.rows[i].diff != 0)
			acc.rows[kept++] = acc.rows[i];
		else
			__dtfl_free(acc.rows[i].rec);
	acc.n = kept;
	if (acc.n > 1)
		qsort(acc.rows, acc.n, sizeof(peek_row), peek_cmp);

	fprintf(s->out, "peek \"%s\" @ epoch %" PRIu64 ":\n", trace, s->epoch);
	for (i = 0; i < acc.n; i++) {
		const uint8_t *r = acc.rows[i].rec;
		size_t len = acc.rows[i].len;

		fprintf(s->out, "  ");
		/* Format with the arrangement's own record type, which knows each
		 * field's signedness (an i64 field is stored sign-flipped, D1).
		 * P14 records are fixed-width 64-bit fields, so a type that does
		 * not format (opaque) falls back to a tuple of signed words. */
		if (acc.type != NULL &&
		    dtfl_type_value_len(acc.type, r, len) == len) {
			dtfl_type_fmt(acc.type, s->out, r, len);
		} else if (len == 0) {
			fprintf(s->out, "()");
		} else if (len % 8 == 0) {
			size_t w;

			fprintf(s->out, "(");
			for (w = 0; w < len / 8; w++)
				fprintf(s->out, "%s%" PRId64, w ? ", " : "",
				    (int64_t)dtfl_get_u64(r + 8 * w));
			fprintf(s->out, ")");
		} else {
			fprintf(s->out, "<%zu bytes>", len);
		}
		fprintf(s->out, " : %" PRId64 "\n", acc.rows[i].diff);
		__dtfl_free(acc.rows[i].rec);
	}
	__dtfl_free(acc.rows);
	return 0;
}

void
dtfl_dd_server_list(const dtfl_dd_server *s)
{
	size_t i;

	fprintf(s->out, "epoch %" PRIu64 "\n", s->epoch);
	fprintf(s->out, "traces (%zu):\n", s->handler.n);
	for (i = 0; i < s->handler.n; i++)
		fprintf(s->out, "  %s\n", s->handler.entries[i].name);
	fprintf(s->out, "instances (%zu):\n", s->n_instances);
	for (i = 0; i < s->n_instances; i++)
		fprintf(s->out, "  %s\n", s->instances[i].instance);
}
