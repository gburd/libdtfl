# SPDX-License-Identifier: MIT
# src/timely: activations, the worker, scopes, execution, timely logging
# (P3); the dataflow layer and operators land here in P4.

timely_sources = \
	src/timely/worker_int.h \
	src/timely/scope_int.h \
	src/timely/timely_int.h \
	src/timely/log_codec_int.h \
	src/timely/ops_src_int.h \
	src/timely/activate.c \
	src/timely/worker.c \
	src/timely/timely_log.c \
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
	src/timely/builder_raw.c \
	src/timely/builder.c \
	src/timely/ops_input.c \
	src/timely/ops_unordered_input.c \
	src/timely/ops_probe.c \
	src/timely/ops_feedback.c \
	src/timely/ops_reclock.c

libdtfl_la_SOURCES += $(timely_sources)
libdtfl_internal_la_SOURCES += $(timely_sources)
dtflinclude_HEADERS += include/dtfl/timely.h
dtflinclude_HEADERS += include/dtfl/timely_ops.h
dist_man_MANS += man/man3/dtfl_execute.3
dist_man_MANS += man/man3/dtfl_worker_dataflow.3
dist_man_MANS += man/man3/dtfl_scope_scoped.3
dist_man_MANS += man/man3/dtfl_op_builder_new.3
dist_man_MANS += man/man3/dtfl_op_input_next.3
dist_man_MANS += man/man3/dtfl_cap_delayed.3
dist_man_MANS += man/man3/dtfl_stream_scope.3
dist_man_MANS += man/man3/dtfl_activator_activate.3
dist_man_MANS += man/man3/dtfl_pact_pipeline.3
dist_man_MANS += man/man3/dtfl_input_new.3
dist_man_MANS += man/man3/dtfl_unordered_input_new.3
dist_man_MANS += man/man3/dtfl_probe_new.3
dist_man_MANS += man/man3/dtfl_feedback_new.3
dist_man_MANS += man/man3/dtfl_stream_reclock.3
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
