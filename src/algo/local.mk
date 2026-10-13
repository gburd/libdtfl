# SPDX-License-Identifier: MIT
# src/algo: graph and sequence algorithms built on the differential
# dataflow collection/join/reduce/iterate stack (P9).  Each algorithm is
# a differential program over collections of u64 node identifiers.

algo_sources = \
	src/algo/bfs.c \
	src/algo/propagate.c \
	src/algo/scc.c \
	src/algo/sequential.c \
	src/algo/bijkstra.c \
	src/algo/identifiers.c \
	src/algo/prefix_sum.c

libdtfl_la_SOURCES += $(algo_sources)
libdtfl_internal_la_SOURCES += $(algo_sources)
dtflinclude_HEADERS += include/dtfl/algo.h
EXTRA_DIST += src/algo/exports.sym

dist_man_MANS += man/man3/dtfl_algo_bfs.3

check_PROGRAMS += tests/upstream/dd/bfs
TESTS += tests/upstream/dd/bfs
tests_upstream_dd_bfs_SOURCES = tests/upstream/dd/bfs.c
tests_upstream_dd_bfs_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_bfs_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/upstream/dd/scc
TESTS += tests/upstream/dd/scc
tests_upstream_dd_scc_SOURCES = tests/upstream/dd/scc.c
tests_upstream_dd_scc_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dd_scc_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_identifiers
TESTS += tests/unit/test_identifiers
tests_unit_test_identifiers_SOURCES = tests/unit/test_identifiers.c
tests_unit_test_identifiers_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_identifiers_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_bfs
TESTS += tests/pbt/pbt_bfs
tests_pbt_pbt_bfs_SOURCES = tests/pbt/pbt_bfs.c
tests_pbt_pbt_bfs_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_bfs_LDADD = $(INTERNAL_PBT_LIBS)
