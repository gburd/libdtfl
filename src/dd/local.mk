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
	src/dd/arrangement.c \
	src/dd/upsert.c \
	src/dd/reduce.c \
	src/dd/count.c \
	src/dd/threshold.c \
	src/dd/common.c \
	src/dd/common_int.h \
	src/dd/capture.c \
	src/dd/join.c \
	src/dd/iterate.c \
	src/dd/dynamic.c \
	src/dd/dd_server.c \
	src/dd/wrap_int.h \
	src/dd/trace/trace_int.h \
	src/dd/trace/chunk_int.h \
	src/dd/trace/description.c \
	src/dd/trace/layout.c \
	src/dd/trace/cursor.c \
	src/dd/trace/cursor_list.c \
	src/dd/trace/ord_neu.c \
	src/dd/trace/chunk.c \
	src/dd/trace/chunk_vec.c \
	src/dd/trace/spill.c \
	src/dd/trace/merge_batcher.c \
	src/dd/trace/chunker.c \
	src/dd/trace/spine_fueled.c \
	src/dd/trace/wrap_frontier.c \
	src/dd/trace/wrap_enter.c \
	src/dd/trace/wrap_enter_at.c \
	src/dd/trace/trace.c \
	src/dd/dd_log.c

libdtfl_la_SOURCES += $(dd_sources)
libdtfl_internal_la_SOURCES += $(dd_sources)
dtflinclude_HEADERS += include/dtfl/collection.h
dtflinclude_HEADERS += include/dtfl/arrange.h
dtflinclude_HEADERS += include/dtfl/reduce.h
dtflinclude_HEADERS += include/dtfl/iterate.h
dtflinclude_HEADERS += include/dtfl/dd_capture.h
dtflinclude_HEADERS += include/dtfl/dd_server.h
EXTRA_DIST += src/dd/exports.sym

dist_man_MANS += man/man3/dtfl_collection_new.3
dist_man_MANS += man/man3/dtfl_input_session_new.3
dist_man_MANS += man/man3/dtfl_collection_arrange.3
dist_man_MANS += man/man3/dtfl_collection_arrange_by_key.3
dist_man_MANS += man/man3/dtfl_collection_reduce.3
dist_man_MANS += man/man3/dtfl_collection_join.3
dist_man_MANS += man/man3/dtfl_collection_iterate.3
dist_man_MANS += man/man3/dtfl_collection_capture.3
dist_man_MANS += man/man3/dtfl_dd_server.3

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

check_PROGRAMS += tests/unit/test_dd_capture
TESTS += tests/unit/test_dd_capture
tests_unit_test_dd_capture_SOURCES = tests/unit/test_dd_capture.c
tests_unit_test_dd_capture_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_dd_capture_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_update_align
TESTS += tests/unit/test_update_align
tests_unit_test_update_align_SOURCES = tests/unit/test_update_align.c
tests_unit_test_update_align_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_update_align_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_enter_at
TESTS += tests/unit/test_enter_at
tests_unit_test_enter_at_SOURCES = tests/unit/test_enter_at.c
tests_unit_test_enter_at_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_enter_at_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_merge_batcher tests/unit/test_cursor \
	tests/unit/test_ord_neu tests/unit/test_chunk
TESTS += tests/unit/test_merge_batcher tests/unit/test_cursor \
	tests/unit/test_ord_neu tests/unit/test_chunk
tests_unit_test_merge_batcher_SOURCES = tests/unit/test_merge_batcher.c
tests_unit_test_merge_batcher_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_merge_batcher_LDADD = $(INTERNAL_LIBS)
tests_unit_test_cursor_SOURCES = tests/unit/test_cursor.c
tests_unit_test_cursor_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_cursor_LDADD = $(INTERNAL_LIBS)
tests_unit_test_ord_neu_SOURCES = tests/unit/test_ord_neu.c
tests_unit_test_ord_neu_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ord_neu_LDADD = $(INTERNAL_LIBS)
tests_unit_test_chunk_SOURCES = tests/unit/test_chunk.c
tests_unit_test_chunk_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_chunk_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_trace_spill
TESTS += tests/unit/test_trace_spill
tests_unit_test_trace_spill_SOURCES = tests/unit/test_trace_spill.c
tests_unit_test_trace_spill_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_trace_spill_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_chunk_vec
TESTS += tests/unit/test_chunk_vec
tests_unit_test_chunk_vec_SOURCES = tests/unit/test_chunk_vec.c
tests_unit_test_chunk_vec_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_chunk_vec_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/dd/trace
TESTS += tests/upstream/dd/trace
tests_upstream_dd_trace_SOURCES = tests/upstream/dd/trace.c
tests_upstream_dd_trace_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_trace_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/dd/reduce
TESTS += tests/upstream/dd/reduce
tests_upstream_dd_reduce_SOURCES = tests/upstream/dd/reduce.c
tests_upstream_dd_reduce_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_reduce_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_reduce
TESTS += tests/unit/test_reduce
tests_unit_test_reduce_SOURCES = tests/unit/test_reduce.c
tests_unit_test_reduce_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_reduce_LDADD = $(INTERNAL_LIBS)
check_PROGRAMS += tests/upstream/dd/join
TESTS += tests/upstream/dd/join
tests_upstream_dd_join_SOURCES = tests/upstream/dd/join.c
tests_upstream_dd_join_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_join_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/dd/import
TESTS += tests/upstream/dd/import
tests_upstream_dd_import_SOURCES = tests/upstream/dd/import.c
tests_upstream_dd_import_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_import_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/dd/reduce_reference
TESTS += tests/upstream/dd/reduce_reference
tests_upstream_dd_reduce_reference_SOURCES = tests/upstream/dd/reduce_reference.c
tests_upstream_dd_reduce_reference_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_reduce_reference_LDADD = $(INTERNAL_LIBS)

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

