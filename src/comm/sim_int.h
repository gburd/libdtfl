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
 *
 * Faults (P16.7) widen the interleavings a seed can produce without
 * breaking any guarantee a real allocator gives; see __dtfl_sim_faults.
 */
#ifndef DTFL_SIM_INT_H
#define DTFL_SIM_INT_H

#include "comm/comm_int.h"
#include "dtfl/config.h"

/*
 * Fault injection.  Every fault is drawn from the group's generator, so
 * a seed and a fault set still fix the run.  A zero field is off; with
 * every field zero the draws are exactly those of a fault-free run.
 *
 *   stall_permille  the chance in 1000, each time the scheduler picks a
 *                   worker that is not stalled, that it stalls instead
 *                   of stepping
 *   stall_max       a stall lasts [1, stall_max] scheduler turns, during
 *                   which the worker is picked but does not step (0 is
 *                   taken as 1)
 *   delay_max       each message, data or flush, may not be delivered
 *                   for [0, delay_max] receive turns of the group.  A
 *                   held head holds back everything behind it on its
 *                   (sender, receiver, channel) FIFO, so a delay only
 *                   reorders messages of different FIFOs.
 *   max_steps       scheduler turns before the run panics as stuck; 0
 *                   is DTFL_SIM_MAX_STEPS
 *
 * Stalls and max_steps belong to the scheduler (src/timely/execute.c),
 * delay to delivery (alloc_sim.c).
 */
typedef struct __dtfl_sim_faults {
	uint32_t stall_permille;
	uint32_t stall_max;
	uint32_t delay_max;
	uint64_t max_steps;
} __dtfl_sim_faults;

/* Scheduler turns before a simulation is declared stuck. */
#define DTFL_SIM_MAX_STEPS UINT64_C(100000000)

/*
 * Fills builders[0 .. peers) with the builders of one simulated group.
 * `faults` (copied; NULL for none) configures delivery faults.  The
 * group's shared state is freed when every allocator (or unbuilt
 * builder) and every channel endpoint has been destroyed.
 */
void __dtfl_sim_builders(size_t peers, uint64_t seed,
    const __dtfl_sim_faults *faults, __dtfl_allocator_builder **builders);

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

/*
 * The faults a DTFL_COMM_SIM run of `cfg` injects; internal, for the
 * property tests (tests/pbt/pbt_sim.h).  Copied; NULL clears them.
 */
void __dtfl_config_set_sim_faults(dtfl_config *cfg,
    const __dtfl_sim_faults *faults);
const __dtfl_sim_faults *__dtfl_config_sim_faults(const dtfl_config *cfg);

/* splitmix64, the simulator's generator; exposed for its tests. */
uint64_t __dtfl_splitmix64(uint64_t *state);

#endif /* DTFL_SIM_INT_H */
