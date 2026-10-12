# SPDX-License-Identifier: MIT
# src/dd: differential dataflow.  P5: diff classes, consolidation, trace
# descriptions, the Collection type and the differential input session.
# The trace/arrange layer and the join/reduce collection methods land in
# P6 and later.

dd_sources = \
	src/dd/dd_int.h \
	src/dd/collection_int.h \
	src/dd/diff.c \
	src/dd/consolidate.c \
	src/dd/collection.c \
	src/dd/input.c \
	src/dd/trace/description.c

libdtfl_la_SOURCES += $(dd_sources)
libdtfl_internal_la_SOURCES += $(dd_sources)
dtflinclude_HEADERS += include/dtfl/collection.h
EXTRA_DIST += src/dd/exports.sym

dist_man_MANS += man/man3/dtfl_collection_new.3
dist_man_MANS += man/man3/dtfl_input_session_new.3

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
