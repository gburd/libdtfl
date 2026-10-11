/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_config.c: dtfl_config and its command-line and
 * environment parsers, against the behavior of timely's getopts-based
 * Config::from_args at timely-v0.31.0.
 */
#define _POSIX_C_SOURCE 200809L	/* mkstemp, fdopen, unlink */

#include "dtfl_test.h"
#include "base/base_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dtfl/config.h"

/* Parses a NULL-terminated argument list into a new config. */
static int
parse(dtfl_config **out, char **rest, int *nrest, char *const *argv)
{
	int argc = 0;

	while (argv[argc] != NULL)
		argc++;
	*out = dtfl_config_new();
	return dtfl_config_from_args(*out, argc, argv, rest, nrest);
}

DTFL_TEST(config_defaults)
{
	dtfl_config *cfg = dtfl_config_new();

	(void)params; (void)data;
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_THREAD);
	assert_size(dtfl_config_threads(cfg), ==, 1);
	assert_size(dtfl_config_process(cfg), ==, 0);
	assert_size(dtfl_config_processes(cfg), ==, 1);
	assert_null(dtfl_config_address(cfg, 0));
	assert_false(dtfl_config_report(cfg));
	assert_int(dtfl_config_progress_mode(cfg), ==, DTFL_PROGRESS_DEMAND);
	assert_uint64(dtfl_config_seed(cfg), ==, 0);
	assert_null(dtfl_config_timely_log_addr(cfg));
	assert_null(dtfl_config_differential_log_addr(cfg));
	dtfl_config_free(cfg);
	dtfl_config_free(NULL);
	return MUNIT_OK;
}

DTFL_TEST(config_setters_validate)
{
	dtfl_config *cfg = dtfl_config_new();
	const char *addrs[] = { "a:1", "b:2" };

	(void)params; (void)data;
	assert_int(dtfl_config_set_threads(cfg, 0), ==, DTFL_EINVAL);
	assert_int(dtfl_config_set_threads(cfg, 6), ==, 0);
	assert_size(dtfl_config_threads(cfg), ==, 6);
	assert_int(dtfl_config_set_mode(cfg, (dtfl_comm_mode)99), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_SIM), ==, 0);
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_SIM);
	assert_int(dtfl_config_set_cluster(cfg, 2, addrs, 2), ==, DTFL_EINVAL);
	assert_int(dtfl_config_set_cluster(cfg, 0, NULL, 0), ==, DTFL_EINVAL);
	assert_int(dtfl_config_set_cluster(cfg, 1, addrs, 2), ==, 0);
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_SIM);	/* unchanged */
	assert_size(dtfl_config_process(cfg), ==, 1);
	assert_size(dtfl_config_processes(cfg), ==, 2);
	assert_ptr_not_equal(dtfl_config_address(cfg, 0), addrs[0]);	/* copied */
	assert_string_equal(dtfl_config_address(cfg, 1), "b:2");
	assert_null(dtfl_config_address(cfg, 2));
	dtfl_config_set_report(cfg, true);
	assert_true(dtfl_config_report(cfg));
	dtfl_config_set_progress_mode(cfg, DTFL_PROGRESS_EAGER);
	assert_int(dtfl_config_progress_mode(cfg), ==, DTFL_PROGRESS_EAGER);
	dtfl_config_set_seed(cfg, 42);
	assert_uint64(dtfl_config_seed(cfg), ==, 42);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

DTFL_TEST(config_from_args_thread_forms)
{
	static char *const forms[][4] = {
		{ "prog", "-w4", NULL },
		{ "prog", "-w", "4", NULL },
		{ "prog", "--threads=4", NULL },
		{ "prog", "--threads", "4", NULL },
		{ "prog", "-w+4", NULL },	/* usize::from_str takes '+' */
	};
	char *rest[8];
	dtfl_config *cfg;
	int nrest;
	size_t i;

	(void)params; (void)data;
	for (i = 0; i < DTFL_ARRAY_LEN(forms); i++) {
		assert_int(parse(&cfg, rest, &nrest, forms[i]), ==, 0);
		assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_PROCESS);
		assert_size(dtfl_config_threads(cfg), ==, 4);
		assert_int(nrest, ==, 1);
		assert_string_equal(rest[0], "prog");
		dtfl_config_free(cfg);
	}
	return MUNIT_OK;
}

