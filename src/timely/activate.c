/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/activate.c: operator activations.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/scheduling/activate.rs
 *   timely/src/scheduling/mod.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Activations is upstream's (offset, length) bounds into a slice
 * buffer.  The first `clean` bounds are sorted and distinct (advance()
 * establishes that); later ones are pending.  for_extensions() only
 * consults the clean prefix, as upstream.
 *
 * SyncActivations is the one structure here shared across threads: a
 * mutex-protected list of paths plus the worker's parker.  Other threads
 * push and unpark; the worker drains in advance().  The mutex orders
 * every push before the drain that observes it.
 */
#include "timely/worker_int.h"

typedef struct bound {
	size_t off, len;
} bound;

/* A delayed activation: deadline on the activation clock, owned path. */
typedef struct timer_heap_entry {
	uint64_t deadline;
	size_t len;
	size_t *path;
} timer_heap_entry;

struct __dtfl_sync_activations {
	__dtfl_arc ref;
	pthread_mutex_t lock;
	__dtfl_vec pending;	/* size_t words: len, then path */
	bool closed;
	__dtfl_parker *parker;
};

/* ------------------------------------------------------------------ */
/* SyncActivations                                                     */
/* ------------------------------------------------------------------ */

__dtfl_sync_activations *
__dtfl_sync_activations_new(__dtfl_parker *parker)
{
	__dtfl_sync_activations *s = __dtfl_calloc(1, sizeof *s);

	__dtfl_arc_init(&s->ref);
	pthread_mutex_init(&s->lock, NULL);
	__dtfl_vec_init(&s->pending, sizeof(size_t));
	s->parker = parker != NULL ? __dtfl_parker_retain(parker) : NULL;
	return s;
}

__dtfl_sync_activations *
__dtfl_sync_activations_retain(__dtfl_sync_activations *s)
{
	__dtfl_arc_retain(&s->ref);
	return s;
}

void
__dtfl_sync_activations_release(__dtfl_sync_activations *s)
{
	if (s == NULL || !__dtfl_arc_release(&s->ref))
		return;
	pthread_mutex_destroy(&s->lock);
	__dtfl_vec_free(&s->pending);
	if (s->parker != NULL)
		__dtfl_parker_release(s->parker);
	__dtfl_free(s);
}

/*
 * SyncActivations::activate: queues the path and unparks the worker.
 * Fails once the worker has closed the queue (the receiver was dropped).
 */
int
__dtfl_sync_activations_push(__dtfl_sync_activations *s, const size_t *path,
    size_t len)
{
	pthread_mutex_lock(&s->lock);
	if (s->closed) {
		pthread_mutex_unlock(&s->lock);
		return __dtfl_err(DTFL_ESTATE, "worker is gone");
	}
	__dtfl_vec_push(&s->pending, &len);
	__dtfl_vec_push_n(&s->pending, path, len);
	pthread_mutex_unlock(&s->lock);
	if (s->parker != NULL)
		__dtfl_parker_unpark(s->parker);
	return DTFL_OK;
}

void
__dtfl_sync_activations_close(__dtfl_sync_activations *s)
{
	pthread_mutex_lock(&s->lock);
	s->closed = true;
	__dtfl_vec_clear(&s->pending);
	pthread_mutex_unlock(&s->lock);
}

/* ------------------------------------------------------------------ */
/* Activations                                                         */
/* ------------------------------------------------------------------ */

static uint64_t
real_clock(void *ctx)
{
	(void)ctx;
	return __dtfl_clock_ns();
}

void
__dtfl_activations_init(__dtfl_activations *a, bool has_timer,
    uint64_t (*clock)(void *), void *clock_ctx)
{
	memset(a, 0, sizeof *a);
	__dtfl_vec_init(&a->bounds, sizeof(bound));
	__dtfl_vec_init(&a->slices, sizeof(size_t));
	__dtfl_vec_init(&a->buffer, sizeof(size_t));
	__dtfl_vec_init(&a->timers, sizeof(timer_heap_entry));
	a->has_timer = has_timer;
	a->clock = clock != NULL ? clock : real_clock;
	a->clock_ctx = clock_ctx;
	a->timer_start_ns = a->clock(a->clock_ctx);
}

void
__dtfl_activations_free(__dtfl_activations *a)
{
	size_t i;

	for (i = 0; i < a->timers.len; i++)
		__dtfl_free(DTFL_VEC_AT(&a->timers, timer_heap_entry, i)->path);
	__dtfl_vec_free(&a->bounds);
	__dtfl_vec_free(&a->slices);
	__dtfl_vec_free(&a->buffer);
	__dtfl_vec_free(&a->timers);
	if (a->sync != NULL) {
		__dtfl_sync_activations_close(a->sync);
		__dtfl_sync_activations_release(a->sync);
		a->sync = NULL;
	}
}

uint64_t
__dtfl_activations_elapsed(const __dtfl_activations *a)
{
	return a->clock(a->clock_ctx) - a->timer_start_ns;
}

