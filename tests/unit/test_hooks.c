/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_hooks.c: the embedding host hooks for logging and
 * thread spawning (dtfl_set_log_hook, dtfl_set_thread_hooks).  Each
 * test runs in munit's forked child, so the process-wide hook state
 * starts clean.
 */
#define _POSIX_C_SOURCE 200809L

#include "dtfl_test.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

#include "base/base_int.h"
#include "dtfl/config.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

/* ------------------------------------------------------------------ */
/* Log hook                                                            */
/* ------------------------------------------------------------------ */

static dtfl_log_level last_level;
static char last_msg[256];
static int log_calls;

static void
record_log(dtfl_log_level level, const char *msg, void *ctx)
{
	int *marker = ctx;

	last_level = level;
	snprintf(last_msg, sizeof last_msg, "%s", msg);
	log_calls++;
	if (marker != NULL)
		*marker = 1;
}

DTFL_TEST(log_hook_sees_level_and_message)
{
	int marker = 0;

	(void)params; (void)data;
	dtfl_set_log_hook(record_log, &marker);

	__dtfl_log(DTFL_LOG_DEBUG, "d%d", 1);
	assert_int(last_level, ==, DTFL_LOG_DEBUG);
	assert_string_equal(last_msg, "d1");

	__dtfl_log(DTFL_LOG_INFO, "info");
	assert_int(last_level, ==, DTFL_LOG_INFO);

	__dtfl_log(DTFL_LOG_WARN, "w%s", "arn");
	assert_int(last_level, ==, DTFL_LOG_WARN);
	assert_string_equal(last_msg, "warn");

	__dtfl_log(DTFL_LOG_ERROR, "boom");
	assert_int(last_level, ==, DTFL_LOG_ERROR);
	assert_string_equal(last_msg, "boom");

	/* ctx reached the hook, and every level was delivered. */
	assert_int(marker, ==, 1);
	assert_int(log_calls, ==, 4);

	dtfl_set_log_hook(NULL, NULL);
	return MUNIT_OK;
}

/*
 * With no hook, DEBUG and INFO are dropped.  The default writes WARN
 * and ERROR to stderr.  Redirect stderr to a temp file (safe: munit
 * forks this test) and check that the quiet levels produce no bytes
 * while WARN does, so the drop is observable, not assumed.
 */
DTFL_TEST(log_default_is_quiet)
{
	char path[] = "/tmp/dtfl_log_quiet_XXXXXX";
	int fd;
	long debug_pos, info_pos, warn_pos;

	(void)params; (void)data;
	fd = mkstemp(path);
	assert_int(fd, !=, -1);
	assert_not_null(freopen(path, "w", stderr));
	dtfl_set_log_hook(NULL, NULL);

	__dtfl_log(DTFL_LOG_DEBUG, "quiet");
	debug_pos = ftell(stderr);
	__dtfl_log(DTFL_LOG_INFO, "quiet");
	info_pos = ftell(stderr);
	__dtfl_log(DTFL_LOG_WARN, "loud");
	warn_pos = ftell(stderr);

	/* DEBUG and INFO wrote nothing; WARN advanced the stream. */
	assert_long(debug_pos, ==, 0);
	assert_long(info_pos, ==, 0);
	assert_long(warn_pos, >, 0);

	/* A hook, once installed, does see DEBUG. */
	dtfl_set_log_hook(record_log, NULL);
	log_calls = 0;
	__dtfl_log(DTFL_LOG_DEBUG, "loud");
	assert_int(log_calls, ==, 1);
	dtfl_set_log_hook(NULL, NULL);

	close(fd);
	unlink(path);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Thread hooks                                                        */
/* ------------------------------------------------------------------ */

static atomic_int spawn_calls;
static atomic_int join_calls;

static int
record_spawn(void *(*start)(void *), void *arg, void **handle_out, void *ctx)
{
	pthread_t *t = malloc(sizeof *t);
	int rc;

	(void)ctx;
	atomic_fetch_add(&spawn_calls, 1);
	rc = pthread_create(t, NULL, start, arg);
	if (rc != 0) {
		free(t);
		return DTFL_EIO;
	}
	*handle_out = t;
	return 0;
}

static int
record_join(void *handle, void **retval_out, void *ctx)
{
	pthread_t *t = handle;

	(void)ctx;
	atomic_fetch_add(&join_calls, 1);
	pthread_join(*t, retval_out);
	free(t);
	return 0;
}

static void
noop_worker(dtfl_worker *w, void *ctx)
{
	(void)w; (void)ctx;
}

static void
run_process(size_t workers)
{
	dtfl_config *cfg = dtfl_config_new();

	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, workers), ==, 0);
	assert_int(dtfl_execute(cfg, noop_worker, NULL, NULL), ==, 0);
	dtfl_config_free(cfg);
}

