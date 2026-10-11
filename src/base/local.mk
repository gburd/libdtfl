# SPDX-License-Identifier: MIT
# src/base: allocation and panic hooks, vectors, bytes, batches, hashing.

base_sources = \
	src/base/base_int.h \
	src/base/os_alloc.c \
	src/base/util.c

libdtfl_la_SOURCES += $(base_sources)
libdtfl_internal_la_SOURCES += $(base_sources)
dtflinclude_HEADERS += include/dtfl/base.h include/dtfl/dtfl.h
dist_man_MANS += man/man3/dtfl_set_alloc_hooks.3 man/man3/dtfl_strerror.3 \
	man/man7/dtfl.7
EXTRA_DIST += src/base/exports.sym

check_PROGRAMS += tests/unit/test_base
TESTS += tests/unit/test_base
tests_unit_test_base_SOURCES = tests/unit/test_base.c
tests_unit_test_base_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_base_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_smoke
TESTS += tests/pbt/pbt_smoke
tests_pbt_pbt_smoke_SOURCES = tests/pbt/pbt_smoke.c
tests_pbt_pbt_smoke_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_smoke_LDADD = $(INTERNAL_PBT_LIBS)
