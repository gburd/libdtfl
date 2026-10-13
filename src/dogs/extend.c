/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/extend.c: CollectionIndex, the prefix extender, extend_using,
 * and the generic-join driver (ProposeExtensionMethod::extend).
 *
 * CollectionIndex::index builds three shared traces from a (key, value)
 * collection: a count trace (key -> unit whose accumulated diff is the
 * number of distinct extensions, from distinct().map(key).arrange_by_self),
 * a propose trace (key -> value, from arrange_by_key), and a validate trace
 * ((key, value) -> unit, from arrange_by_self).  An extender pairs an index
 * with a key selector that reads the join key out of a prefix.
 *
 * extend runs the generic join: with one extender it is a plain propose;
 * with several it annotates each prefix with each extender's offered count,
 * routes each prefix to the extender offering the fewest (the worst-case-
 * optimal choice), proposes from that one, validates against the rest, and
 * concatenates the per-extender results.
 */
#include "dogs/dogs_int.h"
#include "dd/common_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"
#include "dtfl/reduce.h"
#include "dtfl/record.h"
#include "timely/timely_int.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* CollectionIndex                                                     */
/* ------------------------------------------------------------------ */

struct dtfl_collection_index {
	dtfl_trace *count_trace;	/* key -> unit, distinct-count diff */
	dtfl_trace *propose_trace;	/* key -> value */
	dtfl_trace *validate_trace;	/* (key, value) -> unit */
	const dtfl_type *kt;		/* key type (retained) */
	const dtfl_type *vt;		/* value type (retained) */
	const dtfl_type *kvt;		/* key||value pair type (retained) */
};

/* map: strip the value, keeping the key prefix (distinct (K,V) -> K). */
typedef struct strip_val_ctx {
	const dtfl_type *kt;
	const dtfl_type *rt;	/* the key||value record type */
} strip_val_ctx;

static void
strip_to_key(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	strip_val_ctx *c = vctx;
	size_t klen = dtfl_type_value_len(c->kt, rec, len);

	if (klen == SIZE_MAX || klen > len)
		klen = len;
	dtfl_collection_emit_record(out, rec, klen);
}

dtfl_collection_index *
dtfl_collection_index_new(dtfl_collection *c, const dtfl_type *kt,
    const dtfl_type *vt)
{
	dtfl_collection_index *idx = __dtfl_alloc(sizeof *idx);
	const dtfl_type *kvt = dtfl_type_pair(kt, vt);
	dtfl_diff *isize = dtfl_diff_i64();
	dtfl_arranged *by_self = dtfl_collection_arrange(c, "ArrangeBySelf");
	dtfl_arranged *by_key = __dtfl_collection_arrange_by_key(c, kt, vt,
	    "ArrangeByKey");
	/* count: distinct (k,v), map to k, arrange_by_self. */
	dtfl_collection *distinct = dtfl_collection_distinct(c, isize);
	strip_val_ctx *sc = __dtfl_alloc(sizeof *sc);
	dtfl_collection *keys;
	dtfl_arranged *count_arr;

	sc->kt = kt;
	sc->rt = kvt;
	keys = dtfl_collection_map(distinct, kt, strip_to_key,
	    DTFL_CTX(sc, __dtfl_free));
	count_arr = dtfl_collection_arrange(keys, "ArrangeCount");

	idx->count_trace = dtfl_arranged_trace(count_arr);
	idx->propose_trace = dtfl_arranged_trace(by_key);
	idx->validate_trace = dtfl_arranged_trace(by_self);
	idx->kt = dtfl_type_retain(kt);
	idx->vt = dtfl_type_retain(vt);
	idx->kvt = kvt;	/* dtfl_type_pair returns a retained reference */

	dtfl_arranged_free(count_arr);
	dtfl_arranged_free(by_key);
	dtfl_arranged_free(by_self);
	dtfl_collection_free(keys);
	dtfl_collection_free(distinct);
	dtfl_diff_free(isize);
	return idx;
}

void
dtfl_collection_index_free(dtfl_collection_index *idx)
{
	if (idx == NULL)
		return;
	dtfl_trace_free(idx->count_trace);
	dtfl_trace_free(idx->propose_trace);
	dtfl_trace_free(idx->validate_trace);
	dtfl_type_free(idx->kt);
	dtfl_type_free(idx->vt);
	dtfl_type_free(idx->kvt);
	__dtfl_free(idx);
}

/* ------------------------------------------------------------------ */
/* The prefix extender                                                 */
/* ------------------------------------------------------------------ */

