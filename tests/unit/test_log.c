/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_log.c: the logging registry (logging/src/lib.rs).
 */
#include "dtfl_test.h"
#include "base/log_int.h"
#include "base/batch_int.h"

typedef struct sink {
	size_t events;
	size_t flushes;
	size_t bytes;
	uint64_t last_time;
	int freed;
	char first[32];
} sink;

static void
sink_action(void *ctx, uint64_t now, const __dtfl_log_event *ev, size_t n)
{
	sink *s = ctx;
	size_t i;

	if (ev == NULL) {
		s->flushes++;
		return;
	}
	for (i = 0; i < n; i++) {
		if (s->events == 0 && ev[i].len < sizeof s->first)
			memcpy(s->first, ev[i].data, ev[i].len);
		/* Timestamps never exceed the batch's time. */
		munit_assert_uint64(ev[i].time_ns, <=, now);
		s->events++;
		s->bytes += ev[i].len;
		s->last_time = ev[i].time_ns;
	}
}

static void
sink_free(void *ctx)
{
	((sink *)ctx)->freed++;
}

static uint64_t fake_now;

static uint64_t
fake_clock(void *ctx)
{
	(void)ctx;
	return fake_now;
}

DTFL_TEST(registry_insert_get_remove)
{
	__dtfl_log_registry *r = __dtfl_log_registry_new(NULL, NULL);
	sink s = { 0 };
	__dtfl_logger *l;
	const char *names[4];

	(void)params; (void)data;
	assert_null(__dtfl_log_registry_get(r, "timely"));
	__dtfl_log_registry_insert(r, "timely", sink_action, &s, sink_free);
	l = __dtfl_log_registry_get(r, "timely");
	assert_not_null(l);
	__dtfl_logger_log(l, "hello", 5);
	__dtfl_logger_flush(l);
	assert_size(s.events, ==, 1);
	assert_size(s.flushes, ==, 1);
	assert_memory_equal(5, s.first, "hello");
	assert_size(__dtfl_log_registry_names(r, names, 4), ==, 1);
	assert_string_equal(names[0], "timely");
	/* Removing drops the registry's reference; ours keeps it alive. */
	assert_true(__dtfl_log_registry_remove(r, "timely"));
	assert_false(__dtfl_log_registry_remove(r, "timely"));
	assert_int(s.freed, ==, 0);
	__dtfl_logger_release(l);
	assert_int(s.freed, ==, 1);
	__dtfl_log_registry_free(r);
	return MUNIT_OK;
}

DTFL_TEST(logger_batches_at_buffer_size)
{
	__dtfl_log_registry *r = __dtfl_log_registry_new(NULL, NULL);
	sink s = { 0 };
	__dtfl_logger *l;
	char ev[100];
	size_t i;

	(void)params; (void)data;
	memset(ev, 'x', sizeof ev);
	__dtfl_log_registry_insert(r, "x", sink_action, &s, NULL);
	l = __dtfl_log_registry_get(r, "x");
	/* Fewer than a buffer's worth: nothing delivered yet. */
	for (i = 0; i < 10; i++)
		__dtfl_logger_log(l, ev, sizeof ev);
	assert_size(s.events, ==, 0);
	/* Enough to cross DTFL_BUFFER_SIZE_BYTES: delivered without flush. */
	for (i = 0; i < DTFL_BUFFER_SIZE_BYTES / sizeof ev; i++)
		__dtfl_logger_log(l, ev, sizeof ev);
	assert_size(s.events, >, 0);
	assert_size(s.flushes, ==, 0);
	__dtfl_logger_release(l);
	/* Freeing the registry drops the last reference, which flushes. */
	__dtfl_log_registry_free(r);
	assert_size(s.events, ==, 10 + DTFL_BUFFER_SIZE_BYTES / sizeof ev);
	assert_size(s.bytes, ==, s.events * sizeof ev);
	assert_size(s.flushes, ==, 1);
	return MUNIT_OK;
}

DTFL_TEST(logger_times_relative_to_registry)
{
	__dtfl_log_registry *r;
	sink s = { 0 };
	__dtfl_logger *l;

	(void)params; (void)data;
	fake_now = 1000;
	r = __dtfl_log_registry_new(fake_clock, NULL);
	__dtfl_log_registry_insert(r, "t", sink_action, &s, NULL);
	l = __dtfl_log_registry_get(r, "t");
	fake_now = 1500;
	__dtfl_logger_log(l, "a", 1);
	assert_uint64(__dtfl_logger_now(l), ==, 500);
	__dtfl_logger_flush(l);
	assert_uint64(s.last_time, ==, 500);
	__dtfl_logger_release(l);
	__dtfl_log_registry_free(r);
	return MUNIT_OK;
}

DTFL_TEST(insert_replaces_binding)
{
	__dtfl_log_registry *r = __dtfl_log_registry_new(NULL, NULL);
	sink a = { 0 }, b = { 0 };
	__dtfl_logger *l;

	(void)params; (void)data;
	__dtfl_log_registry_insert(r, "n", sink_action, &a, sink_free);
	__dtfl_log_registry_insert(r, "n", sink_action, &b, sink_free);
	assert_int(a.freed, ==, 1);
	l = __dtfl_log_registry_get(r, "n");
	__dtfl_logger_log(l, "z", 1);
	__dtfl_logger_flush(l);
	assert_size(a.events, ==, 0);
	assert_size(b.events, ==, 1);
	__dtfl_logger_release(l);
	__dtfl_log_registry_free(r);
	assert_int(b.freed, ==, 1);
	return MUNIT_OK;
}

static __dtfl_logger *reentrant_logger;

static void
reentrant_action(void *ctx, uint64_t now, const __dtfl_log_event *ev, size_t n)
{
	(void)ctx; (void)now; (void)n;
	if (ev != NULL)
		__dtfl_logger_log(reentrant_logger, "again", 5);
}

DTFL_TEST(reentrant_log_panics)
{
	__dtfl_log_registry *r = __dtfl_log_registry_new(NULL, NULL);

	(void)params; (void)data;
	__dtfl_log_registry_insert(r, "r", reentrant_action, NULL, NULL);
	reentrant_logger = __dtfl_log_registry_get(r, "r");
	__dtfl_logger_log(reentrant_logger, "first", 5);
	DTFL_EXPECT_PANIC(__dtfl_logger_flush(reentrant_logger));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(registry_insert_get_remove),
	DTFL_T(logger_batches_at_buffer_size),
	DTFL_T(logger_times_relative_to_registry),
	DTFL_T(insert_replaces_binding),
	DTFL_T(reentrant_log_panics),
	DTFL_T_END
};

DTFL_TEST_MAIN("log", tests)
