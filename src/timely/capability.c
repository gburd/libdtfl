/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/capability.c: capabilities to send output.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/capability.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A capability holds a +1 at its time in its output's internal change
 * batch for its whole life.  clone adds another +1; drop a -1; downgrade
 * moves the +1 to the new time (a +1 at the new time, then a -1 at the
 * old).  These counts flow into the operator's shared internals and so
 * into progress tracking, which refuses to advance an output's frontier
 * past a held capability.  A delay or downgrade to a time not greater or
 * equal to the current time is a contract violation: delayed and
 * downgrade panic through the panic hook, try_* report the error.
 */
#include "timely/timely_int.h"

/*
 * Upstream's Capability is a value (time plus an Rc to the internal
 * batch), so minting one allocates nothing; a pooled block here comes
 * from the worker's free list.  Every field is set: a recycled block is
 * stale.
 */
dtfl_cap *
__dtfl_cap_new_pooled(__dtfl_freelist *pool, const dtfl_time_class *tc,
    const uint64_t *time, __dtfl_change_batch *internal, size_t port)
{
	dtfl_cap *c = pool != NULL ? __dtfl_freelist_get(pool) :
	    __dtfl_alloc(sizeof *c);

	c->tc = tc;
	__dtfl_time_copy(tc, c->time, time);
	c->internal = internal;
	c->port = port;
	c->pool = pool;
	__dtfl_cb_update(internal, time, 1);
	return c;
}

dtfl_cap *
__dtfl_cap_new(const dtfl_time_class *tc, const uint64_t *time,
    __dtfl_change_batch *internal, size_t port)
{
	return __dtfl_cap_new_pooled(NULL, tc, time, internal, port);
}

const uint64_t *
dtfl_cap_time(const dtfl_cap *c)
{
	return c->time;
}

size_t
dtfl_cap_output(const dtfl_cap *c)
{
	return c->port;
}

dtfl_cap *
dtfl_cap_try_delayed(const dtfl_cap *c, const uint64_t *t)
{
	if (!dtfl_time_less_equal(c->tc, c->time, t))
		return NULL;
	return __dtfl_cap_new_pooled(c->pool, c->tc, t, c->internal, c->port);
}

dtfl_cap *
dtfl_cap_delayed(const dtfl_cap *c, const uint64_t *t)
{
	dtfl_cap *out = dtfl_cap_try_delayed(c, t);

	if (out == NULL) {
		char from[128], to[128];

		dtfl_time_format(c->tc, c->time, from, sizeof from);
		dtfl_time_format(c->tc, t, to, sizeof to);
		__dtfl_panic("Attempted to delay %s to %s, which is not beyond the "
		    "capability's time.", from, to);
	}
	return out;
}

int
dtfl_cap_try_downgrade(dtfl_cap *c, const uint64_t *t)
{
	if (!dtfl_time_less_equal(c->tc, c->time, t))
		return __dtfl_err(DTFL_EINVAL, "could not downgrade the given capability");
	/* Move the +1: new time first, then retire the old. */
	__dtfl_cb_update(c->internal, t, 1);
	__dtfl_cb_update(c->internal, c->time, -1);
	__dtfl_time_copy(c->tc, c->time, t);
	return DTFL_OK;
}

void
dtfl_cap_downgrade(dtfl_cap *c, const uint64_t *t)
{
	if (dtfl_cap_try_downgrade(c, t) != DTFL_OK) {
		char from[128], to[128];

		dtfl_time_format(c->tc, c->time, from, sizeof from);
		dtfl_time_format(c->tc, t, to, sizeof to);
		__dtfl_panic("Attempted to downgrade %s to %s, which is not beyond the "
		    "capability's time.", from, to);
	}
}

dtfl_cap *
dtfl_cap_clone(const dtfl_cap *c)
{
	return __dtfl_cap_new_pooled(c->pool, c->tc, c->time, c->internal,
	    c->port);
}

void
dtfl_cap_drop(dtfl_cap *c)
{
	if (c == NULL)
		return;
	__dtfl_cb_update(c->internal, c->time, -1);
	if (c->pool != NULL)
		__dtfl_freelist_put(c->pool, c);
	else
		__dtfl_free(c);
}