/* Activations::activate: marks a path (and so its prefixes) active. */
void
__dtfl_activations_activate(__dtfl_activations *a, const size_t *path, size_t len)
{
	bound b;

	b.off = a->slices.len;
	b.len = len;
	__dtfl_vec_push(&a->bounds, &b);
	__dtfl_vec_push_n(&a->slices, path, len);
}

/* Timer min-heap on deadline; each entry owns a copy of its path. */

static void
timer_push(__dtfl_activations *a, uint64_t deadline, const size_t *path, size_t len)
{
	timer_heap_entry e, *h;
	size_t i;

	e.deadline = deadline;
	e.len = len;
	e.path = __dtfl_memdup(path, len * sizeof(size_t));
	__dtfl_vec_push(&a->timers, &e);
	h = (timer_heap_entry *)(void *)a->timers.data;
	for (i = a->timers.len - 1; i > 0;) {
		size_t parent = (i - 1) / 2;
		timer_heap_entry tmp;

		if (h[parent].deadline <= h[i].deadline)
			break;
		tmp = h[parent];
		h[parent] = h[i];
		h[i] = tmp;
		i = parent;
	}
}

static void
timer_pop(__dtfl_activations *a, timer_heap_entry *out)
{
	timer_heap_entry *h = (timer_heap_entry *)(void *)a->timers.data;
	size_t n = a->timers.len - 1, i = 0;

	*out = h[0];
	h[0] = h[n];
	__dtfl_vec_truncate(&a->timers, n);
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;
		timer_heap_entry tmp;

		if (l < n && h[l].deadline < h[m].deadline)
			m = l;
		if (r < n && h[r].deadline < h[m].deadline)
			m = r;
		if (m == i)
			break;
		tmp = h[m];
		h[m] = h[i];
		h[i] = tmp;
		i = m;
	}
}

/*
 * Activations::activate_after: with a timer, schedules the path `delay`
 * from now; without one (or a zero delay), activates immediately.
 */
void
__dtfl_activations_activate_after(__dtfl_activations *a, const size_t *path,
    size_t len, uint64_t delay_ns)
{
	if (a->has_timer && delay_ns != 0)
		timer_push(a, __dtfl_activations_elapsed(a) + delay_ns, path, len);
	else
		__dtfl_activations_activate(a, path, len);
}

/* Compares the paths of two bounds lexicographically, as Rust slices. */
static int
bound_cmp(const size_t *slices, const bound *x, const bound *y)
{
	size_t i, n = DTFL_MIN(x->len, y->len);

	for (i = 0; i < n; i++) {
		size_t a = slices[x->off + i], b = slices[y->off + i];

		if (a != b)
			return a < b ? -1 : 1;
	}
	return x->len < y->len ? -1 : x->len > y->len;
}

