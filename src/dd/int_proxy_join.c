/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/int_proxy/join.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/int_proxy_join.c: the proxy join framework.  A conventional
 * differential join against (u64, u64) values, which are provided by and
 * then interpreted by a backend, who is relieved of lattice-time reasoning.
 *
 * join_prep and join_key are ported literally.  ProxyJoinTactic::prep
 * returns an iterator of output containers; here the unit's containers are
 * the batches the backend's cross pushes into the deferred unit's output
 * builder (src/dd/join.c), which the join driver ships under the unit's
 * capability with the usual fuel accounting.
 *
 * __dtfl_proxy_join_u64_backend is not upstream (upstream ships the
 * ProxyJoinBackend trait and no implementation): both arrangements have
 * 8-byte order-preserving keys and values, presented as (hash, id) =
 * (key, value), and cross runs the join result closure per product.
 */
#include "dd/int_proxy_int.h"
#include "dd/trace/trace_int.h"
#include "dtfl/record.h"
#include "progress/time_int.h"

/*
 * Update count threshold used to return to the backend with join output to
 * instantiate.
 */
#define JOIN_CHUNK ((size_t)1 << 20)

/* The matched products of one unit, buffered until a flush. */
typedef struct join_out {
	__dtfl_vec li;		/* __dtfl_proxy_id */
	__dtfl_vec ri;		/* __dtfl_proxy_id */
	__dtfl_vec ot;		/* __dtfl_time_val */
	__dtfl_vec od;		/* rout width bytes */
	__dtfl_proxy_join_backend *backend;
	const __dtfl_join_instance *instance;
	__dtfl_consolidator *out;
	uint64_t kh;		/* the key of the wave in progress */
	unsigned tw;
} join_out;

/* Ships the accumulated batch as a container and resets the buffers. */
static void
flush(join_out *o)
{
	o->backend->ops->cross(o->backend->self, o->instance,
	    (const __dtfl_proxy_id *)(void *)o->li.data,
	    (const __dtfl_proxy_id *)(void *)o->ri.data,
	    (const __dtfl_time_val *)(void *)o->ot.data, o->od.data,
	    o->li.len, o->out);
	__dtfl_vec_clear(&o->li);
	__dtfl_vec_clear(&o->ri);
	__dtfl_vec_clear(&o->ot);
	__dtfl_vec_clear(&o->od);
}

/* Push one product, flushing at each JOIN_CHUNK boundary. */
static void
push_product(join_out *o, uint64_t v0, uint64_t v1, const uint64_t *t,
    const void *d)
{
	__dtfl_proxy_id l = { o->kh, v0 }, r = { o->kh, v1 };
	__dtfl_time_val tv;

	memset(&tv, 0, sizeof tv);
	if (o->tw != 0)
		memcpy(tv.t, t, (size_t)o->tw * sizeof(uint64_t));
	__dtfl_vec_push(&o->li, &l);
	__dtfl_vec_push(&o->ri, &r);
	__dtfl_vec_push(&o->ot, &tv);
	__dtfl_vec_push(&o->od, d);
	if (o->li.len >= JOIN_CHUNK)
		flush(o);
}

/* bilinear_wave's emit: the values are ids as 8-byte u64 encodings. */
static void
wave_emit(void *vctx, const void *v0, size_t v0len, const void *v1,
    size_t v1len, const uint64_t *time, const void *d0, const void *d1)
{
	join_out *o = vctx;
	unsigned char d[DTFL_COMMON_DIFF_MAX];
	const __dtfl_diff_class *oc;

	(void)v0len; (void)v1len;
	if (__dtfl_diff_multiply(o->backend->r0, o->backend->r1, d0, d1, d,
	    &oc) != DTFL_OK)
		__dtfl_panic("int_proxy join: no multiply for %s * %s",
		    o->backend->r0->name, o->backend->r1->name);
	push_product(o, dtfl_get_u64(v0), dtfl_get_u64(v1), time, d);
}

/*
 * Match one key's records across the two presented runs.  If either history
 * is small, this performs a simple cross product.  If both histories are
 * large, this replays the histories compacting as it goes in order to
 * (potentially) avoid quadratic blow-up.  The flush is checked after every
 * produced pair, including inside the replay wave, so even a key whose
 * fanout dwarfs JOIN_CHUNK never materializes more than a chunk of its cross
 * product.
 */
