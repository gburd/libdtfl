/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/graspan.c: mirrors differential-dataflow/examples/graspan.rs.
 *
 * Arguments: [query-file data-file] [timely flags].
 *
 * A query is a list of productions, one per line: "Target Rel1 Rel2 ..",
 * where a Rel starting with '~' is followed in reverse.  Target gains every
 * (x, y) with a path x -> z0 -> .. -> y through the named relations.  A
 * production with no relations just declares an input relation.  The data
 * file holds "src dst relation" lines.
 *
 * Every relation is an EdgeVariable in one iterative scope: a Variable
 * (dtfl_variable_new, step +1 on the loop coordinate) whose collection is
 * arranged forward and reverse by key before the Variable is set, so the
 * productions join against the relations they recursively define.  Each
 * relation's definition is distinct(source + productions).  A second
 * dataflow imports each relation's trace and prints its final size.
 *
 * With no positional arguments a built-in query and four-edge graph are
 * used: p is the transitive closure of e and s relates nodes with a
 * common predecessor.  Divergence: the elapsed-time column is omitted so
 * the output is deterministic.
 */
#define _POSIX_C_SOURCE 200809L	/* strtok_r */

#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The built-in query: e is an input; p = e+ (transitive closure);
 * s(x, y) if some z has edges z->x and z->y.  On the graph below
 * |e| = 4, |p| = 6, |s| = 5 (see graspan.expected).
 */
static const char builtin_query[] =
    "e\n"
    "p e\n"
    "p p e\n"
    "s ~e e\n";
static const char builtin_data[] =
    "0 1 e\n"
    "1 2 e\n"
    "2 3 e\n"
    "0 2 e\n";

/* shortcut: fixed caps on query size; raise them for larger queries. */
#define MAX_REL 32
#define MAX_PROD 64
#define MAX_RULE 16
#define MAX_NAME 64

typedef struct relation_ref {
	bool reverse;
	char name[MAX_NAME];
} relation_ref;

typedef struct production {
	char left_hand[MAX_NAME];
	relation_ref relations[MAX_RULE];
	size_t nrel;
} production;

typedef struct query {
	production prods[MAX_PROD];
	size_t nprod;
} query;

/* Production::from: whitespace-separated names, '~' marks Reverse. */
static int
query_parse(query *q, const char *text)
{
	const char *line = text;

	q->nprod = 0;
	while (*line != '\0') {
		const char *end = strchr(line, '\n');
		size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
		char buf[1024], *tok, *save;
		production *p;

		if (len >= sizeof buf || q->nprod == MAX_PROD)
			return -1;
		memcpy(buf, line, len);
		buf[len] = '\0';
		line += len + (end != NULL);
		tok = strtok_r(buf, " \t\r", &save);
		if (tok == NULL) {
			fprintf(stderr, "All rules must have a target.\n");
			return -1;
		}
		p = &q->prods[q->nprod++];
		snprintf(p->left_hand, MAX_NAME, "%s", tok);
		p->nrel = 0;
		while ((tok = strtok_r(NULL, " \t\r", &save)) != NULL) {
			relation_ref *r;

			if (p->nrel == MAX_RULE)
				return -1;
			r = &p->relations[p->nrel++];
			r->reverse = tok[0] == '~';
			snprintf(r->name, MAX_NAME, "%s", tok + r->reverse);
		}
	}
	return 0;
}

/* The {:?} of Query, as upstream prints it. */
static void
query_print(const query *q)
{
	size_t i, j;

	printf("Dataflow assembled for Query { productions: [");
	for (i = 0; i < q->nprod; i++) {
		const production *p = &q->prods[i];

		printf("%sProduction { left_hand: \"%s\", relations: [",
		    i == 0 ? "" : ", ", p->left_hand);
		for (j = 0; j < p->nrel; j++)
			printf("%s%s(\"%s\")", j == 0 ? "" : ", ",
			    p->relations[j].reverse ? "Reverse" : "Forward",
			    p->relations[j].name);
		printf("] }");
	}
	printf("] }\n");
}

/* ------------------------------------------------------------------ */
/* EdgeVariable                                                        */
/* ------------------------------------------------------------------ */

/*
 * An evolving set of edges, in the iterative subscope.  `collection` is
 * the Variable's collection (usable before the Variable is set); forward
 * and reverse are its arrangements, built on first use.  `current`
 * accumulates the source and the productions until complete() sets the
 * Variable to their distinct union.  All handles are freed by complete.
 */
