# SPDX-License-Identifier: MIT
# src/timely: activations, the worker, scopes, execution, timely logging
# (P3); the dataflow layer and operators land here in P4.

timely_sources = \
	src/timely/worker_int.h \
	src/timely/scope_int.h \
	src/timely/timely_int.h \
	src/timely/log_codec_int.h \
	src/timely/ops_src_int.h \
	src/timely/capture_int.h \
	src/timely/activate.c \
	src/timely/worker.c \
	src/timely/timely_log.c \
	src/timely/log.c \
	src/timely/scope.c \
	src/timely/execute.c \
	src/timely/message.c \
	src/timely/push_tee.c \
	src/timely/push_counter.c \
	src/timely/pull_counter.c \
	src/timely/push_exchange.c \
	src/timely/push_progress.c \
	src/timely/pact.c \
	src/timely/stream.c \
	src/timely/capability.c \
	src/timely/handles.c \
	src/timely/ops_int.h \
	src/timely/ops_map.c \
	src/timely/ops_filter.c \
	src/timely/ops_inspect.c \
	src/timely/ops_concat.c \
	src/timely/ops_exchange.c \
	src/timely/ops_ok_err.c \
	src/timely/ops_partition.c \
	src/timely/ops_to_stream.c \
	src/timely/ops_count.c \
	src/timely/builder_raw.c \
	src/timely/builder.c \
	src/timely/ops_input.c \
	src/timely/ops_unordered_input.c \
	src/timely/ops_probe.c \
	src/timely/ops_feedback.c \
	src/timely/ops_reclock.c \
	src/timely/ops_notify_int.h \
	src/timely/notificator.c \
	src/timely/operator.c \
	src/timely/ops_delay.c \
	src/timely/ops_aggregate.c \
	src/timely/ops_state_machine.c \
	src/timely/ops_branch.c \
	src/timely/ops_broadcast.c \
	src/timely/ops_flow_controlled.c \
	src/timely/ops_enterleave.c \
	src/timely/ops_result.c \
	src/timely/sync.c \
	src/timely/event.c \
	src/timely/capture.c \
	src/timely/extract.c \
	src/timely/replay.c

libdtfl_la_SOURCES += $(timely_sources)
libdtfl_internal_la_SOURCES += $(timely_sources)
dtflinclude_HEADERS += include/dtfl/timely.h
dtflinclude_HEADERS += include/dtfl/timely_ops.h
dtflinclude_HEADERS += include/dtfl/log.h
dist_man_MANS += man/man3/dtfl_execute.3
dist_man_MANS += man/man3/dtfl_worker_dataflow.3
dist_man_MANS += man/man3/dtfl_scope_scoped.3
dist_man_MANS += man/man3/dtfl_op_builder_new.3
dist_man_MANS += man/man3/dtfl_op_input_next.3
dist_man_MANS += man/man3/dtfl_cap_delayed.3
dist_man_MANS += man/man3/dtfl_stream_scope.3
dist_man_MANS += man/man3/dtfl_activator_activate.3
dist_man_MANS += man/man3/dtfl_pact_pipeline.3
dist_man_MANS += man/man3/dtfl_stream_map.3
dist_man_MANS += man/man3/dtfl_stream_concat.3
dist_man_MANS += man/man3/dtfl_stream_exchange.3
dist_man_MANS += man/man3/dtfl_stream_ok_err.3
dist_man_MANS += man/man3/dtfl_stream_partition.3
dist_man_MANS += man/man3/dtfl_stream_count.3
dist_man_MANS += man/man3/dtfl_to_stream.3
dist_man_MANS += man/man3/dtfl_input_new.3
dist_man_MANS += man/man3/dtfl_unordered_input_new.3
dist_man_MANS += man/man3/dtfl_probe_new.3
dist_man_MANS += man/man3/dtfl_feedback_new.3
dist_man_MANS += man/man3/dtfl_stream_reclock.3
dist_man_MANS += man/man3/dtfl_notificator_notify_at.3
dist_man_MANS += man/man3/dtfl_stream_unary_notify.3
dist_man_MANS += man/man3/dtfl_stream_delay.3
dist_man_MANS += man/man3/dtfl_stream_aggregate.3
dist_man_MANS += man/man3/dtfl_stream_state_machine.3
dist_man_MANS += man/man3/dtfl_stream_branch.3
dist_man_MANS += man/man3/dtfl_stream_broadcast.3
dist_man_MANS += man/man3/dtfl_iterator_source.3
dist_man_MANS += man/man3/dtfl_stream_capture.3
dist_man_MANS += man/man3/dtfl_event_link_replay.3
dist_man_MANS += man/man3/dtfl_event_link_extract.3
dist_man_MANS += man/man3/dtfl_event_writer_new.3
dist_man_MANS += man/man3/dtfl_stream_enter.3
dist_man_MANS += man/man3/dtfl_stream_result.3
dist_man_MANS += man/man3/dtfl_barrier.3
dist_man_MANS += man/man3/dtfl_sequencer.3
dist_man_MANS += man/man3/dtfl_worker_log_subscribe.3
EXTRA_DIST += src/timely/exports.sym