static void
join_key(uint64_t kh, const __dtfl_bridge_upd *p0, size_t a0, size_t e0,
    const __dtfl_bridge_upd *p1, size_t a1, size_t e1,
    __dtfl_value_history *h0, __dtfl_value_history *h1, join_out *o)
{
	const dtfl_time_class *tc = h0->tc;
	size_t a, b;

	o->kh = kh;
	if (e0 - a0 < 16 || e1 - a1 < 16) {
		for (a = a0; a < e0; a++)
			for (b = a1; b < e1; b++) {
				uint64_t t[DTFL_TIME_MAX] = { 0 };
				unsigned char d[DTFL_COMMON_DIFF_MAX];
				const __dtfl_diff_class *oc;

				if (o->tw != 0)
					dtfl_time_join(tc, p0[a].time,
					    p1[b].time, t);
				if (__dtfl_diff_multiply(o->backend->r0,
				    o->backend->r1, p0[a].diff, p1[b].diff, d,
				    &oc) != DTFL_OK)
					__dtfl_panic("int_proxy join: no "
					    "multiply for %s * %s",
					    o->backend->r0->name,
					    o->backend->r1->name);
				push_product(o, p0[a].id, p1[b].id, t, d);
			}
		return;
	}

	/*
	 * Reusable replay scratch, reloaded per key (load_iter clears and
	 * rebuilds, keeping capacity).
	 */
	__dtfl_value_history_load_iter(h0, p0 + a0, e0 - a0, NULL);
	__dtfl_value_history_load_iter(h1, p1 + a1, e1 - a1, NULL);
	__dtfl_bilinear_wave(h0, h1, wave_emit, o);
}

static const __dtfl_bridge_upd *
bridge_at(const __dtfl_vec *v, size_t i)
{
	return &((const __dtfl_bridge_upd *)(const void *)v->data)[i];
}

/* The deduplicated key hashes of a sorted bridge. */
static void
bridge_keys(const __dtfl_vec *p, __dtfl_vec *keys)
{
	size_t i;

	for (i = 0; i < p->len; i++) {
		uint64_t k = bridge_at(p, i)->hash;

		if (keys->len == 0 || ((const uint64_t *)(const void *)
		    keys->data)[keys->len - 1] != k)
			__dtfl_vec_push(keys, &k);
	}
}

/*
 * Prepare one join unit: present both sides, merge-match key runs over the
 * sorted hashes, and cross-product matched records into a sequence of output
 * containers, one per ~JOIN_CHUNK matches, flushed wherever the batch fills
 * (including mid-key).  meet is a lower bound on the fresh side's times, so
 * the accumulated side loads compacted; every output is produced under the
 * capability at meet, so advancing loaded times by it leaves the output
 * unchanged.
 */
void
__dtfl_proxy_join_prep(__dtfl_proxy_join_backend *backend,
    __dtfl_batch_ord *const *input0, size_t n0,
    __dtfl_batch_ord *const *input1, size_t n1, bool fresh_is_input0,
    const uint64_t *meet, unsigned tw, __dtfl_consolidator *out)
{
	const __dtfl_proxy_join_backend_ops *ops = backend->ops;
	__dtfl_join_instance instance;
	__dtfl_vec p0, p1, keys;
	__dtfl_value_history h0, h1;
	join_out o;
	size_t i = 0, j = 0;
	const dtfl_time_class *tc;

	instance.batches0 = input0;
	instance.n0 = n0;
	instance.batches1 = input1;
	instance.n1 = n1;
	instance.lower = meet;

	__dtfl_vec_init(&p0, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&p1, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&keys, sizeof(uint64_t));

	/*
	 * Prepare work using the keys of the just-received side.  FIXME
	 * (upstream): use the smaller of the two instead.
	 */
	if (fresh_is_input0) {
		ops->present0(backend->self, &instance, NULL, 0, &p0);
		if (p0.len != 0) {
			bridge_keys(&p0, &keys);
			ops->present1(backend->self, &instance,
			    (const uint64_t *)(void *)keys.data, keys.len,
			    &p1);
		}
	} else {
		ops->present1(backend->self, &instance, NULL, 0, &p1);
		if (p1.len != 0) {
			bridge_keys(&p1, &keys);
			ops->present0(backend->self, &instance,
			    (const uint64_t *)(void *)keys.data, keys.len,
			    &p0);
		}
	}
	if (p0.len == 0 || p1.len == 0)
		goto done;
	__dtfl_bridge_assert_sorted(&p0, tw, "present0");
	__dtfl_bridge_assert_sorted(&p1, tw, "present1");

	/*
	 * Merge-join the two presented runs on key_hash (both sorted) and
	 * cross matched pairs.  Per-key replay histories are held across the
	 * unit and reloaded per key (only the >=16/>=16 replay-wave path
	 * touches them).
	 */
	tc = (*(n0 != 0 ? input0 : input1))->tc;
	__dtfl_value_history_init(&h0, tc, backend->r0);
	__dtfl_value_history_init(&h1, tc, backend->r1);
	__dtfl_vec_init(&o.li, sizeof(__dtfl_proxy_id));
	__dtfl_vec_init(&o.ri, sizeof(__dtfl_proxy_id));
	__dtfl_vec_init(&o.ot, sizeof(__dtfl_time_val));
	__dtfl_vec_init(&o.od, DTFL_MAX(backend->rout->width, 1u));
	o.backend = backend;
	o.instance = &instance;
	o.out = out;
	o.kh = 0;
	o.tw = tw;
	while (i < p0.len && j < p1.len) {
		uint64_t ki = bridge_at(&p0, i)->hash;
		uint64_t kj = bridge_at(&p1, j)->hash;

		if (ki < kj)
			i++;
		else if (kj < ki)
			j++;
		else {
			size_t e0 = i, e1 = j;

			while (e0 < p0.len && bridge_at(&p0, e0)->hash == ki)
				e0++;
			while (e1 < p1.len && bridge_at(&p1, e1)->hash == ki)
				e1++;
			join_key(ki, bridge_at(&p0, 0), i, e0,
			    bridge_at(&p1, 0), j, e1, &h0, &h1, &o);
			i = e0;
			j = e1;
		}
	}
	if (o.li.len != 0)
		flush(&o);
	__dtfl_vec_free(&o.li);
	__dtfl_vec_free(&o.ri);
	__dtfl_vec_free(&o.ot);
	__dtfl_vec_free(&o.od);
	__dtfl_value_history_free(&h0);
	__dtfl_value_history_free(&h1);
done:
	__dtfl_vec_free(&keys);
	__dtfl_vec_free(&p1);
	__dtfl_vec_free(&p0);
}

