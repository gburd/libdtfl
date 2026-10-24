/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/scope.c: scopes, and building dataflows into them.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/scope.rs
 *   timely/src/worker.rs (dataflow, dataflow_named, dataflow_core)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A dtfl_scope wraps a subgraph builder for the duration of a build
 * callback (upstream's Scope borrows a RefCell<SubgraphBuilder>).  The
 * scope struct lives on the C stack of the call that created it and is
 * valid only inside the build callback, as upstream's lifetime 'scope.
 * Operators reserve a slot (index + worker-unique identifier) and later
 * install themselves into it.
 */
#include "timely/scope_int.h"

dtfl_worker *
dtfl_scope_worker(const dtfl_scope *s)
{
	return s->pub;
}

const dtfl_time_class *
dtfl_scope_time_class(const dtfl_scope *s)
{
	return __dtfl_subgraph_builder_inner(s->builder);
}

size_t
dtfl_scope_index(const dtfl_scope *s)
{
	return s->worker->allocator->index;
}

size_t
dtfl_scope_peers(const dtfl_scope *s)
{
	return s->worker->allocator->peers;
}

const size_t *
dtfl_scope_addr(const dtfl_scope *s, size_t *len)
{
	return __dtfl_subgraph_builder_path(s->builder, len);
}

dtfl_scope *
dtfl_scope_parent(const dtfl_scope *s)
{
	return s->parent;
}

/* Scope::reserve_operator: a child index and a worker-unique id. */
__dtfl_slot
__dtfl_scope_reserve(dtfl_scope *s)
{
	__dtfl_slot slot;

	slot.index = __dtfl_subgraph_builder_allocate_child_id(s->builder);
	slot.identifier = __dtfl_worker_new_identifier(s->worker);
	slot.installed = false;
	return slot;
}

/* The address of a reserved slot: the scope's path plus its index. */
size_t *
__dtfl_scope_slot_addr(const dtfl_scope *s, const __dtfl_slot *slot, size_t *len)
{
	size_t plen;
	const size_t *path = __dtfl_subgraph_builder_path(s->builder, &plen);
	size_t *addr = __dtfl_alloc((plen + 1) * sizeof(size_t));

	memcpy(addr, path, plen * sizeof(size_t));
	addr[plen] = slot->index;
	*len = plen + 1;
	return addr;
}

/* OperatorSlot::install: hands the operator to the subgraph builder. */
void
__dtfl_scope_install(dtfl_scope *s, __dtfl_slot *slot, __dtfl_operate *op)
{
	DTFL_ASSERT(!slot->installed);
	__dtfl_subgraph_builder_add_child(s->builder, op, slot->index, slot->identifier);
	slot->installed = true;
}

void
__dtfl_scope_add_edge(dtfl_scope *s, __dtfl_location source, __dtfl_location target)
{
	__dtfl_subgraph_builder_connect(s->builder, source, target);
}

/*
 * Scope::scoped: reserves a slot, builds the child subgraph with
 * `build`, and installs it.  The inner class must refine this scope's.
 */
int
dtfl_scope_scoped(dtfl_scope *s, const char *name, const dtfl_time_class *inner,
    dtfl_build_fn build, void *ctx)
{
	__dtfl_slot slot;
	dtfl_scope child;
	size_t *addr, len;
	__dtfl_operate *op;

	DTFL_WORKER_CHECK(s->worker);
	if (!__dtfl_refines(inner, dtfl_scope_time_class(s)))
		return __dtfl_err(DTFL_EINVAL,
		    "scope %s: inner time class does not refine the outer class", name);
	slot = __dtfl_scope_reserve(s);
	addr = __dtfl_scope_slot_addr(s, &slot, &len);
	memset(&child, 0, sizeof child);
	child.worker = s->worker;
	child.pub = s->pub;
	child.parent = s;
	child.builder = __dtfl_subgraph_builder_new(addr, len, slot.identifier, name,
	    dtfl_scope_time_class(s), inner);
	__dtfl_free(addr);
	build(&child, ctx);
	op = __dtfl_subgraph_builder_build(child.builder, s->worker);
	__dtfl_scope_install(s, &slot, op);
	return DTFL_OK;
}

int
dtfl_scope_iterative(dtfl_scope *s, const char *name, dtfl_build_fn build,
    void *ctx)
{
	dtfl_time_class *u = dtfl_time_u64();
	dtfl_time_class *inner = dtfl_time_product(dtfl_scope_time_class(s), u);
	int rc;

	dtfl_time_class_free(u);
	if (inner == NULL)
		return DTFL_ERANGE;
	rc = dtfl_scope_scoped(s, name != NULL ? name : "Iterative", inner, build, ctx);
	dtfl_time_class_free(inner);
	return rc;
}

int
dtfl_scope_region(dtfl_scope *s, const char *name, dtfl_build_fn build, void *ctx)
{
	return dtfl_scope_scoped(s, name != NULL ? name : "Region",
	    dtfl_scope_time_class(s), build, ctx);
}

/*
 * Worker::dataflow_core: builds a top-level subgraph over `tc` (which
 * refines the unit class) and installs it.
 */
int
__dtfl_worker_dataflow(__dtfl_worker *w, dtfl_worker *pub, const char *name,
    const dtfl_time_class *tc, dtfl_build_fn build, void *ctx)
{
	dtfl_time_class *unit = dtfl_time_unit();
	size_t index, identifier;
	dtfl_scope root;
	__dtfl_operate *op;

	DTFL_WORKER_CHECK(w);
	if (w->stepping) {
		dtfl_time_class_free(unit);
		return __dtfl_err(DTFL_ESTATE, "dataflows cannot be built from inside an operator");
	}
	index = __dtfl_worker_allocate_dataflow_index(w);
	identifier = __dtfl_worker_new_identifier(w);
	memset(&root, 0, sizeof root);
	root.worker = w;
	root.pub = pub;
	root.builder = __dtfl_subgraph_builder_new(&index, 1, identifier,
	    name != NULL ? name : "Dataflow", unit, tc);
	dtfl_time_class_free(unit);
	build(&root, ctx);
	op = __dtfl_subgraph_builder_build(root.builder, w);
	__dtfl_worker_install_dataflow(w, index, identifier, op, NULL, NULL);
	return DTFL_OK;
}
