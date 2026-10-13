# SPDX-License-Identifier: MIT
# bench/local.mk: performance benchmarks (PLAN section 9).  These are
# noinst_PROGRAMs: built (so distcheck proves they compile) but not run by
# make check.  They benchmark the internal trace batch backings directly, so
# they link the internal convenience library the unit tests use, not the
# installed shared library.  They run on dedicated hosts through the
# aws-benchmark skill, never in CI.

BENCH_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include -I$(top_srcdir)/src \
	-Wno-pedantic
BENCH_LDADD = libdtfl_internal.la

noinst_PROGRAMS += bench/chunk_bench bench/spines

bench_chunk_bench_SOURCES = bench/chunk_bench.c
bench_chunk_bench_CFLAGS = $(BENCH_CFLAGS)
bench_chunk_bench_LDADD = $(BENCH_LDADD)

bench_spines_SOURCES = bench/spines.c
bench_spines_CFLAGS = $(BENCH_CFLAGS)
bench_spines_LDADD = $(BENCH_LDADD)