/* ------------------------------------------------------------------ */
/* The u64 backend (not upstream; see the file comment).               */
/* ------------------------------------------------------------------ */

typedef struct u64_join_backend {
	dtfl_join_result_fn result;
	void *ctx;
	void (*ctx_free)(void *);
	const __dtfl_diff_class *r0;
	const __dtfl_diff_class *r1;
	unsigned tw;
} u64_join_backend;

/*
 * Present one side: the fresh side unchanged (its times lie at or beyond
 * lower), the accumulated side advanced by lower (the JoinInstance lower:
 * the compaction frontier for loading).  The fresh side is the one asked
 * for with no filter.
 */
static void
u64_present(const u64_join_backend *b, __dtfl_batch_ord *const *batches,
    size_t n, const __dtfl_diff_class *dc, const __dtfl_join_instance *inst,
    const uint64_t *filter, size_t nfilter, __dtfl_vec *out)
{
	__dtfl_proxy_present_u64(batches, n, dc, b->tw, filter, nfilter,
	    filter != NULL ? inst->lower : NULL, out);
	__dtfl_bridge_consolidate(out, b->tw, dc);
}

static void
u64_present0(void *self, const __dtfl_join_instance *inst,
    const uint64_t *filter, size_t nfilter, __dtfl_vec *out)
{
	u64_join_backend *b = self;

	u64_present(b, inst->batches0, inst->n0, b->r0, inst, filter, nfilter,
	    out);
}

static void
u64_present1(void *self, const __dtfl_join_instance *inst,
    const uint64_t *filter, size_t nfilter, __dtfl_vec *out)
{
	u64_join_backend *b = self;

	u64_present(b, inst->batches1, inst->n1, b->r1, inst, filter, nfilter,
	    out);
}

static void
u64_cross(void *self, const __dtfl_join_instance *inst,
    const __dtfl_proxy_id *left, const __dtfl_proxy_id *right,
    const __dtfl_time_val *times, const void *diffs, size_t n,
    __dtfl_consolidator *out)
{
	u64_join_backend *b = self;
	const __dtfl_diff_class *oc = __dtfl_diff_multiply_class(b->r0, b->r1);
	size_t i;

	(void)inst;
	for (i = 0; i < n; i++) {
		unsigned char kb[8], v0[8], v1[8];

		dtfl_put_u64(kb, left[i].hash);
		dtfl_put_u64(v0, left[i].id);
		dtfl_put_u64(v1, right[i].id);
		__dtfl_join_result_emit(b->result, b->ctx, kb, sizeof kb, v0,
		    sizeof v0, v1, sizeof v1, times[i].t,
		    (const unsigned char *)diffs + i * DTFL_MAX(oc->width, 1u),
		    out);
	}
}

static void
u64_join_free(void *self)
{
	u64_join_backend *b = self;

	if (b->ctx_free != NULL)
		b->ctx_free(b->ctx);
	__dtfl_free(b);
}

static const __dtfl_proxy_join_backend_ops u64_join_ops = {
	u64_present0, u64_present1, u64_cross, u64_join_free
};

__dtfl_proxy_join_backend
__dtfl_proxy_join_u64_backend(const struct dtfl_arranged *a0,
    const struct dtfl_arranged *a1, dtfl_join_result_fn result, void *ctx,
    void (*ctx_free)(void *))
{
	const __dtfl_trace_agent *g0 = __dtfl_arranged_agent(a0);
	const __dtfl_trace_agent *g1 = __dtfl_arranged_agent(a1);
	u64_join_backend *b = __dtfl_alloc(sizeof *b);
	__dtfl_proxy_join_backend be;

	b->result = result;
	b->ctx = ctx;
	b->ctx_free = ctx_free;
	b->r0 = g0->dc;
	b->r1 = g1->dc;
	b->tw = dtfl_time_depth(g0->tc);
	be.ops = &u64_join_ops;
	be.self = b;
	be.r0 = g0->dc;
	be.r1 = g1->dc;
	be.rout = __dtfl_diff_multiply_class(g0->dc, g1->dc);
	return be;
}
