/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/log_registry.c: the logging registry.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   logging/src/lib.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A registry is a small name -> logger map (linear scan; registries hold
 * a handful of names).  A logger buffers events in an arena and hands
 * them to its action when the arena reaches DTFL_BUFFER_SIZE_BYTES, on
 * flush, and when dropped, mirroring LoggerInner::log_many and
 * Flush::flush.  Ownership: the registry holds one reference to each
 * bound logger; __dtfl_log_registry_get returns another.  A logger's
 * context is released when its last reference goes away.
 */
#include "base/log_int.h"
#include "base/batch_int.h"

struct __dtfl_logger {
	size_t refs;			/* worker-local; no atomics needed */
	uint64_t start_ns;		/* registry start instant */
	__dtfl_clock_fn clock;
	void *clock_ctx;
	__dtfl_log_action action;
	void *ctx;
	void (*ctx_free)(void *);
	__dtfl_vec arena;		/* event bytes */
	__dtfl_vec events;		/* __dtfl_log_event with data as offset */
	bool in_action;			/* guards against reentrant logging */
};

typedef struct registry_entry {
	char *name;
	__dtfl_logger *logger;
} registry_entry;

struct __dtfl_log_registry {
	uint64_t start_ns;
	__dtfl_clock_fn clock;
	void *clock_ctx;
	__dtfl_vec entries;	/* registry_entry */
};

static uint64_t
default_clock(void *ctx)
{
	(void)ctx;
	return __dtfl_clock_ns();
}

/*
 * Creates an empty registry whose loggers stamp events relative to now.
 * A NULL clock means the monotonic clock.
 */
__dtfl_log_registry *
__dtfl_log_registry_new(__dtfl_clock_fn clock, void *clock_ctx)
{
	__dtfl_log_registry *r = __dtfl_calloc(1, sizeof *r);

	r->clock = clock != NULL ? clock : default_clock;
	r->clock_ctx = clock_ctx;
	r->start_ns = r->clock(r->clock_ctx);
	__dtfl_vec_init(&r->entries, sizeof(registry_entry));
	return r;
}

/* Drops every logger (flushing it) and frees the registry. */
void
__dtfl_log_registry_free(__dtfl_log_registry *r)
{
	size_t i;

	if (r == NULL)
		return;
	for (i = 0; i < r->entries.len; i++) {
		registry_entry *e = DTFL_VEC_AT(&r->entries, registry_entry, i);
		__dtfl_logger_release(e->logger);
		__dtfl_free(e->name);
	}
	__dtfl_vec_free(&r->entries);
	__dtfl_free(r);
}

/* Index of the entry bound to `name`, or SIZE_MAX. */
static size_t
registry_find(const __dtfl_log_registry *r, const char *name)
{
	size_t i;

	for (i = 0; i < r->entries.len; i++) {
		const registry_entry *e = DTFL_VEC_AT(&r->entries, registry_entry, i);
		if (strcmp(e->name, name) == 0)
			return i;
	}
	return SIZE_MAX;
}

/*
 * Registry::insert: binds `name` to a new logger, replacing and
 * releasing any previous binding.
 */
void
__dtfl_log_registry_insert(__dtfl_log_registry *r, const char *name,
    __dtfl_log_action action, void *ctx, void (*ctx_free)(void *))
{
	__dtfl_logger *l = __dtfl_calloc(1, sizeof *l);
	size_t at;

	l->refs = 1;
	l->start_ns = r->start_ns;
	l->clock = r->clock;
	l->clock_ctx = r->clock_ctx;
	l->action = action;
	l->ctx = ctx;
	l->ctx_free = ctx_free;
	__dtfl_vec_init(&l->arena, 1);
	__dtfl_vec_init(&l->events, sizeof(__dtfl_log_event));

	at = registry_find(r, name);
	if (at != SIZE_MAX) {
		registry_entry *e = DTFL_VEC_AT(&r->entries, registry_entry, at);
		__dtfl_logger_release(e->logger);
		e->logger = l;
	} else {
		registry_entry e = { __dtfl_strdup(name), l };
		__dtfl_vec_push(&r->entries, &e);
	}
}