DTFL_TEST(config_from_args_modes)
{
	static char *const zc[] = { "p", "-z", "-w", "2", NULL };
	static char *const cluster[] = { "p", "-zr", "-n", "2", "-p", "1", NULL };
	static char *const one[] = { "p", "-w", "1", "-z", NULL };
	static char *const zw[] = { "p", "-zw3", NULL };
	static char *const eager[] = { "p", "--progress-mode", "eager", NULL };
	static char *const demand[] = { "p", "--progress-mode=demand", NULL };
	char *rest[8];
	dtfl_config *cfg;
	int nrest;

	(void)params; (void)data;
	assert_int(parse(&cfg, rest, &nrest, zc), ==, 0);
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_PROCESS_BINARY);
	assert_size(dtfl_config_threads(cfg), ==, 2);
	dtfl_config_free(cfg);

	assert_int(parse(&cfg, rest, &nrest, cluster), ==, 0);
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_CLUSTER);
	assert_size(dtfl_config_threads(cfg), ==, 1);
	assert_size(dtfl_config_process(cfg), ==, 1);
	assert_size(dtfl_config_processes(cfg), ==, 2);
	assert_true(dtfl_config_report(cfg));
	/* from_matches: localhost:2101 + index. */
	assert_string_equal(dtfl_config_address(cfg, 0), "localhost:2101");
	assert_string_equal(dtfl_config_address(cfg, 1), "localhost:2102");
	dtfl_config_free(cfg);

	assert_int(parse(&cfg, rest, &nrest, one), ==, 0);
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_THREAD);
	assert_size(dtfl_config_threads(cfg), ==, 1);
	dtfl_config_free(cfg);

	/* A flag cluster ending in an option with its argument attached. */
	assert_int(parse(&cfg, rest, &nrest, zw), ==, 0);
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_PROCESS_BINARY);
	assert_size(dtfl_config_threads(cfg), ==, 3);
	dtfl_config_free(cfg);

	assert_int(parse(&cfg, rest, &nrest, eager), ==, 0);
	assert_int(dtfl_config_progress_mode(cfg), ==, DTFL_PROGRESS_EAGER);
	dtfl_config_free(cfg);
	assert_int(parse(&cfg, rest, &nrest, demand), ==, 0);
	assert_int(dtfl_config_progress_mode(cfg), ==, DTFL_PROGRESS_DEMAND);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

DTFL_TEST(config_from_args_rest_in_order)
{
	static char *const argv[] = {
		"prog", "input.txt", "-w", "2", "--unknown", "-x", "7", "-",
		"-zq", "--", "-w", "5", NULL
	};
	char *rest[16];
	dtfl_config *cfg;
	int nrest;

	(void)params; (void)data;
	assert_int(parse(&cfg, rest, &nrest, argv), ==, 0);
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_PROCESS);
	assert_size(dtfl_config_threads(cfg), ==, 2);
	/*
	 * Kept in order: argv[0], free arguments, options timely does not
	 * know ("-zq" has an unknown letter, so the word is kept whole),
	 * and everything after "--", which itself is dropped.
	 */
	assert_int(nrest, ==, 9);
	assert_string_equal(rest[0], "prog");
	assert_string_equal(rest[1], "input.txt");
	assert_string_equal(rest[2], "--unknown");
	assert_string_equal(rest[3], "-x");
	assert_string_equal(rest[4], "7");
	assert_string_equal(rest[5], "-");
	assert_string_equal(rest[6], "-zq");
	assert_string_equal(rest[7], "-w");
	assert_string_equal(rest[8], "5");
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

