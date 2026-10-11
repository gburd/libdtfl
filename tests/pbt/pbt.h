/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt.h: hegel property-test harness (libhegel, in-process).
 *
 * A suite defines properties of type pbt_fn and a pbt_prop table, and
 * ends with PBT_MAIN("suite", props).  Inside a property:
 *
 *   int64_t x = pbt_int(tc, lo, hi);   draw an integer in [lo, hi]
 *   bool b = pbt_bool(tc);
 *   PBT_ASSERT(cond);                  counterexample if false
 *   PBT_ASSUME(cond);                  discard the case if false
 *
 * Without --with-hegel each suite prints SKIP with the number of
 * properties left unverified and exits 77 (automake's SKIP status), so
 * a green `make check` never counts an unrun property (AGENTS.md
 * section 4).  The harness follows libxtc's tests/pbt/pbt_common.h,
 * including its rule that an assertion is INTERESTING and an assumption
 * is INVALID; mixing the two up makes a property pass vacuously.
 */
#ifndef DTFL_PBT_H
#define DTFL_PBT_H

#include "config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct pbt_tc pbt_tc;
typedef void (*pbt_fn)(pbt_tc *tc);

typedef struct pbt_prop {
	const char *name;
	pbt_fn fn;
	int cases;	/* 0 = default (200) */
} pbt_prop;

#if defined(DTFL_HAVE_HEGEL)

#include <setjmp.h>

#include "hegel.h"

struct pbt_tc {
	hegel_context_t *ctx;
	hegel_test_case_t *tc;
	jmp_buf jmp;
	int status;
	const char *origin;
};

#define PBT_STR2(x) #x
#define PBT_STR(x) PBT_STR2(x)

static inline void
pbt_unwind(pbt_tc *tc, int status, const char *origin)
{
	tc->status = status;
	tc->origin = origin;
	longjmp(tc->jmp, 1);
}

#define PBT_ASSERT(cond)						\
	do {								\
		if (!(cond))						\
			pbt_unwind(tc, HEGEL_STATUS_INTERESTING,	\
			    __FILE__ ":" PBT_STR(__LINE__));		\
	} while (0)

#define PBT_ASSUME(cond)						\
	do {								\
		if (!(cond))						\
			pbt_unwind(tc, HEGEL_STATUS_INVALID,		\
			    __FILE__ ":" PBT_STR(__LINE__));		\
	} while (0)

static inline int64_t
pbt_int(pbt_tc *tc, int64_t lo, int64_t hi)
{
	int64_t v = 0;

	if (hegel_generate_integer(tc->ctx, tc->tc, lo, hi, &v) != HEGEL_OK)
		pbt_unwind(tc, HEGEL_STATUS_OVERRUN, "draw-overrun");
	return v;
}

static inline bool
pbt_bool(pbt_tc *tc)
{
	return pbt_int(tc, 0, 1) != 0;
}

static int
pbt_run_one(hegel_context_t *ctx, const char *suite, const pbt_prop *p)
{
	hegel_settings_t *settings = NULL;
	hegel_run_t *run = NULL;
	hegel_run_result_t *res = NULL;
	hegel_run_status_t st = HEGEL_RUN_STATUS_PASSED;
	volatile unsigned long nvalid = 0, ninvalid = 0;
	volatile int failed = 0;
	static pbt_tc cur;

	if (hegel_settings_new(ctx, &settings) != HEGEL_OK) {
		printf("[PBT] %s/%s ERROR: settings_new\n", suite, p->name);
		return 1;
	}
	(void)hegel_settings_set_test_cases(ctx, settings,
	    (uint64_t)(p->cases > 0 ? p->cases : 200));
	if (getenv("DTFL_PBT_SEED") != NULL)
		(void)hegel_settings_set_seed(ctx, settings, true,
		    strtoull(getenv("DTFL_PBT_SEED"), NULL, 0));
	if (hegel_run_start(ctx, settings, NULL, NULL, &run) != HEGEL_OK) {
		printf("[PBT] %s/%s ERROR: run_start\n", suite, p->name);
		(void)hegel_settings_free(ctx, settings);
		return 1;
	}
	for (;;) {
		hegel_test_case_t *htc = NULL;

		if (hegel_next_test_case(ctx, run, &htc) != HEGEL_OK) {
			failed = 1;
			break;
		}
		if (htc == NULL)
			break;
		memset(&cur, 0, sizeof cur);
		cur.ctx = ctx;
		cur.tc = htc;
		cur.status = HEGEL_STATUS_VALID;
		if (setjmp(cur.jmp) == 0)
			p->fn(&cur);
		if (cur.status == HEGEL_STATUS_VALID)
			nvalid++;
		else if (cur.status == HEGEL_STATUS_INVALID)
			ninvalid++;
		(void)hegel_mark_complete(ctx, htc, (uint32_t)cur.status,
		    cur.origin != NULL ? cur.origin : p->name);
		(void)hegel_test_case_free(ctx, htc);
	}
	if (!failed && hegel_run_result(ctx, run, &res) == HEGEL_OK) {
		(void)hegel_run_result_status(ctx, res, &st);
		if (st == HEGEL_RUN_STATUS_PASSED) {
			printf("[PBT] %s/%s OK (%lu valid, %lu filtered)\n",
			    suite, p->name, (unsigned long)nvalid,
			    (unsigned long)ninvalid);
		} else {
			size_t nf = 0, i;

			(void)hegel_run_result_failure_count(ctx, res, &nf);
			printf("[PBT] %s/%s FAIL (%zu failure(s))\n", suite,
			    p->name, nf);
			for (i = 0; i < nf; i++) {
				hegel_failure_t *f = NULL;
				const char *o = NULL;

				if (hegel_run_result_failure(ctx, res, i, &f) !=
				    HEGEL_OK)
					continue;
				(void)hegel_failure_origin(ctx, f, &o);
				printf("    at %s\n", o != NULL ? o : "?");
				(void)hegel_failure_free(ctx, f);
			}
			failed = 1;
		}
		(void)hegel_run_result_free(ctx, res);
	} else {
		printf("[PBT] %s/%s ERROR: run_result\n", suite, p->name);
		failed = 1;
	}
	(void)hegel_run_free(ctx, run);
	(void)hegel_settings_free(ctx, settings);
	return failed;
}

#define PBT_MAIN(suite, props)						\
	int main(void)							\
	{								\
		hegel_context_t *ctx = hegel_context_new();		\
		size_t i;						\
		int fails = 0;						\
		if (ctx == NULL) {					\
			printf("[PBT] %s ERROR: no context\n", suite);	\
			return 99;					\
		}							\
		for (i = 0; i < sizeof(props) / sizeof(props[0]); i++)	\
			fails += pbt_run_one(ctx, suite, &props[i]);	\
		(void)hegel_context_free(ctx);				\
		return fails != 0;					\
	}

#else /* !DTFL_HAVE_HEGEL */

/* Compile the property bodies so they cannot rot, but never run them. */
struct pbt_tc { int unused; };
#define PBT_ASSERT(cond) ((void)(cond))
#define PBT_ASSUME(cond) ((void)(cond))
static inline int64_t pbt_int(pbt_tc *tc, int64_t lo, int64_t hi)
{ (void)tc; (void)hi; return lo; }
static inline bool pbt_bool(pbt_tc *tc) { (void)tc; return false; }

#define PBT_MAIN(suite, props)						\
	int main(void)							\
	{								\
		printf("[PBT] %s SKIP (configure --with-hegel to run); "	\
		    "%zu properties unverified\n", suite,		\
		    sizeof(props) / sizeof(props[0]));			\
		return 77;						\
	}

#endif /* DTFL_HAVE_HEGEL */

#endif /* DTFL_PBT_H */