check_PROGRAMS += tests/unit/test_arrange
TESTS += tests/unit/test_arrange
tests_unit_test_arrange_SOURCES = tests/unit/test_arrange.c
tests_unit_test_arrange_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_arrange_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_arrange_by_key
TESTS += tests/unit/test_arrange_by_key
tests_unit_test_arrange_by_key_SOURCES = tests/unit/test_arrange_by_key.c
tests_unit_test_arrange_by_key_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_arrange_by_key_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_arrange_lifetime
TESTS += tests/unit/test_arrange_lifetime
tests_unit_test_arrange_lifetime_SOURCES = tests/unit/test_arrange_lifetime.c
tests_unit_test_arrange_lifetime_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_arrange_lifetime_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_iterate
TESTS += tests/unit/test_iterate
tests_unit_test_iterate_SOURCES = tests/unit/test_iterate.c
tests_unit_test_iterate_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_iterate_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_dynamic
TESTS += tests/unit/test_dynamic
tests_unit_test_dynamic_SOURCES = tests/unit/test_dynamic.c
tests_unit_test_dynamic_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_dynamic_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_upsert
TESTS += tests/unit/test_upsert
tests_unit_test_upsert_SOURCES = tests/unit/test_upsert.c
tests_unit_test_upsert_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_upsert_LDADD = $(INTERNAL_LIBS)

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

check_PROGRAMS += tests/pbt/pbt_chunk
TESTS += tests/pbt/pbt_chunk
tests_pbt_pbt_chunk_SOURCES = tests/pbt/pbt_chunk.c
tests_pbt_pbt_chunk_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_chunk_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/pbt/pbt_arrange
TESTS += tests/pbt/pbt_arrange
tests_pbt_pbt_arrange_SOURCES = tests/pbt/pbt_arrange.c
tests_pbt_pbt_arrange_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_arrange_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/pbt/pbt_reduce
TESTS += tests/pbt/pbt_reduce
tests_pbt_pbt_reduce_SOURCES = tests/pbt/pbt_reduce.c
tests_pbt_pbt_reduce_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_reduce_LDADD = $(INTERNAL_PBT_LIBS)
check_PROGRAMS += tests/pbt/pbt_join
TESTS += tests/pbt/pbt_join
tests_pbt_pbt_join_SOURCES = tests/pbt/pbt_join.c
tests_pbt_pbt_join_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_join_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/pbt/pbt_iterate
TESTS += tests/pbt/pbt_iterate
tests_pbt_pbt_iterate_SOURCES = tests/pbt/pbt_iterate.c
tests_pbt_pbt_iterate_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_iterate_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/unit/test_nested_iterate
TESTS += tests/unit/test_nested_iterate
tests_unit_test_nested_iterate_SOURCES = tests/unit/test_nested_iterate.c
tests_unit_test_nested_iterate_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_nested_iterate_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_trace_compaction
TESTS += tests/unit/test_trace_compaction
tests_unit_test_trace_compaction_SOURCES = tests/unit/test_trace_compaction.c
tests_unit_test_trace_compaction_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_trace_compaction_LDADD = $(INTERNAL_LIBS)

# The P14 plugin server: the degr_dist and reachability plugins run over a
# fixed graph and match the hand count, and a shared arrangement is shown to
# be genuinely shared (two importers see one trace).  The example plugin
# sources compile into the test (the static-registration model: a plugin is
# a struct of callbacks, dd_server.h), with examples/server on the include
# path for plugins.h.
check_PROGRAMS += tests/unit/test_dd_server
TESTS += tests/unit/test_dd_server
tests_unit_test_dd_server_SOURCES = tests/unit/test_dd_server.c \
	examples/server/plugins.h \
	examples/server/degr_dist.c \
	examples/server/reachability.c
tests_unit_test_dd_server_CFLAGS = $(TEST_CFLAGS) \
	-I$(top_srcdir)/examples/server
tests_unit_test_dd_server_LDADD = $(INTERNAL_LIBS)
