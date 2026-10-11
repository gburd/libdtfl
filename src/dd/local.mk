# SPDX-License-Identifier: MIT
# src/dd: differential dataflow.  P5/P6: diff classes, consolidation,
# trace descriptions.  No public API yet, so no headers, man pages or
# exports.sym.

dd_sources = \
	src/dd/dd_int.h \
	src/dd/diff.c \
	src/dd/consolidate.c \
	src/dd/trace/description.c

libdtfl_la_SOURCES += $(dd_sources)
libdtfl_internal_la_SOURCES += $(dd_sources)

check_PROGRAMS += tests/unit/test_diff tests/unit/test_consolidate
TESTS += tests/unit/test_diff tests/unit/test_consolidate
tests_unit_test_diff_SOURCES = tests/unit/test_diff.c
tests_unit_test_diff_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_diff_LDADD = $(INTERNAL_LIBS)
tests_unit_test_consolidate_SOURCES = tests/unit/test_consolidate.c
tests_unit_test_consolidate_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_consolidate_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_consolidate
TESTS += tests/pbt/pbt_consolidate
tests_pbt_pbt_consolidate_SOURCES = tests/pbt/pbt_consolidate.c
tests_pbt_pbt_consolidate_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_consolidate_LDADD = $(INTERNAL_PBT_LIBS)
