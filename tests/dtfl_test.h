/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/dtfl_test.h: shared scaffolding for libdtfl unit tests.
 *
 * Each tests/unit/test_*.c defines MunitTest entries and ends with
 * DTFL_TEST_MAIN("name", tests).  Test function names follow the test
 * ledger: <module>__<upstream name> for ported tests (PLAN D6).
 *
 * DTFL_EXPECT_PANIC(stmt) runs stmt with a panic hook that longjmps
 * back, and asserts the hook fired.  It ports #[should_panic].
 */
#ifndef DTFL_TEST_H
#define DTFL_TEST_H

#define MUNIT_ENABLE_ASSERT_ALIASES
#include "munit.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "dtfl/base.h"

static jmp_buf dtfl_test_panic_jmp;
static int dtfl_test_panic_armed;
static char dtfl_test_panic_msg[1024];

static void
dtfl_test_panic_hook(const char *msg, void *ctx)
{
	(void)ctx;
	snprintf(dtfl_test_panic_msg, sizeof dtfl_test_panic_msg, "%s", msg);
	if (dtfl_test_panic_armed) {
		dtfl_test_panic_armed = 0;
		longjmp(dtfl_test_panic_jmp, 1);
	}
	fprintf(stderr, "unexpected libdtfl panic: %s\n", msg);
	abort();
}

/*
 * Assert that `stmt` panics.  The statement must not hold resources
 * that need cleanup after the panic; each test runs in a forked child
 * (munit's default), so leaked state does not affect other tests.
 */
#define DTFL_EXPECT_PANIC(stmt)						\
	do {								\
		dtfl_set_panic_hook(dtfl_test_panic_hook, NULL);	\
		dtfl_test_panic_msg[0] = '\0';				\
		dtfl_test_panic_armed = 1;				\
		if (setjmp(dtfl_test_panic_jmp) == 0) {			\
			stmt;						\
			dtfl_test_panic_armed = 0;			\
			munit_error("expected a panic from: " #stmt);	\
		}							\
		dtfl_set_panic_hook(NULL, NULL);			\
	} while (0)

#define DTFL_TEST(name) \
	static MunitResult name(const MunitParameter params[], void *data)

#define DTFL_T(name) { "/" #name, name, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
#define DTFL_T_END { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }

#define DTFL_TEST_MAIN(suite_name, tests)				\
	int main(int argc, char *argv[])				\
	{								\
		static const MunitSuite suite = {			\
			"/" suite_name, tests, NULL, 1,			\
			MUNIT_SUITE_OPTION_NONE				\
		};							\
		return munit_suite_main(&suite, NULL, argc, argv);	\
	}

#endif /* DTFL_TEST_H */