DTFL_TEST(config_from_args_rest_may_alias_argv)
{
	char a0[] = "prog", a1[] = "-w", a2[] = "3", a3[] = "file";
	char *argv[] = { a0, a1, a2, a3, NULL };
	dtfl_config *cfg = dtfl_config_new();
	int nrest;

	(void)params; (void)data;
	assert_int(dtfl_config_from_args(cfg, 4, argv, argv, &nrest), ==, 0);
	assert_int(nrest, ==, 2);
	assert_ptr_equal(argv[0], a0);
	assert_ptr_equal(argv[1], a3);
	assert_size(dtfl_config_threads(cfg), ==, 3);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

DTFL_TEST(config_from_args_errors)
{
	static char *const bad[][4] = {
		{ "p", "-w", NULL },		/* ArgumentMissing */
		{ "p", "--threads", NULL },
		{ "p", "-w", "x", NULL },	/* invalid digit */
		{ "p", "-w", "", NULL },
		{ "p", "-w", "-3", NULL },
		{ "p", "-w2", "-w3", NULL },	/* OptionDuplicated */
		{ "p", "--report=yes", NULL },	/* UnexpectedArgument */
		{ "p", "--progress-mode", "lazy", NULL },
		{ "p", "-w", "99999999999999999999999", NULL },
		{ "p", "-n", "2", "-p2" },	/* -p out of range */
	};
	char *rest[8] = { NULL };
	dtfl_config *cfg;
	int nrest = -1;
	size_t i;

	(void)params; (void)data;
	for (i = 0; i < DTFL_ARRAY_LEN(bad); i++) {
		char *argv[5] = { NULL };
		int argc;

		for (argc = 0; argc < 4 && bad[i][argc] != NULL; argc++)
			argv[argc] = bad[i][argc];
		cfg = dtfl_config_new();
		assert_int(dtfl_config_from_args(cfg, argc, argv, rest, &nrest),
		    ==, DTFL_EINVAL);
		assert_string_not_equal(dtfl_last_error_detail(), "");
		/* On error nothing changes. */
		assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_THREAD);
		assert_int(nrest, ==, -1);
		assert_null(rest[0]);
		dtfl_config_free(cfg);
	}
	return MUNIT_OK;
}

/* Writes `text` to a new temporary file and returns its path. */
static char *
write_hostfile(const char *text)
{
	static char path[256];
	const char *dir = getenv("TMPDIR");
	FILE *f;
	int fd;

	snprintf(path, sizeof path, "%s/dtfl-hosts-XXXXXX",
	    dir != NULL && dir[0] != '\0' ? dir : "/tmp");
	fd = mkstemp(path);
	if (fd < 0 || (f = fdopen(fd, "w")) == NULL)
		return NULL;
	fputs(text, f);
	fclose(f);
	return path;
}

DTFL_TEST(config_from_args_hostfile)
{
	char *path, *rest[8];
	dtfl_config *cfg;
	int nrest;

	(void)params; (void)data;
	/* CRLF and a last line without a newline; only -n lines are read. */
	path = write_hostfile("h0:7000\r\nh1:7001\nh2:7002 h2b:7003\nextra\n");
	assert_not_null(path);
	{
		char *const argv[] = { "p", "-n", "3", "-h", path, "-p", "2", NULL };

		assert_int(parse(&cfg, rest, &nrest, argv), ==, 0);
	}
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_CLUSTER);
	assert_size(dtfl_config_processes(cfg), ==, 3);
	assert_size(dtfl_config_process(cfg), ==, 2);
	assert_string_equal(dtfl_config_address(cfg, 0), "h0:7000");
	assert_string_equal(dtfl_config_address(cfg, 1), "h1:7001");
	/* Several whitespace-separated listening addresses stay one line. */
	assert_string_equal(dtfl_config_address(cfg, 2), "h2:7002 h2b:7003");
	dtfl_config_free(cfg);
	unlink(path);

	/* Fewer lines than -n is an error, as upstream. */
	path = write_hostfile("h0:1\nh1:2");
	assert_not_null(path);
	{
		char *const argv[] = { "p", "-n", "3", "--hostfile", path, NULL };

		assert_int(parse(&cfg, rest, &nrest, argv), ==, DTFL_EINVAL);
	}
	assert_not_null(strstr(dtfl_last_error_detail(),
	    "could only read 2 addresses"));
	dtfl_config_free(cfg);
	unlink(path);

	/* A last line without a newline counts. */
	path = write_hostfile("h0:1\nh1:2");
	{
		char *const argv[] = { "p", "-n2", "-h", path, NULL };

		assert_int(parse(&cfg, rest, &nrest, argv), ==, 0);
	}
	assert_string_equal(dtfl_config_address(cfg, 1), "h1:2");
	dtfl_config_free(cfg);
	unlink(path);

	/* A missing file is an I/O error. */
	{
		char *const argv[] = { "p", "-n", "2", "-h",
		    "/nonexistent/dtfl-hosts", NULL };

		assert_int(parse(&cfg, rest, &nrest, argv), ==, DTFL_EIO);
	}
	dtfl_config_free(cfg);
	/* -h without -n > 1 is not read, as upstream. */
	{
		char *const argv[] = { "p", "-h", "/nonexistent/dtfl-hosts", NULL };

		assert_int(parse(&cfg, rest, &nrest, argv), ==, 0);
	}
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_THREAD);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

