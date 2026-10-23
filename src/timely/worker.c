/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/worker.c: the worker, which steps installed dataflows.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/worker.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A worker owns its allocator, activations, log registry and dataflows,
 * and belongs to one thread.  step_or_park follows upstream exactly:
 * surface channel events as activations, organize activations, then
 * either park (if nothing is active and a non-zero wait is allowed) or
 * step every active dataflow, removing completed ones, and finally flush
 * logs and release the allocator.
 *
 * Channel paths: upstream keeps HashMap<channel id, path>.  Channel ids
 * are dense small integers handed out by this worker, so a vector
 * indexed by id holds them (a NULL entry means "no path").
 */
#include "base/probes_int.h"
#include "timely/worker_int.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct path_entry {
	size_t *path;	/* owned; NULL if unused */
	size_t len;
} path_entry;

/* worker.rs Wrapper: one installed dataflow. */
struct __dtfl_dataflow {
	size_t index;
	size_t identifier;
	__dtfl_schedule *operate;	/* NULL once complete */
	__dtfl_shared_progress *progress;
	void *resources;
	void (*resources_free)(void *);
	__dtfl_vec channel_ids;		/* size_t */
	bool logging;
};

static uint64_t
real_clock(void *ctx)
{
	(void)ctx;
	return __dtfl_clock_ns();
}

__dtfl_worker *
__dtfl_worker_new(__dtfl_allocator *a, dtfl_progress_mode mode, bool timer,
    uint64_t (*clock)(void *), void *clock_ctx)
{
	__dtfl_worker *w = __dtfl_calloc(1, sizeof *w);

	if (clock == NULL)
		clock = real_clock;
	w->allocator = a;
	w->progress_mode = mode;
	__dtfl_activations_init(&w->activations, timer, clock, clock_ctx);
	w->activations.sync = __dtfl_sync_activations_new(a->parker);
	/* Upstream creates the log registry only with a timer (Some(now)). */
	w->log = timer ? __dtfl_log_registry_new(clock, clock_ctx) : NULL;
	__dtfl_vec_init(&w->dataflows, sizeof(__dtfl_dataflow *));
	__dtfl_vec_init(&w->paths, sizeof(path_entry));
	__dtfl_vec_init(&w->temp_channel_ids, sizeof(size_t));
	__dtfl_vec_init(&w->active_dataflows, sizeof(size_t));
	w->owner = __dtfl_thread_self();
	return w;
}

static void
path_remove(__dtfl_worker *w, size_t channel)
{
	path_entry *p;

	if (channel >= w->paths.len)
		return;
	p = DTFL_VEC_AT(&w->paths, path_entry, channel);
	__dtfl_free(p->path);
	p->path = NULL;
	p->len = 0;
}

/*
 * Drops a dataflow: logs Shutdown, then drops the operator before the
 * resources ("ensure drop order"), then forgets its channels' paths.
 */
static void
dataflow_drop(__dtfl_worker *w, __dtfl_dataflow *df)
{
	size_t i;

	if (df->logging)
		__dtfl_worker_log_shutdown(w, df->identifier);
	if (df->operate != NULL) {
		df->operate->ops->destroy(df->operate);
		df->operate = NULL;
	}
	__dtfl_shared_progress_free(df->progress);
	df->progress = NULL;
	if (df->resources_free != NULL)
		df->resources_free(df->resources);
	for (i = 0; i < df->channel_ids.len; i++)
		path_remove(w, *DTFL_VEC_AT(&df->channel_ids, size_t, i));
	__dtfl_vec_free(&df->channel_ids);
	__dtfl_free(df);
}

void
__dtfl_worker_free(__dtfl_worker *w)
{
	size_t i;

	if (w == NULL)
		return;
	for (i = 0; i < w->dataflows.len; i++)
		dataflow_drop(w, *DTFL_VEC_AT(&w->dataflows, __dtfl_dataflow *, i));
	__dtfl_vec_free(&w->dataflows);
	for (i = 0; i < w->paths.len; i++)
		__dtfl_free(DTFL_VEC_AT(&w->paths, path_entry, i)->path);
	__dtfl_vec_free(&w->paths);
	__dtfl_vec_free(&w->temp_channel_ids);
	__dtfl_vec_free(&w->active_dataflows);
	__dtfl_log_registry_free(w->log);
	__dtfl_activations_free(&w->activations);
	__dtfl_allocator_destroy(w->allocator);
	__dtfl_free(w);
}

size_t
__dtfl_worker_new_identifier(__dtfl_worker *w)
{
	return w->identifiers++;
}

size_t
__dtfl_worker_peek_identifier(const __dtfl_worker *w)
{
	return w->identifiers;
}

