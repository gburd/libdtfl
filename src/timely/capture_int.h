/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/capture_int.h: capture events and the event link.
 *
 * An Event<T, C> is either Progress (an antichain change: times with
 * i64 deltas) or Messages (a time and a batch of records).  The event
 * link (capture/event.rs) is a shared, append-only singly linked list of
 * events; an EventPusher appends and an EventIterator reads, so a stream
 * captured on one side can be replayed on the other.  The sync link is
 * mutex-protected so a push on one thread is seen by a replay on another
 * (PLAN section 5).
 *
 * The wire format (dtfl_event_writer / dtfl_event_reader) is PLAN D9:
 * a u64 LE length then the bincode-1 fixint LE Event<T, C>.  Only
 * libdtfl reads these bytes, so the encoding of T and C is libdtfl's.
 */
#ifndef DTFL_CAPTURE_INT_H
#define DTFL_CAPTURE_INT_H

#include "base/os_sync.h"

#include "timely/timely_int.h"

#include "dtfl/timely_ops.h"

/* ------------------------------------------------------------------ */
/* Event (capture/event.rs Event<T, C>)                                */
/* ------------------------------------------------------------------ */

enum dtfl_event_tag {
	DTFL_EVENT_PROGRESS = 0,
	DTFL_EVENT_MESSAGES = 1
};

/*
 * A captured event.  Progress carries a change batch keyed by time
 * (depth words) with i64 deltas; Messages carries one time (depth words)
 * and a refcounted batch.  Owned by whoever holds it.
 */
typedef struct dtfl_event {
	enum dtfl_event_tag tag;
	unsigned depth;			/* time words */
	union {
		__dtfl_change_batch progress;	/* keyed by time, i64 delta */
		struct {
			uint64_t time[DTFL_TIME_MAX];
			__dtfl_batch *data;	/* owned */
		} messages;
	} u;
} dtfl_event;

/* Frees the event's contents (the change batch or the batch reference). */
void __dtfl_event_clear(dtfl_event *e);

/* ------------------------------------------------------------------ */
/* Event link (capture/event.rs link / link_sync)                      */
/* ------------------------------------------------------------------ */

/*
 * A shared singly linked list of events.  A pusher appends to the tail;
 * an iterator advances a read cursor and returns a borrow of each event.
 * The list and every event belong to it; dtfl_event_link_free drops the
 * whole chain iteratively (avoid_stack_overflow_in_drop).  A sync link
 * guards the links with a mutex so push and iterate may run on different
 * threads.
 */
struct dtfl_event_link {
	const dtfl_time_class *tc;	/* retained */
	const dtfl_type *type;		/* retained */
	bool sync;
	__dtfl_mutex lock;		/* used only when sync */
	struct dtfl_event_node *head;	/* owned chain */
	struct dtfl_event_node *tail;	/* append point */
	struct dtfl_event_node *read;	/* iterator cursor (extract/replay) */
};

/* ------------------------------------------------------------------ */
/* Event pusher constructors shared across capture.c / event.c         */
/* ------------------------------------------------------------------ */

/* Appends an event (takes ownership of its contents) to the link. */
void __dtfl_event_link_push(dtfl_event_link *l, dtfl_event *e);
/*
 * Advances the read cursor and returns the next event, or NULL at the
 * end.  The borrow is valid until the link is freed (events are never
 * removed, as upstream's Cow::Borrowed path).
 */
const dtfl_event *__dtfl_event_link_next(dtfl_event_link *l);

/* ------------------------------------------------------------------ */
/* Event wire codec (PLAN D9), in event.c                              */
/* ------------------------------------------------------------------ */

/* Appends the u64 LE length + bincode Event<T, C> framing to `out`. */
void __dtfl_event_encode(const dtfl_time_class *tc, const dtfl_event *e,
    __dtfl_vec *out);
/*
 * Decodes one framed event from `p` (len bytes available).  On success
 * fills *e and stores the bytes consumed in *consumed; returns true.
 * Returns false if fewer than a whole event is available.
 */
bool __dtfl_event_decode(const dtfl_time_class *tc, const dtfl_type *type,
    const unsigned char *p, size_t len, dtfl_event *e, size_t *consumed);

#endif /* DTFL_CAPTURE_INT_H */
