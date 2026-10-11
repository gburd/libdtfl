/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/sim_int.h: the simulation allocator (PLAN D8).
 *
 * `peers` workers share one OS thread.  Each worker has its own
 * allocator, but a message pushed by one is only "in flight" until the
 * receiver calls receive(); each receive() then delivers a
 * pseudo-random subset of that worker's in-flight messages, in a
 * pseudo-random interleaving, chosen by a splitmix64 generator seeded
 * with `seed`.  Messages on one (channel, sender, receiver) triple stay
 * in FIFO order, as every real allocator guarantees.  Self-sends are
 * delayed the same way.  A flush (done()) travels as a marker in the
 * same FIFO and raises the receiver's channel event when delivered,
 * as ArcPusher does for a flush.
 *
 * Given the same seed and the same sequence of calls, delivery is
 * identical.  Nothing here is thread-safe: every allocator and endpoint
 * of a group must be used from the one thread that runs the simulation.
 *
 * The group has a virtual clock, in nanoseconds from 0, that moves only
 * through __dtfl_sim_advance.  await_events never blocks: with events
 * pending it returns at once (a real park would too); otherwise it
 * advances the clock by the timeout, or by DTFL_SIM_IDLE_STEP_NS for
 * an unbounded wait, and returns.  The scheduler that decides which
 * worker steps next is built on these (src/timely/execute.c).
 */
#ifndef DTFL_SIM_INT_H
#define DTFL_SIM_INT_H

#include "comm/comm_int.h"

/*
 * Fills builders[0 .. peers) with the builders of one simulated group.
 * The group's shared state is freed when every allocator (or unbuilt
 * builder) and every channel endpoint has been destroyed.
 */
void __dtfl_sim_builders(size_t peers, uint64_t seed,
    __dtfl_allocator_builder **builders);

/* Virtual time an unbounded await_events moves the clock by: 1 ms. */
#define DTFL_SIM_IDLE_STEP_NS UINT64_C(1000000)

/*
 * The group state of a simulation allocator, or NULL if `a` is another
 * kind of allocator.  Valid while any allocator, builder or channel of
 * the group lives.
 */
void *__dtfl_sim_of(__dtfl_allocator *a);
/* The next draw of the group's generator (shared with delivery). */
uint64_t __dtfl_sim_rand(void *sim);
/* The group's virtual time in nanoseconds. */
uint64_t __dtfl_sim_clock(void *sim);
/* Moves virtual time forward by `ns` (saturating). */
void __dtfl_sim_advance(void *sim, uint64_t ns);

/* splitmix64, the simulator's generator; exposed for its tests. */
uint64_t __dtfl_splitmix64(uint64_t *state);

#endif /* DTFL_SIM_INT_H */