__dtfl_logger *
__dtfl_worker_logger(__dtfl_worker *w, const char *name)
{
	return w->log != NULL ? __dtfl_log_registry_get(w->log, name) : NULL;
}

/* Records channel -> path, for routing channel events to activations. */
static void
record_path(__dtfl_worker *w, size_t identifier, const size_t *path, size_t len)
{
	path_entry *p;

	if (len == 0)
		__dtfl_panic("Unacceptable address: Length zero");
	while (w->paths.len <= identifier)
		__dtfl_vec_push(&w->paths, NULL);
	p = DTFL_VEC_AT(&w->paths, path_entry, identifier);
	__dtfl_free(p->path);
	p->path = __dtfl_memdup(path, len * sizeof(size_t));
	p->len = len;
	__dtfl_vec_push(&w->temp_channel_ids, &identifier);
}

void
__dtfl_worker_allocate(__dtfl_worker *w, size_t identifier, const size_t *path,
    size_t len, const __dtfl_msg_class *mc, __dtfl_push **pushers,
    __dtfl_pull **puller)
{
	DTFL_WORKER_CHECK(w);
	record_path(w, identifier, path, len);
	__dtfl_allocate(w->allocator, identifier, mc, pushers, puller);
}

void
__dtfl_worker_pipeline(__dtfl_worker *w, size_t identifier, const size_t *path,
    size_t len, const __dtfl_msg_class *mc, __dtfl_push **pusher,
    __dtfl_pull **puller)
{
	DTFL_WORKER_CHECK(w);
	record_path(w, identifier, path, len);
	__dtfl_pipeline(w->allocator, identifier, mc, pusher, puller);
}

void
__dtfl_worker_broadcast(__dtfl_worker *w, size_t identifier, const size_t *path,
    size_t len, const __dtfl_msg_class *mc, __dtfl_push **pusher,
    __dtfl_pull **puller)
{
	DTFL_WORKER_CHECK(w);
	record_path(w, identifier, path, len);
	__dtfl_broadcast(w->allocator, identifier, mc, pusher, puller);
}

size_t
__dtfl_worker_allocate_dataflow_index(__dtfl_worker *w)
{
	return w->dataflow_counter++;
}

/* Index of dataflow `index` in w->dataflows (kept sorted), or the insertion point. */
static size_t
dataflow_find(const __dtfl_worker *w, size_t index, bool *found)
{
	size_t lo = 0, hi = w->dataflows.len;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		size_t mi = (*DTFL_VEC_AT(&w->dataflows, __dtfl_dataflow *, mid))->index;

		if (mi < index)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = lo < w->dataflows.len &&
	    (*DTFL_VEC_AT(&w->dataflows, __dtfl_dataflow *, lo))->index == index;
	return lo;
}

/*
 * worker.rs dataflow_core, after the build closure: logs OperatesEvent
 * for the dataflow, initializes it, and wraps it with the channels
 * allocated since the previous install.
 */
void
__dtfl_worker_install_dataflow(__dtfl_worker *w, size_t index, size_t identifier,
    __dtfl_operate *op, void *resources, void (*resources_free)(void *))
{
	__dtfl_dataflow *df = __dtfl_calloc(1, sizeof *df);
	__dtfl_connectivity conn;
	__dtfl_logger *l = __dtfl_worker_logger(w, "timely");
	size_t at;
	bool found;

	df->logging = l != NULL;
	df->index = index;
	df->identifier = identifier;
	df->resources = resources;
	df->resources_free = resources_free;
	df->operate = op->ops->initialize(op, &conn, &df->progress);
	__dtfl_connectivity_free(&conn);
	if (l != NULL) {
		__dtfl_worker_log_operates(w, identifier, df->operate->path,
		    df->operate->path_len, df->operate->name);
		__dtfl_logger_flush(l);
		__dtfl_logger_release(l);
	}
	__dtfl_vec_init(&df->channel_ids, sizeof(size_t));
	__dtfl_vec_swap(&df->channel_ids, &w->temp_channel_ids);
	at = dataflow_find(w, index, &found);
	DTFL_ASSERT(!found);
	__dtfl_vec_push(&w->dataflows, NULL);
	memmove((__dtfl_dataflow **)(void *)w->dataflows.data + at + 1,
	    (__dtfl_dataflow **)(void *)w->dataflows.data + at,
	    (w->dataflows.len - at - 1) * sizeof(__dtfl_dataflow *));
	((__dtfl_dataflow **)(void *)w->dataflows.data)[at] = df;
}

