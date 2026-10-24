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

noinst_PROGRAMS += bench/chunk_bench bench/spines bench/trace_spill

# The experiments benchmarks drive whole dataflows through the public API
# (PLAN section 9 parity table), so they link the installed library.
noinst_PROGRAMS += bench/experiments/arrange bench/experiments/graphs \
	bench/experiments/graphs-static bench/longrun bench/idle_merge \
	bench/ivm_latency
bench_ivm_latency_SOURCES = bench/ivm_latency.c
bench_ivm_latency_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
bench_ivm_latency_LDADD = libdtfl.la
bench_idle_merge_SOURCES = bench/idle_merge.c
bench_idle_merge_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
bench_idle_merge_LDADD = libdtfl.la
bench_longrun_SOURCES = bench/longrun.c
bench_longrun_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
bench_longrun_LDADD = libdtfl.la
bench_experiments_arrange_SOURCES = bench/experiments/arrange.c
bench_experiments_arrange_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
bench_experiments_arrange_LDADD = libdtfl.la

# Smoke test of the arrange experiment's modes and output format (not a
# measurement): tiny sizes, about a second per run.
dist_check_SCRIPTS += tests/dist/test_bench_arrange.sh
TESTS += tests/dist/test_bench_arrange.sh

bench_experiments_graphs_SOURCES = bench/experiments/graphs.c
bench_experiments_graphs_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
bench_experiments_graphs_LDADD = libdtfl.la

bench_experiments_graphs_static_SOURCES = bench/experiments/graphs-static.c
bench_experiments_graphs_static_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
bench_experiments_graphs_static_LDADD = libdtfl.la

bench_chunk_bench_SOURCES = bench/chunk_bench.c
bench_chunk_bench_CFLAGS = $(BENCH_CFLAGS)
bench_chunk_bench_LDADD = $(BENCH_LDADD)

bench_spines_SOURCES = bench/spines.c
bench_spines_CFLAGS = $(BENCH_CFLAGS)
bench_spines_LDADD = $(BENCH_LDADD)

bench_trace_spill_SOURCES = bench/trace_spill.c
bench_trace_spill_CFLAGS = $(BENCH_CFLAGS)
bench_trace_spill_LDADD = $(BENCH_LDADD)