DTFL_TEST(config_from_args_replaces_cluster)
{
	static char *const c2[] = { "p", "-n", "2", NULL };
	static char *const w3[] = { "p", "-w", "3", NULL };
	char *rest[4];
	dtfl_config *cfg;
	int nrest;

	(void)params; (void)data;
	assert_int(parse(&cfg, rest, &nrest, c2), ==, 0);
	dtfl_config_set_seed(cfg, 5);
	assert_int(dtfl_config_from_args(cfg, 3, w3, rest, &nrest), ==, 0);
	/* A whole new communication config; the seed is not one of its fields. */
	assert_int(dtfl_config_mode(cfg), ==, DTFL_COMM_PROCESS);
	assert_size(dtfl_config_processes(cfg), ==, 1);
	assert_null(dtfl_config_address(cfg, 0));
	assert_uint64(dtfl_config_seed(cfg), ==, 5);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

static const char *
fake_lookup(const char *name, void *ctx)
{
	int *calls = ctx;

	(*calls)++;
	if (strcmp(name, "TIMELY_WORKER_LOG_ADDR") == 0)
		return "127.0.0.1:51317";
	return NULL;
}

DTFL_TEST(config_null_arguments)
{
	dtfl_config *cfg = dtfl_config_new();
	char *rest[1];
	int nrest;

	(void)params; (void)data;
	DTFL_EXPECT_PANIC(__dtfl_panic("hook installed"));	/* harness check */
	assert_int(dtfl_config_from_args(NULL, 0, NULL, rest, &nrest), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_config_from_args(cfg, 0, NULL, NULL, &nrest), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_config_from_args(cfg, 0, NULL, rest, &nrest), ==, 0);
	assert_int(nrest, ==, 0);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

DTFL_TEST(config_from_env_uses_lookup)
{
	dtfl_config *cfg = dtfl_config_new();
	int calls = 0;

	(void)params; (void)data;
	assert_int(dtfl_config_from_env(cfg, NULL, NULL), ==, DTFL_EINVAL);
	assert_int(dtfl_config_from_env(cfg, fake_lookup, &calls), ==, 0);
	assert_int(calls, ==, 2);
	assert_string_equal(dtfl_config_timely_log_addr(cfg), "127.0.0.1:51317");
	assert_null(dtfl_config_differential_log_addr(cfg));
	/* Applying it again replaces the copy without leaking. */
	assert_int(dtfl_config_from_env(cfg, fake_lookup, &calls), ==, 0);
	assert_string_equal(dtfl_config_timely_log_addr(cfg), "127.0.0.1:51317");
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(config_defaults),
	DTFL_T(config_setters_validate),
	DTFL_T(config_from_args_thread_forms),
	DTFL_T(config_from_args_modes),
	DTFL_T(config_from_args_rest_in_order),
	DTFL_T(config_from_args_rest_may_alias_argv),
	DTFL_T(config_from_args_errors),
	DTFL_T(config_from_args_hostfile),
	DTFL_T(config_from_args_replaces_cluster),
	DTFL_T(config_null_arguments),
	DTFL_T(config_from_env_uses_lookup),
	DTFL_T_END
};

DTFL_TEST_MAIN("config", tests)
