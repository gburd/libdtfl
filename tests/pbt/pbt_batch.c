/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_batch.c: __dtfl_batch against an array model.
 *
 * A random sequence of push, push_from, copy, retain + make_mut and
 * clear runs on two batch handles that may share storage, while a plain
 * array model tracks what each handle must contain.  Every step checks
 * that writing through one handle never changes the other, and the end
 * state round-trips through encode/decode.
 */
#include "pbt.h"
#include "base/batch_int.h"

#define MAXR 64

typedef struct mrec {
	unsigned char b[8];
	size_t len;
	uint64_t t;
	uint32_t d;
} mrec;

typedef struct model {
	mrec r[MAXR];
	size_t n;
} model;

static bool
matches(const __dtfl_batch *b, const model *m)
{
	size_t i, len;
	const void *p;
	uint32_t d;

	if (__dtfl_batch_len(b) != m->n)
		return false;
	for (i = 0; i < m->n; i++) {
		p = __dtfl_batch_rec(b, i, &len);
		memcpy(&d, __dtfl_batch_diff(b, i), 4);
		if (len != m->r[i].len || (len != 0 && memcmp(p, m->r[i].b, len) != 0) ||
		    __dtfl_batch_time(b, i)[0] != m->r[i].t || d != m->r[i].d)
			return false;
	}
	return true;
}

static void
prop_batch_model(pbt_tc *tc)
{
	static model m[2];
	static struct { int op, h, x; mrec r; } ops[100];
	__dtfl_batch *b[2];
	size_t nops = (size_t)pbt_int(tc, 0, 100), k, i;
	bool ok = true;

	/* Draw everything first so an overrun cannot leak batches. */
	for (k = 0; k < nops; k++) {
		ops[k].op = (int)pbt_int(tc, 0, 5);
		ops[k].h = (int)pbt_int(tc, 0, 1);
		ops[k].x = (int)pbt_int(tc, 0, MAXR - 1);
		ops[k].r.len = (size_t)pbt_int(tc, 0, 8);
		for (i = 0; i < ops[k].r.len; i++)
			ops[k].r.b[i] = (unsigned char)pbt_int(tc, 0, 255);
		ops[k].r.t = (uint64_t)pbt_int(tc, 0, 1000);
		ops[k].r.d = (uint32_t)pbt_int(tc, 0, 1000);
	}
	b[0] = __dtfl_batch_new(1, 4);
	b[1] = __dtfl_batch_retain(b[0]);
	m[0].n = m[1].n = 0;
	for (k = 0; k < nops && ok; k++) {
		int h = ops[k].h, o = 1 - h;
		mrec *r = &ops[k].r;

		switch (ops[k].op) {
		case 0: case 1:		/* push to h, copy-on-write first */
			if (m[h].n == MAXR)
				break;
			b[h] = __dtfl_batch_make_mut(b[h]);
			__dtfl_batch_push(b[h], r->b, r->len, &r->t, &r->d);
			m[h].r[m[h].n++] = *r;
			break;
		case 2:			/* push_from other handle, or itself */
			if (m[h].n == MAXR)
				break;
			if (ops[k].x % 2 == 0)
				o = h;
			if (m[o].n == 0)
				break;
			i = (size_t)ops[k].x % m[o].n;
			b[h] = __dtfl_batch_make_mut(b[h]);
			/* make_mut may have detached h from o; read from the current b[o]. */
			__dtfl_batch_push_from(b[h], b[o], i);
			m[h].r[m[h].n] = m[o].r[i];
			m[h].n++;
			break;
		case 3:			/* h becomes a shared reference to o */
			__dtfl_batch_release(b[h]);
			b[h] = __dtfl_batch_retain(b[o]);
			m[h] = m[o];
			break;
		case 4:			/* h becomes a deep copy of o */
			__dtfl_batch_release(b[h]);
			b[h] = __dtfl_batch_copy(b[o]);
			m[h] = m[o];
			break;
		default:		/* clear h */
			b[h] = __dtfl_batch_make_mut(b[h]);
			__dtfl_batch_clear(b[h]);
			m[h].n = 0;
			break;
		}
		ok = matches(b[0], &m[0]) && matches(b[1], &m[1]);
	}
	if (ok) {
		size_t n = __dtfl_batch_encoded_len(b[0]);
		unsigned char *buf = __dtfl_alloc(n);
		__dtfl_batch *d;

		__dtfl_batch_encode(b[0], buf);
		d = __dtfl_batch_decode(buf, n);
		ok = d != NULL && matches(d, &m[0]);
		__dtfl_batch_release(d);
		__dtfl_free(buf);
	}
	__dtfl_batch_release(b[0]);
	__dtfl_batch_release(b[1]);
	PBT_ASSERT(ok);
}

static const pbt_prop props[] = {
	{ "batch_model", prop_batch_model, 500 },
};

PBT_MAIN("batch", props)
