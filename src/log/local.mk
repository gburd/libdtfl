# SPDX-License-Identifier: MIT
# src/log: the logging registry (logging/src/lib.rs).  Its source lives in
# src/base/log_registry.c; this fragment exists so src/base/local.mk stays
# owned by one worker at a time.

log_sources = src/base/log_int.h src/base/log_registry.c
libdtfl_la_SOURCES += $(log_sources)
libdtfl_internal_la_SOURCES += $(log_sources)

check_PROGRAMS += tests/unit/test_log
TESTS += tests/unit/test_log
tests_unit_test_log_SOURCES = tests/unit/test_log.c
tests_unit_test_log_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_log_LDADD = $(INTERNAL_LIBS)
