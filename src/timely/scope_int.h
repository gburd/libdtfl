/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/scope_int.h: the public scope and worker handles.
 *
 * A dtfl_worker (public) wraps the internal __dtfl_worker.  A dtfl_scope
 * exists only during a build callback and wraps a subgraph builder.
 */
#ifndef DTFL_SCOPE_INT_H
#define DTFL_SCOPE_INT_H

#include "dtfl/timely.h"
#include "timely/worker_int.h"

struct dtfl_worker {
	__dtfl_worker *w;
	bool standalone;	/* created by dtfl_worker_new_standalone */
};

struct dtfl_scope {
	__dtfl_worker *worker;
	dtfl_worker *pub;
	dtfl_scope *parent;		/* NULL at the root */
	__dtfl_subgraph_builder *builder;
};

/* OperatorSlot: a reserved child position in a scope. */
typedef struct __dtfl_slot {
	size_t index;
	size_t identifier;
	bool installed;
} __dtfl_slot;

__dtfl_slot __dtfl_scope_reserve(dtfl_scope *s);
/* The slot's address (scope path + index); caller frees. */
size_t *__dtfl_scope_slot_addr(const dtfl_scope *s, const __dtfl_slot *slot,
    size_t *len);
void __dtfl_scope_install(dtfl_scope *s, __dtfl_slot *slot, __dtfl_operate *op);
void __dtfl_scope_add_edge(dtfl_scope *s, __dtfl_location source,
    __dtfl_location target);

int __dtfl_worker_dataflow(__dtfl_worker *w, dtfl_worker *pub, const char *name,
    const dtfl_time_class *tc, dtfl_build_fn build, void *ctx);

/* The simulation runtime (execute.c). */
int __dtfl_sim_execute(const dtfl_config *cfg, dtfl_worker_fn fn, void *ctx);

#endif /* DTFL_SCOPE_INT_H */
