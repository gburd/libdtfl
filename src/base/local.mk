# SPDX-License-Identifier: MIT
# src/base: allocation and panic hooks, vectors, bytes, batches, hashing.

base_sources = \
	src/base/base_int.h \
	src/base/batch_int.h \
	src/base/batch.c \
	src/base/bytes.c \
	src/base/hash.c \
	src/base/os_alloc.c \
	src/base/record.c \
	src/base/util.c

libdtfl_la_SOURCES += $(base_sources)
libdtfl_internal_la_SOURCES += $(base_sources)
dtflinclude_HEADERS += include/dtfl/base.h include/dtfl/dtfl.h \
	include/dtfl/record.h
dist_man_MANS += man/man3/dtfl_set_alloc_hooks.3 man/man3/dtfl_strerror.3 \
	man/man3/dtfl_type.3 man/man3/dtfl_put_u64.3 man/man7/dtfl.7
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

check_PROGRAMS += tests/unit/test_record tests/unit/test_bytes tests/unit/test_batch
TESTS += tests/unit/test_record tests/unit/test_bytes tests/unit/test_batch
tests_unit_test_record_SOURCES = tests/unit/test_record.c
tests_unit_test_record_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_record_LDADD = $(INTERNAL_LIBS)
tests_unit_test_bytes_SOURCES = tests/unit/test_bytes.c
tests_unit_test_bytes_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_bytes_LDADD = $(INTERNAL_LIBS)
tests_unit_test_batch_SOURCES = tests/unit/test_batch.c
tests_unit_test_batch_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_batch_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_record tests/pbt/pbt_batch
TESTS += tests/pbt/pbt_record tests/pbt/pbt_batch
tests_pbt_pbt_record_SOURCES = tests/pbt/pbt_record.c
tests_pbt_pbt_record_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_record_LDADD = $(INTERNAL_PBT_LIBS)
tests_pbt_pbt_batch_SOURCES = tests/pbt/pbt_batch.c
tests_pbt_pbt_batch_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_batch_LDADD = $(INTERNAL_PBT_LIBS)

# Thread creation (the one pthread_create; AGENTS.md section 5).
os_thread_sources = src/base/os_thread.h src/base/os_thread.c
libdtfl_la_SOURCES += $(os_thread_sources)
libdtfl_internal_la_SOURCES += $(os_thread_sources)

# Embedding host hooks: logging sink and thread spawn (dtfl_embedding(7)).
dist_man_MANS += man/man3/dtfl_set_log_hook.3 man/man3/dtfl_set_thread_hooks.3 \
	man/man7/dtfl_embedding.7