DTFL_TEST(thread_hooks_replace_pthread)
{
	dtfl_thread_hooks h = { record_spawn, record_join, NULL };

	(void)params; (void)data;
	assert_int(dtfl_set_thread_hooks(&h), ==, 0);
	atomic_store(&spawn_calls, 0);
	atomic_store(&join_calls, 0);

	run_process(2);

	/* One spawn and one join per worker, through the hook. */
	assert_int(atomic_load(&spawn_calls), ==, 2);
	assert_int(atomic_load(&join_calls), ==, 2);
	dtfl_set_thread_hooks(NULL);
	return MUNIT_OK;
}

DTFL_TEST(thread_hooks_unset_restores_pthread)
{
	dtfl_thread_hooks h = { record_spawn, record_join, NULL };

	(void)params; (void)data;
	assert_int(dtfl_set_thread_hooks(&h), ==, 0);
	assert_int(dtfl_set_thread_hooks(NULL), ==, 0);
	atomic_store(&spawn_calls, 0);
	atomic_store(&join_calls, 0);

	/* The default pthread path runs; the hook counters stay zero. */
	run_process(2);
	assert_int(atomic_load(&spawn_calls), ==, 0);
	assert_int(atomic_load(&join_calls), ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(thread_hooks_reject_half_a_pair)
{
	dtfl_thread_hooks spawn_only = { record_spawn, NULL, NULL };
	dtfl_thread_hooks join_only = { NULL, record_join, NULL };

	(void)params; (void)data;
	assert_int(dtfl_set_thread_hooks(&spawn_only), ==, DTFL_EINVAL);
	assert_int(dtfl_set_thread_hooks(&join_only), ==, DTFL_EINVAL);
	return MUNIT_OK;
}

/*
 * The single-threaded cooperative path needs no thread hook: the host
 * steps the worker on its own thread.  Spawn must never be called.
 */
DTFL_TEST(standalone_needs_no_thread_hook)
{
	dtfl_thread_hooks h = { record_spawn, record_join, NULL };
	dtfl_worker *w;

	(void)params; (void)data;
	assert_int(dtfl_set_thread_hooks(&h), ==, 0);
	atomic_store(&spawn_calls, 0);

	w = dtfl_worker_new_standalone();
	assert_not_null(w);
	while (dtfl_worker_has_dataflows(w))
		(void)dtfl_worker_step(w);
	dtfl_worker_free(w);

	assert_int(atomic_load(&spawn_calls), ==, 0);
	dtfl_set_thread_hooks(NULL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(log_hook_sees_level_and_message),
	DTFL_T(log_default_is_quiet),
	DTFL_T(thread_hooks_replace_pthread),
	DTFL_T(thread_hooks_unset_restores_pthread),
	DTFL_T(thread_hooks_reject_half_a_pair),
	DTFL_T(standalone_needs_no_thread_hook),
	DTFL_T_END
};

DTFL_TEST_MAIN("hooks", tests)