typedef struct edge_variable {
	dtfl_variable *variable;
	dtfl_collection *collection;
	dtfl_collection *current;
	dtfl_arranged *forward;
	dtfl_arranged *reverse;
} edge_variable;

typedef struct relation {
	char name[MAX_NAME];
	dtfl_input_session *input;
	dtfl_trace *trace;
	edge_variable var;
} relation;

typedef struct worker_ctx {
	query q;
	relation rels[MAX_REL];	/* sorted by name, as upstream's BTreeMap */
	size_t nrel;
	const dtfl_type *edge;	/* (u64, u64) */
	dtfl_diff *i64;
} worker_ctx;

static relation *
find_relation(worker_ctx *w, const char *name)
{
	size_t i;

	for (i = 0; i < w->nrel; i++)
		if (strcmp(w->rels[i].name, name) == 0)
			return &w->rels[i];
	return NULL;
}

/* map(|(x, y)| (y, x)) */
static void
swap_edge(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 8);
	memcpy(buf + 8, rec, 8);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

static dtfl_arranged *
ev_forward(worker_ctx *w, edge_variable *ev)
{
	if (ev->forward == NULL)
		ev->forward = dtfl_collection_arrange_by_key(ev->collection,
		    dtfl_type_u64(), dtfl_type_u64(), "ArrangeByKey");
	(void)w;
	return ev->forward;
}

static dtfl_arranged *
ev_reverse(worker_ctx *w, edge_variable *ev)
{
	if (ev->reverse == NULL) {
		dtfl_collection *rev = dtfl_collection_map(ev->collection,
		    w->edge, swap_edge, DTFL_NOCTX);

		ev->reverse = dtfl_collection_arrange_by_key(rev,
		    dtfl_type_u64(), dtfl_type_u64(), "ArrangeByKey");
		dtfl_collection_free(rev);
	}
	return ev->reverse;
}

static void
ev_add_production(edge_variable *ev, dtfl_collection *prod)
{
	dtfl_collection *next = dtfl_collection_concat(ev->current, prod);

	dtfl_collection_free(ev->current);
	dtfl_collection_free(prod);
	ev->current = next;
}

static void
ev_complete(worker_ctx *w, edge_variable *ev)
{
	dtfl_collection *distinct = dtfl_collection_distinct(ev->current,
	    w->i64);

	dtfl_variable_set(ev->variable, distinct);
	dtfl_collection_free(ev->current);
	dtfl_collection_free(ev->collection);
	dtfl_arranged_free(ev->forward);
	dtfl_arranged_free(ev->reverse);
}

/* join_core(to_join, |_k, &x, &y| Some((y, x))) */
static void
join_step(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);
	memcpy(buf + 8, v1, 8);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* as_collection(|&dst, &src| (src, dst)) */
static void
untranspose(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)klen; (void)vlen;
	memcpy(buf, val, 8);
	memcpy(buf + 8, key, 8);
	dtfl_arranged_emit_record(out, buf, sizeof buf);
}

/* ------------------------------------------------------------------ */
/* Query::render_in                                                    */
/* ------------------------------------------------------------------ */

typedef struct render_ctx {
	worker_ctx *w;
	dtfl_collection **inputs;	/* outer scope, one per relation */
} render_ctx;