struct dtfl_prefix_extender {
	dtfl_trace *count_trace;	/* retained from the index */
	dtfl_trace *propose_trace;	/* retained */
	dtfl_trace *validate_trace;	/* retained */
	const dtfl_type *kt;		/* retained */
	const dtfl_type *vt;		/* retained */
	const dtfl_type *prefix_type;	/* retained */
	dtfl_key_selector_fn key;
	void *user;
	void (*user_free)(void *);
};

dtfl_prefix_extender *
dtfl_collection_index_extend_using(const dtfl_collection_index *idx,
    const dtfl_type *prefix_type, dtfl_key_selector_fn key, dtfl_ctx ctx)
{
	dtfl_prefix_extender *pe = __dtfl_alloc(sizeof *pe);

	pe->count_trace = __dtfl_trace_clone(idx->count_trace);
	pe->propose_trace = __dtfl_trace_clone(idx->propose_trace);
	pe->validate_trace = __dtfl_trace_clone(idx->validate_trace);
	pe->kt = dtfl_type_retain(idx->kt);
	pe->vt = dtfl_type_retain(idx->vt);
	pe->prefix_type = dtfl_type_retain(prefix_type);
	pe->key = key;
	pe->user = ctx.ptr;
	pe->user_free = ctx.free;
	return pe;
}

void
dtfl_prefix_extender_free(dtfl_prefix_extender *pe)
{
	if (pe == NULL)
		return;
	dtfl_trace_free(pe->count_trace);
	dtfl_trace_free(pe->propose_trace);
	dtfl_trace_free(pe->validate_trace);
	dtfl_type_free(pe->kt);
	dtfl_type_free(pe->vt);
	dtfl_type_free(pe->prefix_type);
	if (pe->user_free != NULL)
		pe->user_free(pe->user);
	__dtfl_free(pe);
}

/* ------------------------------------------------------------------ */
/* count/propose/validate wrappers                                     */
/* ------------------------------------------------------------------ */

/*
 * count/propose/validate: import the relevant trace, then run the operator.
 * The operators capture the user key function and context by value, not the
 * extender, so they keep working after the extender handle is freed (as
 * upstream clones the key selector into each operator).  The user context
 * must outlive the dataflow, as upstream's `'static` bound requires.
 */
static dtfl_collection *
pe_count(dtfl_prefix_extender *pe, dtfl_collection *prefixes,
    const dtfl_type *prefix_type, uint64_t index)
{
	dtfl_scope *scope = dtfl_stream_scope(prefixes->stream);
	dtfl_arranged *arr = dtfl_trace_import(pe->count_trace, scope,
	    "CountImport");
	dtfl_collection *out = dtfl_dogs_count(prefixes, arr, prefix_type,
	    pe->key, DTFL_CTX(pe->user, NULL), index);

	dtfl_arranged_free(arr);
	return out;
}

static dtfl_collection *
pe_propose(dtfl_prefix_extender *pe, dtfl_collection *prefixes,
    const dtfl_type *out_type)
{
	dtfl_scope *scope = dtfl_stream_scope(prefixes->stream);
	dtfl_arranged *arr = dtfl_trace_import(pe->propose_trace, scope,
	    "ProposeImport");
	dtfl_collection *out = dtfl_dogs_propose(prefixes, arr, out_type,
	    pe->key, DTFL_CTX(pe->user, NULL));

	dtfl_arranged_free(arr);
	return out;
}

static dtfl_collection *
pe_validate(dtfl_prefix_extender *pe, dtfl_collection *extensions,
    const dtfl_type *out_type, const dtfl_type *prefix_type)
{
	dtfl_scope *scope = dtfl_stream_scope(extensions->stream);
	dtfl_arranged *arr = dtfl_trace_import(pe->validate_trace, scope,
	    "ValidateImport");
	dtfl_collection *out = dtfl_dogs_validate(extensions, arr, out_type,
	    prefix_type, pe->vt, pe->key, DTFL_CTX(pe->user, NULL));

	dtfl_arranged_free(arr);
	return out;
}

/* ------------------------------------------------------------------ */
/* The generic join driver (ProposeExtensionMethod::extend)            */
/* ------------------------------------------------------------------ */

/* map: prefix p -> p || count(2^31) || index(0) (the initial triple). */
typedef struct init_triple_ctx {
	size_t plen_fixed;	/* 0 if variable */
} init_triple_ctx;