static void
sort_bounds(bound *v, size_t n, const size_t *slices)
{
	size_t i, j;

	/* Insertion sort: activation lists are short and mostly sorted. */
	for (i = 1; i < n; i++) {
		bound x = v[i];

		for (j = i; j > 0 && bound_cmp(slices, &v[j - 1], &x) > 0; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
}

/*
 * Activations::advance: absorbs sync and expired timer activations,
 * drops the previously clean (already served) bounds, sorts and dedups
 * the rest, and compacts the slices.
 */
void
__dtfl_activations_advance(__dtfl_activations *a)
{
	bound *b;
	size_t i, k, n;

	if (a->sync != NULL) {
		__dtfl_vec drained;

		__dtfl_vec_init(&drained, sizeof(size_t));
		pthread_mutex_lock(&a->sync->lock);
		__dtfl_vec_swap(&drained, &a->sync->pending);
		pthread_mutex_unlock(&a->sync->lock);
		for (i = 0; i < drained.len;) {
			size_t len = *DTFL_VEC_AT(&drained, size_t, i);

			__dtfl_activations_activate(a,
			    (const size_t *)(void *)drained.data + i + 1, len);
			i += 1 + len;
		}
		__dtfl_vec_free(&drained);
	}
	if (a->has_timer && a->timers.len > 0) {
		uint64_t now = __dtfl_activations_elapsed(a);

		while (a->timers.len > 0 &&
		    ((timer_heap_entry *)(void *)a->timers.data)[0].deadline <= now) {
			timer_heap_entry e;

			timer_pop(a, &e);
			__dtfl_activations_activate(a, e.path, e.len);
			__dtfl_free(e.path);
		}
	}

	/* bounds.drain(..clean) */
	n = a->bounds.len - a->clean;
	memmove(a->bounds.data, a->bounds.data + a->clean * sizeof(bound),
	    n * sizeof(bound));
	__dtfl_vec_truncate(&a->bounds, n);
	b = (bound *)(void *)a->bounds.data;
	sort_bounds(b, n, (const size_t *)(void *)a->slices.data);
	/* dedup by path */
	k = 0;
	for (i = 0; i < n; i++) {
		if (k > 0 && bound_cmp((const size_t *)(void *)a->slices.data, &b[k - 1], &b[i]) == 0)
			continue;
		b[k++] = b[i];
	}
	__dtfl_vec_truncate(&a->bounds, k);
	/* Compact the slices. */
	__dtfl_vec_clear(&a->buffer);
	for (i = 0; i < k; i++) {
		size_t off = a->buffer.len;

		__dtfl_vec_push_n(&a->buffer, (const size_t *)(void *)a->slices.data + b[i].off,
		    b[i].len);
		b[i].off = off;
	}
	__dtfl_vec_swap(&a->buffer, &a->slices);
	a->clean = k;
}

/*
 * Activations::for_extensions: for every clean active path extending
 * `path`, calls action with the next element, skipping duplicates.
 */
void
__dtfl_activations_for_extensions(const __dtfl_activations *a,
    const size_t *path, size_t len, void (*action)(void *ctx, size_t ext),
    void *ctx)
{
	const bound *b = (const bound *)(void *)a->bounds.data;
	const size_t *sl = (const size_t *)(void *)a->slices.data;
	size_t lo = 0, hi = a->clean, i;
	bool have_prev = false;
	size_t prev = 0;

	/* Binary search the clean prefix for the first path >= `path`. */
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2, j, n = DTFL_MIN(b[mid].len, len);
		int c = 0;

		for (j = 0; j < n && c == 0; j++)
			if (sl[b[mid].off + j] != path[j])
				c = sl[b[mid].off + j] < path[j] ? -1 : 1;
		if (c == 0)
			c = b[mid].len < len ? -1 : b[mid].len > len;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	for (i = lo; i < a->bounds.len; i++) {
		size_t j;

		if (b[i].len < len)
			break;
		for (j = 0; j < len; j++)
			if (sl[b[i].off + j] != path[j])
				break;
		if (j < len)
			break;	/* no longer starts_with(path) */
		if (b[i].len > len) {
			size_t ext = sl[b[i].off + len];

			if (!have_prev || prev != ext) {
				action(ctx, ext);
				prev = ext;
				have_prev = true;
			}
		}
	}
}

/*
 * Activations::empty_for: 0 if something is active or there is no
 * timer; otherwise the time until the next timer, or UINT64_MAX if none.
 */
uint64_t
__dtfl_activations_empty_for(const __dtfl_activations *a)
{
	uint64_t now, next;

	if (a->bounds.len > 0 || !a->has_timer)
		return 0;
	if (a->timers.len == 0)
		return UINT64_MAX;
	now = __dtfl_activations_elapsed(a);
	next = ((const timer_heap_entry *)(void *)a->timers.data)[0].deadline;
	return next < now ? 0 : next - now;
}

/* ------------------------------------------------------------------ */
/* Public activators (handles.rs / activate.rs)                        */
/* ------------------------------------------------------------------ */

struct dtfl_activator {
	__dtfl_worker *w;
	size_t *path;
	size_t len;
};

struct dtfl_sync_activator {
	__dtfl_sync_activations *sync;	/* retained */
	size_t *path;
	size_t len;
};

dtfl_activator *
__dtfl_activator_new(__dtfl_worker *w, const size_t *path, size_t len)
{
	dtfl_activator *a = __dtfl_alloc(sizeof *a);

	a->w = w;
	a->path = __dtfl_memdup(path, len * sizeof(size_t));
	a->len = len;
	return a;
}

void
dtfl_activator_activate(dtfl_activator *a)
{
	DTFL_WORKER_CHECK(a->w);
	__dtfl_activations_activate(&a->w->activations, a->path, a->len);
}

void
dtfl_activator_activate_after(dtfl_activator *a, uint64_t delay_ns)
{
	DTFL_WORKER_CHECK(a->w);
	__dtfl_activations_activate_after(&a->w->activations, a->path, a->len, delay_ns);
}

void
dtfl_activator_free(dtfl_activator *a)
{
	if (a == NULL)
		return;
	__dtfl_free(a->path);
	__dtfl_free(a);
}

dtfl_sync_activator *
__dtfl_sync_activator_new(__dtfl_worker *w, const size_t *path, size_t len)
{
	dtfl_sync_activator *a = __dtfl_alloc(sizeof *a);

	a->sync = __dtfl_sync_activations_retain(w->activations.sync);
	a->path = __dtfl_memdup(path, len * sizeof(size_t));
	a->len = len;
	return a;
}

int
dtfl_sync_activator_activate(dtfl_sync_activator *a)
{
	return __dtfl_sync_activations_push(a->sync, a->path, a->len);
}

void
dtfl_sync_activator_free(dtfl_sync_activator *a)
{
	if (a == NULL)
		return;
	__dtfl_sync_activations_release(a->sync);
	__dtfl_free(a->path);
	__dtfl_free(a);
}