static void
render_iterative(dtfl_scope *sub, void *arg)
{
	render_ctx *rc = arg;
	worker_ctx *w = rc->w;
	size_t i, j;

	/* Create variables and result handles for each named relation. */
	for (i = 0; i < w->nrel; i++) {
		relation *r = &w->rels[i];
		edge_variable *ev = &r->var;
		dtfl_collection *left;
		dtfl_arranged *arr;

		ev->variable = dtfl_variable_new(sub, w->edge, w->i64,
		    &ev->collection);
		ev->current = dtfl_collection_enter(rc->inputs[i], sub);
		ev->forward = ev->reverse = NULL;
		left = dtfl_collection_leave(ev->collection);
		arr = dtfl_collection_arrange(left, "ArrangeBySelf");
		r->trace = dtfl_arranged_trace(arr);
		dtfl_arranged_free(arr);
		dtfl_collection_free(left);
	}

	/*
	 * For each rule, add to the productions for the relation.  Every
	 * name was checked against the relations before the build.
	 */
	for (i = 0; i < w->q.nprod; i++) {
		const production *p = &w->q.prods[i];
		const relation_ref *r0 = &p->relations[0];
		dtfl_arranged *transposed, *own = NULL;
		relation *r;

		/* We need to start somewhere; ignore empty rules. */
		if (p->nrel == 0)
			continue;
		/*
		 * Track the path transposed, indexed by destination.  The
		 * variable's cached arrangements stay owned by it; `own` is
		 * the latest intermediate arrangement.
		 */
		r = find_relation(w, r0->name);
		transposed = r0->reverse ? ev_forward(w, &r->var) :
		    ev_reverse(w, &r->var);
		for (j = 1; j < p->nrel; j++) {
			dtfl_arranged *to_join;
			dtfl_collection *joined;

			r = find_relation(w, p->relations[j].name);
			to_join = p->relations[j].reverse ?
			    ev_reverse(w, &r->var) : ev_forward(w, &r->var);
			joined = dtfl_arranged_join_core(transposed, to_join,
			    w->edge, join_step, DTFL_NOCTX);
			dtfl_arranged_free(own);
			own = dtfl_collection_arrange_by_key(joined,
			    dtfl_type_u64(), dtfl_type_u64(), "ArrangeByKey");
			dtfl_collection_free(joined);
			transposed = own;
		}
		/* Reverse the direction before adding it as a production. */
		ev_add_production(&find_relation(w, p->left_hand)->var,
		    dtfl_arranged_as_collection(transposed, w->edge,
		    untranspose, DTFL_NOCTX));
		dtfl_arranged_free(own);
	}

	for (i = 0; i < w->nrel; i++)
		ev_complete(w, &w->rels[i].var);
}

static void
render_in(dtfl_scope *scope, void *arg)
{
	worker_ctx *w = arg;
	dtfl_collection *inputs[MAX_REL];
	render_ctx rc = { w, inputs };
	size_t i;

	for (i = 0; i < w->nrel; i++) {
		w->rels[i].input = dtfl_input_session_new(w->edge, w->i64);
		inputs[i] = dtfl_input_session_to_collection(w->rels[i].input,
		    scope);
	}
	dtfl_scope_iterative(scope, "Iterative", render_iterative, &rc);
	for (i = 0; i < w->nrel; i++)
		dtfl_collection_free(inputs[i]);
}

/* as_collection(|&_kv, &()| ()) */
static void
to_unit(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    dtfl_arranged_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)val; (void)vlen;
	dtfl_arranged_emit_record(out, NULL, 0);
}

static void
print_size(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)rec; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("final size of relation '%s': %lld\n", (const char *)ctx,
	    (long long)d);
}

static void
build_report(dtfl_scope *scope, void *arg)
{
	worker_ctx *w = arg;
	size_t i;

	for (i = 0; i < w->nrel; i++) {
		dtfl_arranged *imp = dtfl_trace_import(w->rels[i].trace, scope,
		    "ImportRelation");
		dtfl_collection *units = dtfl_arranged_as_collection(imp,
		    dtfl_type_unit(), to_unit, DTFL_NOCTX);
		dtfl_collection *con = dtfl_collection_consolidate(units);
		dtfl_collection *ins = dtfl_collection_inspect(con, print_size,
		    DTFL_CTX(w->rels[i].name, NULL));

		dtfl_collection_free(ins);
		dtfl_collection_free(con);
		dtfl_collection_free(units);
		dtfl_arranged_free(imp);
	}
}

static int
cmp_rel(const void *a, const void *b)
{
	return strcmp(((const relation *)a)->name, ((const relation *)b)->name);
}

/* Inserts this worker's share of "src dst relation" lines. */
static int
load_data(worker_ctx *w, const char *text, size_t index, size_t peers)
{
	const char *line = text;

	while (*line != '\0') {
		const char *end = strchr(line, '\n');
		size_t len = end != NULL ? (size_t)(end - line) : strlen(line);
		unsigned long long src, dst;
		char name[MAX_NAME], buf[1024];

		if (len >= sizeof buf)
			return -1;
		memcpy(buf, line, len);
		buf[len] = '\0';
		line += len + (end != NULL);
		if (buf[0] == '#' || len == 0)
			continue;
		if (sscanf(buf, "%llu %llu %63s", &src, &dst, name) != 3) {
			fprintf(stderr, "malformed data line: %s\n", buf);
			return -1;
		}
		if (src % peers == index) {
			relation *r = find_relation(w, name);
			unsigned char rec[16];

			if (r == NULL) {
				fprintf(stderr,
				    "couldn't find the named relation: \"%s\"\n",
				    name);
				return -1;
			}
			dtfl_put_u64(rec, src);
			dtfl_put_u64(rec + 8, dst);
			dtfl_input_session_insert(r->input, rec, sizeof rec);
		}
	}
	return 0;
}

