# SPDX-License-Identifier: MIT
# src/timely: activations, the worker, scopes, execution, timely logging
# (P3); the dataflow layer and operators land here in P4.

timely_sources = \
	src/timely/worker_int.h \
	src/timely/scope_int.h \
	src/timely/timely_int.h \
	src/timely/log_codec_int.h \
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
	src/timely/builder.c

libdtfl_la_SOURCES += $(timely_sources)
libdtfl_internal_la_SOURCES += $(timely_sources)
dtflinclude_HEADERS += include/dtfl/timely.h
dist_man_MANS += man/man3/dtfl_execute.3
dist_man_MANS += man/man3/dtfl_worker_dataflow.3
dist_man_MANS += man/man3/dtfl_scope_scoped.3
EXTRA_DIST += src/timely/exports.sym

check_PROGRAMS += tests/unit/test_worker
TESTS += tests/unit/test_worker
tests_unit_test_worker_SOURCES = tests/unit/test_worker.c
tests_unit_test_worker_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_worker_LDADD = $(INTERNAL_LIBS)