void
__dtfl_worker_drop_dataflow(__dtfl_worker *w, size_t index)
{
	bool found;
	size_t at = dataflow_find(w, index, &found);

	DTFL_WORKER_CHECK(w);
	if (!found)
		return;
	dataflow_drop(w, *DTFL_VEC_AT(&w->dataflows, __dtfl_dataflow *, at));
	__dtfl_vec_remove(&w->dataflows, at);
}

/*
 * Wrapper::step: runs the dataflow's root; a dataflow that reports
 * complete drops its operator and resources.
 */
static bool
dataflow_step(__dtfl_worker *w, __dtfl_dataflow *df)
{
	bool incomplete;

	if (df->logging)
		__dtfl_worker_log_schedule(w, df->identifier, true);
	incomplete = df->operate != NULL && df->operate->ops->schedule(df->operate);
	if (!incomplete && df->operate != NULL) {
		df->operate->ops->destroy(df->operate);
		df->operate = NULL;
		if (df->resources_free != NULL)
			df->resources_free(df->resources);
		df->resources = NULL;
		df->resources_free = NULL;
	}
	if (df->logging)
		__dtfl_worker_log_schedule(w, df->identifier, false);
	return incomplete;
}

static void
collect_active(void *ctx, size_t ext)
{
	__dtfl_vec_push((__dtfl_vec *)ctx, &ext);
}

static int
size_cmp(const void *a, const void *b)
{
	size_t x = *(const size_t *)a, y = *(const size_t *)b;

	return x < y ? -1 : x > y;
}

/* Worker::step_or_park. */
bool
__dtfl_worker_step_or_park(__dtfl_worker *w, uint64_t timeout_ns)
{
	uint64_t empty_for, delay;
	size_t i, k, n;
	__dtfl_vec *ev;

	DTFL_WORKER_CHECK(w);
	if (w->stepping)
		__dtfl_panic("dtfl_worker_step called from inside an operator");
	w->stepping = 1;

	/* Process channel events; activate responders. */
	__dtfl_allocator_receive(w->allocator);
	ev = &w->allocator->events.ids;
	if (ev->len > 1)
		qsort(ev->data, ev->len, sizeof(size_t), size_cmp);
	for (i = 0; i < ev->len; i++) {
		size_t ch = *DTFL_VEC_AT(ev, size_t, i);
		const path_entry *p;

		if (i > 0 && ch == *DTFL_VEC_AT(ev, size_t, i - 1))
			continue;
		if (ch >= w->paths.len)
			continue;
		p = DTFL_VEC_AT(&w->paths, path_entry, ch);
		if (p->path != NULL)
			__dtfl_activations_activate(&w->activations, p->path, p->len);
	}
	__dtfl_vec_clear(ev);

	/* Organize activations. */
	__dtfl_activations_advance(&w->activations);

	/* Park only with no pending events and a non-zero allowed wait. */
	empty_for = __dtfl_activations_empty_for(&w->activations);
	delay = DTFL_MIN(timeout_ns, empty_for);
	if (delay != 0) {
		__dtfl_logger *l = __dtfl_worker_logger(w, "timely");

		if (l != NULL) {
			__dtfl_worker_log_park(w, true, delay);
			__dtfl_logger_flush(l);
		}
		DTFL_PROBE3(park, (uint64_t)w->allocator->index,
		    (uint64_t)delay, (const void *)w->allocator->parker);
		__dtfl_allocator_await_events(w->allocator, delay);
		DTFL_PROBE2(park_end, (uint64_t)w->allocator->index,
		    (const void *)w->allocator->parker);
		if (l != NULL) {
			__dtfl_worker_log_park(w, false, 0);
			__dtfl_logger_release(l);
		}
	} else {
		/* Schedule active dataflows. */
		__dtfl_vec_clear(&w->active_dataflows);
		__dtfl_activations_for_extensions(&w->activations, NULL, 0,
		    collect_active, &w->active_dataflows);
		n = w->active_dataflows.len;
		for (k = 0; k < n; k++) {
			size_t index = *DTFL_VEC_AT(&w->active_dataflows, size_t, k);
			bool found;
			size_t at = dataflow_find(w, index, &found);
			__dtfl_dataflow *df;

			if (!found)
				continue;
			df = *DTFL_VEC_AT(&w->dataflows, __dtfl_dataflow *, at);
			if (!dataflow_step(w, df)) {
				dataflow_drop(w, df);
				__dtfl_vec_remove(&w->dataflows, at);
			}
		}
	}

	if (w->log != NULL)
		__dtfl_log_registry_flush(w->log);
	__dtfl_allocator_release(w->allocator);
	w->stepping = 0;
	return w->dataflows.len > 0;
}
