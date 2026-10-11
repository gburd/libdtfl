# SPDX-License-Identifier: MIT
# src/progress: time classes, change batches, antichains (P1); the
# progress tracker lands here in P3.

progress_sources = \
	src/progress/time_int.h \
	src/progress/progress_int.h \
	src/progress/time.c \
	src/progress/change_batch.c \
	src/progress/antichain.c \
	src/progress/operate_int.h \
	src/progress/operate.c \
	src/progress/reachability.c

libdtfl_la_SOURCES += $(progress_sources)
libdtfl_internal_la_SOURCES += $(progress_sources)
dtflinclude_HEADERS += include/dtfl/time.h
dist_man_MANS += man/man3/dtfl_time_class.3 man/man3/dtfl_time_cmp.3 \
	man/man3/dtfl_summary_results_in.3
EXTRA_DIST += src/progress/exports.sym

check_PROGRAMS += tests/unit/test_time tests/unit/test_antichain
TESTS += tests/unit/test_time tests/unit/test_antichain
tests_unit_test_time_SOURCES = tests/unit/test_time.c
tests_unit_test_time_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_time_LDADD = $(INTERNAL_LIBS)
tests_unit_test_antichain_SOURCES = tests/unit/test_antichain.c
tests_unit_test_antichain_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_antichain_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_time tests/pbt/pbt_antichain
TESTS += tests/pbt/pbt_time tests/pbt/pbt_antichain
tests_pbt_pbt_time_SOURCES = tests/pbt/pbt_time.c
tests_pbt_pbt_time_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_time_LDADD = $(INTERNAL_PBT_LIBS)
tests_pbt_pbt_antichain_SOURCES = tests/pbt/pbt_antichain.c
tests_pbt_pbt_antichain_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_antichain_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/unit/test_reachability
TESTS += tests/unit/test_reachability
tests_unit_test_reachability_SOURCES = tests/unit/test_reachability.c
tests_unit_test_reachability_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_reachability_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_reachability
TESTS += tests/pbt/pbt_reachability
tests_pbt_pbt_reachability_SOURCES = tests/pbt/pbt_reachability.c
tests_pbt_pbt_reachability_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_reachability_LDADD = $(INTERNAL_PBT_LIBS)