typedef struct main_ctx {
	const char *query_text;
	const char *data_text;
	int rc;
} main_ctx;

static void
worker(dtfl_worker *wk, void *vctx)
{
	main_ctx *mc = vctx;
	worker_ctx *w = calloc(1, sizeof *w);
	dtfl_time_class *unit = dtfl_time_unit();
	size_t i, j;

	w->edge = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	w->i64 = dtfl_diff_i64();
	if (query_parse(&w->q, mc->query_text) != 0) {
		fprintf(stderr, "malformed query\n");
		mc->rc = 1;
		goto out;
	}
	/* One relation per distinct left-hand name. */
	for (i = 0; i < w->q.nprod; i++) {
		if (find_relation(w, w->q.prods[i].left_hand) != NULL)
			continue;
		if (w->nrel == MAX_REL) {
			mc->rc = 1;
			goto out;
		}
		snprintf(w->rels[w->nrel++].name, MAX_NAME, "%s",
		    w->q.prods[i].left_hand);
	}
	qsort(w->rels, w->nrel, sizeof w->rels[0], cmp_rel);
	for (i = 0; i < w->q.nprod; i++)
		for (j = 0; j < w->q.prods[i].nrel; j++)
			if (find_relation(w, w->q.prods[i].relations[j].name) ==
			    NULL) {
				fprintf(stderr, "unknown relation: \"%s\"\n",
				    w->q.prods[i].relations[j].name);
				mc->rc = 1;
				goto out;
			}

	dtfl_worker_dataflow(wk, "graspan", unit, render_in, w);
	if (dtfl_worker_index(wk) == 0)
		query_print(&w->q);
	/* Build a dataflow to report final sizes. */
	dtfl_worker_dataflow(wk, "report", unit, build_report, w);

	if (load_data(w, mc->data_text, dtfl_worker_index(wk),
	    dtfl_worker_peers(wk)) != 0)
		mc->rc = 1;
	if (dtfl_worker_index(wk) == 0)
		printf("Data loaded\n");

	/* Dropping the handles ends the inputs and releases the traces. */
	for (i = 0; i < w->nrel; i++) {
		dtfl_input_session_close(w->rels[i].input);
		dtfl_trace_free(w->rels[i].trace);
	}
	while (dtfl_worker_step(wk))
		;
out:
	dtfl_time_class_free(unit);
	dtfl_diff_free(w->i64);
	dtfl_type_free(w->edge);
	free(w);
}

/* Reads a whole file into a NUL-terminated buffer the caller frees. */
static char *
read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf = NULL;
	size_t n = 0, cap = 0, got;

	if (f == NULL)
		return NULL;
	do {
		if (n + 4096 + 1 > cap) {
			char *nb = realloc(buf, cap = (n + 4096) * 2 + 1);

			if (nb == NULL) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = nb;
		}
		got = fread(buf + n, 1, 4096, f);
		n += got;
	} while (got > 0);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

int
main(int argc, char **argv)
{
	main_ctx mc = { builtin_query, builtin_data, 0 };
	char *qtext = NULL, *dtext = NULL;
	int skip = 0, rc;
	dtfl_config *cfg;

	if (argc > 2 && argv[1][0] != '-') {
		qtext = read_file(argv[1]);
		dtext = read_file(argv[2]);
		if (qtext == NULL || dtext == NULL) {
			fprintf(stderr, "Failed to read query or data file\n");
			free(qtext);
			free(dtext);
			return 1;
		}
		mc.query_text = qtext;
		mc.data_text = dtext;
		skip = 2;
	}
	cfg = ex_config(argc, argv, skip);
	rc = dtfl_execute(cfg, worker, &mc, NULL);
	dtfl_config_free(cfg);
	free(qtext);
	free(dtext);
	return rc != 0 ? rc : mc.rc;
}