static void
init_triple(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[DTFL_DOGS_KEY_MAX];

	(void)vctx;
	if (len + 2 * sizeof(uint64_t) > sizeof buf)
		return;
	if (len != 0)
		memcpy(buf, rec, len);
	dtfl_put_u64(buf + len, UINT64_C(1) << 31);
	dtfl_put_u64(buf + len + sizeof(uint64_t), 0);
	dtfl_collection_emit_record(out, buf, len + 2 * sizeof(uint64_t));
}

/* The partition route: the index field (last u64 of the record part). */
typedef struct part_route_ctx {
	unsigned depth;
	size_t dw;
} part_route_ctx;

static size_t
part_route(void *vctx, const void *rec, size_t len)
{
	part_route_ctx *c = vctx;
	size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);

	if (rlen < sizeof(uint64_t))
		return 0;
	return (size_t)dtfl_get_u64((const unsigned char *)rec + rlen -
	    sizeof(uint64_t));
}

/* map: strip the count||index triple suffix, keeping the bare prefix. */
typedef struct strip_triple_ctx {
	int unused;
} strip_triple_ctx;

static void
strip_triple(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)vctx;
	if (len < 2 * sizeof(uint64_t)) {
		dtfl_collection_emit_record(out, rec, len);
		return;
	}
	dtfl_collection_emit_record(out, rec, len - 2 * sizeof(uint64_t));
}

dtfl_collection *
dtfl_collection_extend(dtfl_collection *prefixes,
    dtfl_prefix_extender *const *extenders, size_t n, const dtfl_type *out_type)
{
	dtfl_scope *scope =
	    dtfl_stream_scope(prefixes->stream);
	const dtfl_type *prefix_type = prefixes->type;
	unsigned depth = prefixes->depth;
	size_t dw = prefixes->diff->cls->width;
	dtfl_collection *counts;
	const dtfl_type *triple_type;
	const dtfl_type *triple_fields[3];
	dtfl_collection **results;
	dtfl_collection *out;
	size_t i;

	if (n == 1)
		return pe_propose(extenders[0], prefixes, out_type);

	/* counts = prefixes.map(|p| (p, 1<<31, 0)). */
	triple_fields[0] = prefix_type;
	triple_fields[1] = dtfl_type_u64();
	triple_fields[2] = dtfl_type_u64();
	triple_type = dtfl_type_tuple(triple_fields, 3);
	counts = dtfl_collection_map(prefixes, triple_type, init_triple,
	    DTFL_NOCTX);

	/* for (index, extender): counts = extender.count(counts, index). */
	for (i = 0; i < n; i++) {
		dtfl_collection *next = pe_count(extenders[i], counts,
		    prefix_type, (uint64_t)i);

		dtfl_collection_free(counts);
		counts = next;
	}

	/* Partition by the chosen index into n sub-streams. */
	{
		part_route_ctx *rc = __dtfl_alloc(sizeof *rc);
		dtfl_stream **parts = __dtfl_alloc(n * sizeof *parts);

		rc->depth = depth;
		rc->dw = dw;
		dtfl_stream_partition(counts->stream, n,
		    part_route, DTFL_CTX(rc, __dtfl_free), parts);

		results = __dtfl_alloc(n * sizeof *results);
		for (i = 0; i < n; i++) {
			dtfl_collection *nom = __dtfl_as_collection(parts[i],
			    triple_type,
			    dtfl_diff_retain(counts->diff),
			    depth);
			dtfl_collection *bare = dtfl_collection_map(nom,
			    prefix_type, strip_triple, DTFL_NOCTX);
			dtfl_collection *ext = pe_propose(extenders[i], bare,
			    out_type);
			size_t j;

			for (j = 0; j < n; j++) {
				if (j != i) {
					dtfl_collection *v = pe_validate(
					    extenders[j], ext, out_type,
					    prefix_type);

					dtfl_collection_free(ext);
					ext = v;
				}
			}
			results[i] = ext;
			dtfl_collection_free(bare);
			dtfl_collection_free(nom);
		}
		__dtfl_free(parts);
	}

	/* Concatenate the per-extender results. */
	{
		dtfl_collection **srcs = __dtfl_alloc(n * sizeof *srcs);

		for (i = 0; i < n; i++)
			srcs[i] = results[i];
		out = dtfl_collection_concatenate(scope, srcs, n);
		__dtfl_free(srcs);
	}

	for (i = 0; i < n; i++)
		dtfl_collection_free(results[i]);
	__dtfl_free(results);
	dtfl_collection_free(counts);
	dtfl_type_free(triple_type);
	return out;
}
