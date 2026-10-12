# SPDX-License-Identifier: MIT
# src/dd: differential dataflow.  P5: diff classes, consolidation, trace
# descriptions, the Collection type and the differential input session.
# The trace/arrange layer and the join/reduce collection methods land in
# P6 and later.

dd_sources = \
	src/dd/dd_int.h \
	src/dd/collection_int.h \
	src/dd/arrange_int.h \
	src/dd/diff.c \
	src/dd/consolidate.c \
	src/dd/collection.c \
	src/dd/input.c \
	src/dd/writer.c \
	src/dd/agent.c \
	src/dd/wrap_int.h \
	src/dd/trace/trace_int.h \
	src/dd/trace/description.c \
	src/dd/trace/layout.c \
	src/dd/trace/cursor.c \
	src/dd/trace/cursor_list.c \
	src/dd/trace/ord_neu.c \
	src/dd/trace/merge_batcher.c \
	src/dd/trace/chunker.c \
	src/dd/trace/spine_fueled.c \
	src/dd/trace/wrap_frontier.c \
	src/dd/trace/wrap_enter.c \
	src/dd/trace/wrap_enter_at.c \
	src/dd/trace/trace.c

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

check_PROGRAMS += tests/unit/test_collection
TESTS += tests/unit/test_collection
tests_unit_test_collection_SOURCES = tests/unit/test_collection.c
tests_unit_test_collection_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_collection_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_merge_batcher tests/unit/test_cursor \
	tests/unit/test_ord_neu
TESTS += tests/unit/test_merge_batcher tests/unit/test_cursor \
	tests/unit/test_ord_neu
tests_unit_test_merge_batcher_SOURCES = tests/unit/test_merge_batcher.c
tests_unit_test_merge_batcher_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_merge_batcher_LDADD = $(INTERNAL_LIBS)
tests_unit_test_cursor_SOURCES = tests/unit/test_cursor.c
tests_unit_test_cursor_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_cursor_LDADD = $(INTERNAL_LIBS)
tests_unit_test_ord_neu_SOURCES = tests/unit/test_ord_neu.c
tests_unit_test_ord_neu_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ord_neu_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/dd/trace
TESTS += tests/upstream/dd/trace
tests_upstream_dd_trace_SOURCES = tests/upstream/dd/trace.c
tests_upstream_dd_trace_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_trace_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_agent
TESTS += tests/unit/test_agent
tests_unit_test_agent_SOURCES = tests/unit/test_agent.c
tests_unit_test_agent_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_agent_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_wrappers
TESTS += tests/unit/test_wrappers
tests_unit_test_wrappers_SOURCES = tests/unit/test_wrappers.c
tests_unit_test_wrappers_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_wrappers_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_consolidate
TESTS += tests/pbt/pbt_consolidate
tests_pbt_pbt_consolidate_SOURCES = tests/pbt/pbt_consolidate.c
tests_pbt_pbt_consolidate_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_consolidate_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/pbt/pbt_collection
TESTS += tests/pbt/pbt_collection
tests_pbt_pbt_collection_SOURCES = tests/pbt/pbt_collection.c
tests_pbt_pbt_collection_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_collection_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/pbt/pbt_trace
TESTS += tests/pbt/pbt_trace
tests_pbt_pbt_trace_SOURCES = tests/pbt/pbt_trace.c
tests_pbt_pbt_trace_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_trace_LDADD = $(INTERNAL_PBT_LIBS)