check_PROGRAMS += tests/unit/test_worker
TESTS += tests/unit/test_worker
tests_unit_test_worker_SOURCES = tests/unit/test_worker.c
tests_unit_test_worker_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_worker_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_op
TESTS += tests/unit/test_op
tests_unit_test_op_SOURCES = tests/unit/test_op.c
tests_unit_test_op_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_op_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_exchange
TESTS += tests/pbt/pbt_exchange
tests_pbt_pbt_exchange_SOURCES = tests/pbt/pbt_exchange.c
tests_pbt_pbt_exchange_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_exchange_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/unit/test_ops_data
TESTS += tests/unit/test_ops_data
tests_unit_test_ops_data_SOURCES = tests/unit/test_ops_data.c
tests_unit_test_ops_data_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ops_data_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_ops_data
TESTS += tests/pbt/pbt_ops_data
tests_pbt_pbt_ops_data_SOURCES = tests/pbt/pbt_ops_data.c
tests_pbt_pbt_ops_data_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_ops_data_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/unit/test_builder tests/unit/test_stream
TESTS += tests/unit/test_builder tests/unit/test_stream
tests_unit_test_builder_SOURCES = tests/unit/test_builder.c
tests_unit_test_builder_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_builder_LDADD = $(INTERNAL_LIBS)
tests_unit_test_stream_SOURCES = tests/unit/test_stream.c
tests_unit_test_stream_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_stream_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_ops_probe tests/unit/test_input
TESTS += tests/unit/test_ops_probe tests/unit/test_input
tests_unit_test_ops_probe_SOURCES = tests/unit/test_ops_probe.c
tests_unit_test_ops_probe_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ops_probe_LDADD = $(INTERNAL_LIBS)
tests_unit_test_input_SOURCES = tests/unit/test_input.c
tests_unit_test_input_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_input_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_input
TESTS += tests/pbt/pbt_input
tests_pbt_pbt_input_SOURCES = tests/pbt/pbt_input.c
tests_pbt_pbt_input_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_input_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/unit/test_notificator tests/unit/test_ops_notify
TESTS += tests/unit/test_notificator tests/unit/test_ops_notify
tests_unit_test_notificator_SOURCES = tests/unit/test_notificator.c
tests_unit_test_notificator_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_notificator_LDADD = $(INTERNAL_LIBS)
tests_unit_test_ops_notify_SOURCES = tests/unit/test_ops_notify.c
tests_unit_test_ops_notify_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ops_notify_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_ops_notify
TESTS += tests/pbt/pbt_ops_notify
tests_pbt_pbt_ops_notify_SOURCES = tests/pbt/pbt_ops_notify.c
tests_pbt_pbt_ops_notify_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_ops_notify_LDADD = $(INTERNAL_PBT_LIBS)
check_PROGRAMS += tests/unit/test_capture
TESTS += tests/unit/test_capture
tests_unit_test_capture_SOURCES = tests/unit/test_capture.c
tests_unit_test_capture_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_capture_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_event
TESTS += tests/unit/test_event
tests_unit_test_event_SOURCES = tests/unit/test_event.c
tests_unit_test_event_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_event_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_ops_enterleave
TESTS += tests/unit/test_ops_enterleave
tests_unit_test_ops_enterleave_SOURCES = tests/unit/test_ops_enterleave.c
tests_unit_test_ops_enterleave_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ops_enterleave_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_ops_result
TESTS += tests/unit/test_ops_result
tests_unit_test_ops_result_SOURCES = tests/unit/test_ops_result.c
tests_unit_test_ops_result_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ops_result_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_sync
TESTS += tests/unit/test_sync
tests_unit_test_sync_SOURCES = tests/unit/test_sync.c
tests_unit_test_sync_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_sync_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_capture
TESTS += tests/pbt/pbt_capture
tests_pbt_pbt_capture_SOURCES = tests/pbt/pbt_capture.c
tests_pbt_pbt_capture_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_capture_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/unit/test_log_subscribe
TESTS += tests/unit/test_log_subscribe
tests_unit_test_log_subscribe_SOURCES = tests/unit/test_log_subscribe.c
tests_unit_test_log_subscribe_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_log_subscribe_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/timely/barrier
TESTS += tests/upstream/timely/barrier
tests_upstream_timely_barrier_SOURCES = tests/upstream/timely/barrier.c
tests_upstream_timely_barrier_CFLAGS = $(TEST_CFLAGS)
tests_upstream_timely_barrier_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/timely/gh_523
TESTS += tests/upstream/timely/gh_523
tests_upstream_timely_gh_523_SOURCES = tests/upstream/timely/gh_523.c
tests_upstream_timely_gh_523_CFLAGS = $(TEST_CFLAGS)
tests_upstream_timely_gh_523_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/timely/shape_scaling
TESTS += tests/upstream/timely/shape_scaling
tests_upstream_timely_shape_scaling_SOURCES = tests/upstream/timely/shape_scaling.c
tests_upstream_timely_shape_scaling_CFLAGS = $(TEST_CFLAGS)
tests_upstream_timely_shape_scaling_LDADD = $(INTERNAL_LIBS)