/* Registry::remove: unbinds `name`; returns true if it was bound. */
bool
__dtfl_log_registry_remove(__dtfl_log_registry *r, const char *name)
{
	size_t at = registry_find(r, name);
	registry_entry *e;

	if (at == SIZE_MAX)
		return false;
	e = DTFL_VEC_AT(&r->entries, registry_entry, at);
	__dtfl_logger_release(e->logger);
	__dtfl_free(e->name);
	__dtfl_vec_remove(&r->entries, at);
	return true;
}

/* Registry::get: a new reference to the logger bound to `name`, or NULL. */
__dtfl_logger *
__dtfl_log_registry_get(__dtfl_log_registry *r, const char *name)
{
	size_t at;

	if (r == NULL)
		return NULL;
	at = registry_find(r, name);
	if (at == SIZE_MAX)
		return NULL;
	return __dtfl_logger_retain(
	    DTFL_VEC_AT(&r->entries, registry_entry, at)->logger);
}

/* Flush::flush for every bound logger. */
void
__dtfl_log_registry_flush(__dtfl_log_registry *r)
{
	size_t i;

	for (i = 0; i < r->entries.len; i++)
		__dtfl_logger_flush(DTFL_VEC_AT(&r->entries, registry_entry, i)->logger);
}

/* Registry::names: stores up to `max` names; returns how many exist. */
size_t
__dtfl_log_registry_names(const __dtfl_log_registry *r, const char **names,
    size_t max)
{
	size_t i;

	for (i = 0; i < r->entries.len && i < max; i++)
		names[i] = DTFL_VEC_AT(&r->entries, registry_entry, i)->name;
	return r->entries.len;
}

__dtfl_logger *
__dtfl_logger_retain(__dtfl_logger *l)
{
	l->refs++;
	return l;
}

/*
 * Drops a reference.  The last one flushes pending events, then sends a
 * final flush notification, then releases the context.
 */
void
__dtfl_logger_release(__dtfl_logger *l)
{
	if (l == NULL)
		return;
	DTFL_ASSERT(l->refs > 0);
	if (--l->refs > 0)
		return;
	__dtfl_logger_flush(l);
	if (l->ctx_free != NULL)
		l->ctx_free(l->ctx);
	__dtfl_vec_free(&l->arena);
	__dtfl_vec_free(&l->events);
	__dtfl_free(l);
}

uint64_t
__dtfl_logger_now(const __dtfl_logger *l)
{
	return l->clock(l->clock_ctx) - l->start_ns;
}

/*
 * Hands buffered events to the action.  Event data pointers are stored
 * as arena offsets while buffering (the arena may move when it grows)
 * and resolved here.
 */
static void
logger_push(__dtfl_logger *l)
{
	size_t i;

	if (l->events.len == 0)
		return;
	for (i = 0; i < l->events.len; i++) {
		__dtfl_log_event *e = DTFL_VEC_AT(&l->events, __dtfl_log_event, i);
		e->data = l->arena.data + (size_t)(uintptr_t)e->data;
	}
	l->in_action = true;
	l->action(l->ctx, __dtfl_logger_now(l),
	    (const __dtfl_log_event *)l->events.data, l->events.len);
	l->in_action = false;
	__dtfl_vec_clear(&l->events);
	__dtfl_vec_clear(&l->arena);
}

/*
 * Logs one event stamped now.  A logger whose action logs to itself
 * would recurse; upstream's RefCell would panic, and so do we.
 */
void
__dtfl_logger_log(__dtfl_logger *l, const void *event, size_t len)
{
	__dtfl_log_event e;

	if (l->in_action)
		__dtfl_panic("logger used from within its own action");
	e.time_ns = __dtfl_logger_now(l);
	e.data = (const unsigned char *)(uintptr_t)l->arena.len;
	e.len = len;
	__dtfl_vec_push_n(&l->arena, event, len);
	__dtfl_vec_push(&l->events, &e);
	if (l->arena.len >= DTFL_BUFFER_SIZE_BYTES)
		logger_push(l);
}

/* Flush::flush: pushes buffered events, then a flush notification. */
void
__dtfl_logger_flush(__dtfl_logger *l)
{
	if (l->in_action)
		return;
	logger_push(l);
	l->in_action = true;
	l->action(l->ctx, __dtfl_logger_now(l), NULL, 0);
	l->in_action = false;
}
