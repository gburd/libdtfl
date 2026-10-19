# SPDX-License-Identifier: MIT
# examples/local.mk: the ported timely and differential examples
# (AGENTS.md section 11).  Each example is a noinst_PROGRAM that links the
# built shared library through its public headers only.  A representative
# subset also runs under `make check` as a smoke test (TESTS below), with
# a tiny fixed input, so a crashing or hanging example fails the build.
# The check harness caps every run with a timeout.

EXAMPLE_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include -I$(top_srcdir)/examples/timely
EXAMPLE_LDADD = libdtfl.la

noinst_PROGRAMS += \
	examples/timely/simple \
	examples/timely/hello \
	examples/timely/threadless \
	examples/timely/exchange \
	examples/timely/pingpong \
	examples/timely/distinct \
	examples/timely/unordered_input \
	examples/timely/capture \
	examples/timely/sequence \
	examples/timely/barrier \
	examples/timely/flow_controlled \
	examples/timely/event_driven \
	examples/timely/openloop \
	examples/timely/spill_compare \
	examples/timely/loopdemo \
	examples/timely/unionfind \
	examples/timely/bfs \
	examples/timely/pagerank \
	examples/timely/wordcount \
	examples/timely/hashjoin \
	examples/timely/logging-send \
	examples/timely/capture_send \
	examples/timely/capture_recv

examples_timely_simple_SOURCES = examples/timely/simple.c
examples_timely_simple_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_simple_LDADD = $(EXAMPLE_LDADD)

examples_timely_hello_SOURCES = examples/timely/hello.c
examples_timely_hello_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_hello_LDADD = $(EXAMPLE_LDADD)

examples_timely_threadless_SOURCES = examples/timely/threadless.c
examples_timely_threadless_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_threadless_LDADD = $(EXAMPLE_LDADD)

examples_timely_exchange_SOURCES = examples/timely/exchange.c
examples_timely_exchange_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_exchange_LDADD = $(EXAMPLE_LDADD)

examples_timely_pingpong_SOURCES = examples/timely/pingpong.c
examples_timely_pingpong_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_pingpong_LDADD = $(EXAMPLE_LDADD)

examples_timely_distinct_SOURCES = examples/timely/distinct.c
examples_timely_distinct_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_distinct_LDADD = $(EXAMPLE_LDADD)

examples_timely_unordered_input_SOURCES = examples/timely/unordered_input.c
examples_timely_unordered_input_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_unordered_input_LDADD = $(EXAMPLE_LDADD)

examples_timely_capture_SOURCES = examples/timely/capture.c
examples_timely_capture_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_capture_LDADD = $(EXAMPLE_LDADD)

examples_timely_capture_send_SOURCES = examples/timely/capture_send.c
examples_timely_capture_send_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_capture_send_LDADD = $(EXAMPLE_LDADD)

examples_timely_capture_recv_SOURCES = examples/timely/capture_recv.c
examples_timely_capture_recv_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_capture_recv_LDADD = $(EXAMPLE_LDADD)

examples_timely_sequence_SOURCES = examples/timely/sequence.c
examples_timely_sequence_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_sequence_LDADD = $(EXAMPLE_LDADD)

examples_timely_barrier_SOURCES = examples/timely/barrier.c
examples_timely_barrier_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_barrier_LDADD = $(EXAMPLE_LDADD)

examples_timely_flow_controlled_SOURCES = examples/timely/flow_controlled.c
examples_timely_flow_controlled_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_flow_controlled_LDADD = $(EXAMPLE_LDADD)

examples_timely_event_driven_SOURCES = examples/timely/event_driven.c
examples_timely_event_driven_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_event_driven_LDADD = $(EXAMPLE_LDADD)

examples_timely_openloop_SOURCES = examples/timely/openloop.c
examples_timely_openloop_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_openloop_LDADD = $(EXAMPLE_LDADD)

examples_timely_spill_compare_SOURCES = examples/timely/spill_compare.c
examples_timely_spill_compare_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_spill_compare_LDADD = $(EXAMPLE_LDADD)

examples_timely_loopdemo_SOURCES = examples/timely/loopdemo.c
examples_timely_loopdemo_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_loopdemo_LDADD = $(EXAMPLE_LDADD)

examples_timely_unionfind_SOURCES = examples/timely/unionfind.c
examples_timely_unionfind_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_unionfind_LDADD = $(EXAMPLE_LDADD)

examples_timely_bfs_SOURCES = examples/timely/bfs.c
examples_timely_bfs_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_bfs_LDADD = $(EXAMPLE_LDADD)

examples_timely_pagerank_SOURCES = examples/timely/pagerank.c
examples_timely_pagerank_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_pagerank_LDADD = $(EXAMPLE_LDADD)

examples_timely_wordcount_SOURCES = examples/timely/wordcount.c
examples_timely_wordcount_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_wordcount_LDADD = $(EXAMPLE_LDADD)

examples_timely_hashjoin_SOURCES = examples/timely/hashjoin.c
examples_timely_hashjoin_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_hashjoin_LDADD = $(EXAMPLE_LDADD)

examples_timely_logging_send_SOURCES = examples/timely/logging-send.c
examples_timely_logging_send_CFLAGS = $(EXAMPLE_CFLAGS)
examples_timely_logging_send_LDADD = $(EXAMPLE_LDADD)

# ----------------------------------------------------------------------
# Differential dataflow examples (P9).
# ----------------------------------------------------------------------

noinst_PROGRAMS += \
	examples/dd/degrees \
	examples/dd/bfs \
	examples/dd/monoid-bfs \
	examples/dd/projekt \
	examples/dd/compact \
	examples/dd/stackoverflow \
	examples/dd/hello \
	examples/dd/accumulate \
	examples/dd/interpreted \
	examples/dd/itembased_cf \
	examples/dd/scc \
	examples/dd/pagerank \
	examples/dd/dynamic \
	examples/dd/cursors \
	examples/dd/multitemporal \
	examples/dd/arrange \
	examples/dd/graspan \
	examples/dd/progress

examples_dd_degrees_SOURCES = examples/dd/degrees.c
examples_dd_degrees_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_degrees_LDADD = $(EXAMPLE_LDADD)

examples_dd_bfs_SOURCES = examples/dd/bfs.c
examples_dd_bfs_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_bfs_LDADD = $(EXAMPLE_LDADD)

examples_dd_monoid_bfs_SOURCES = examples/dd/monoid-bfs.c
examples_dd_monoid_bfs_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_monoid_bfs_LDADD = $(EXAMPLE_LDADD)

examples_dd_projekt_SOURCES = examples/dd/projekt.c
examples_dd_projekt_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_projekt_LDADD = $(EXAMPLE_LDADD)

examples_dd_compact_SOURCES = examples/dd/compact.c
examples_dd_compact_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_compact_LDADD = $(EXAMPLE_LDADD)

examples_dd_stackoverflow_SOURCES = examples/dd/stackoverflow.c
examples_dd_stackoverflow_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_stackoverflow_LDADD = $(EXAMPLE_LDADD)

examples_dd_hello_SOURCES = examples/dd/hello.c
examples_dd_hello_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_hello_LDADD = $(EXAMPLE_LDADD)

examples_dd_accumulate_SOURCES = examples/dd/accumulate.c
examples_dd_accumulate_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_accumulate_LDADD = $(EXAMPLE_LDADD)

examples_dd_interpreted_SOURCES = examples/dd/interpreted.c
examples_dd_interpreted_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_interpreted_LDADD = $(EXAMPLE_LDADD)

examples_dd_itembased_cf_SOURCES = examples/dd/itembased_cf.c
examples_dd_itembased_cf_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_itembased_cf_LDADD = $(EXAMPLE_LDADD)

examples_dd_scc_SOURCES = examples/dd/scc.c
examples_dd_scc_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_scc_LDADD = $(EXAMPLE_LDADD)

examples_dd_pagerank_SOURCES = examples/dd/pagerank.c
examples_dd_pagerank_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_pagerank_LDADD = $(EXAMPLE_LDADD)

examples_dd_dynamic_SOURCES = examples/dd/dynamic.c
examples_dd_dynamic_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_dynamic_LDADD = $(EXAMPLE_LDADD)

examples_dd_cursors_SOURCES = examples/dd/cursors.c
examples_dd_cursors_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_cursors_LDADD = $(EXAMPLE_LDADD)

examples_dd_multitemporal_SOURCES = examples/dd/multitemporal.c
examples_dd_multitemporal_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_multitemporal_LDADD = $(EXAMPLE_LDADD)

examples_dd_arrange_SOURCES = examples/dd/arrange.c
examples_dd_arrange_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_arrange_LDADD = $(EXAMPLE_LDADD)

examples_dd_graspan_SOURCES = examples/dd/graspan.c
examples_dd_graspan_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_graspan_LDADD = $(EXAMPLE_LDADD)

examples_dd_progress_SOURCES = examples/dd/progress.c
examples_dd_progress_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dd_progress_LDADD = $(EXAMPLE_LDADD)

# ----------------------------------------------------------------------
# dogsdogsdogs (delta-query) examples (P9).
# ----------------------------------------------------------------------

noinst_PROGRAMS += \
	examples/dogs/delta_query_wcoj \
	examples/dogs/delta_query \
	examples/dogs/delta_query2 \
	examples/dogs/ngo \
	examples/dogs/dogsdogsdogs

examples_dogs_delta_query_wcoj_SOURCES = examples/dogs/delta_query_wcoj.c
examples_dogs_delta_query_wcoj_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dogs_delta_query_wcoj_LDADD = $(EXAMPLE_LDADD)

examples_dogs_delta_query_SOURCES = examples/dogs/delta_query.c
examples_dogs_delta_query_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dogs_delta_query_LDADD = $(EXAMPLE_LDADD)

examples_dogs_delta_query2_SOURCES = examples/dogs/delta_query2.c
examples_dogs_delta_query2_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dogs_delta_query2_LDADD = $(EXAMPLE_LDADD)

examples_dogs_ngo_SOURCES = examples/dogs/ngo.c
examples_dogs_ngo_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dogs_ngo_LDADD = $(EXAMPLE_LDADD)

examples_dogs_dogsdogsdogs_SOURCES = examples/dogs/dogsdogsdogs.c
examples_dogs_dogsdogsdogs_CFLAGS = $(EXAMPLE_CFLAGS)
examples_dogs_dogsdogsdogs_LDADD = $(EXAMPLE_LDADD)

# Smoke test: run a representative subset under `make check` so a
# crashing, hanging or wrong-output example fails the build.  The
# examples the harness runs are added to check_PROGRAMS so `make check`
# builds them, and the script runs after them.
check_PROGRAMS += \
	examples/timely/hello \
	examples/timely/exchange \
	examples/timely/wordcount \
	examples/timely/bfs \
	examples/timely/unionfind \
	examples/timely/pagerank \
	examples/timely/hashjoin \
	examples/timely/capture \
	examples/timely/logging-send \
	examples/dd/degrees \
	examples/dd/bfs \
	examples/dd/projekt \
	examples/dd/compact \
	examples/dd/stackoverflow \
	examples/dogs/delta_query_wcoj \
	examples/dogs/delta_query \
	examples/dogs/delta_query2 \
	examples/dogs/ngo \
	examples/dogs/dogsdogsdogs \
	examples/dd/hello \
	examples/dd/accumulate \
	examples/dd/interpreted \
	examples/dd/itembased_cf \
	examples/dd/scc \
	examples/dd/pagerank \
	examples/dd/dynamic \
	examples/dd/cursors \
	examples/dd/multitemporal \
	examples/dd/arrange \
	examples/dd/graspan \
	examples/dd/progress
dist_check_SCRIPTS += tests/dist/test_examples.sh tests/dist/test_examples_dd.sh
TESTS += tests/dist/test_examples.sh tests/dist/test_examples_dd.sh

EXTRA_DIST += examples/timely/example_args.h \
	examples/timely/hello.expected \
	examples/timely/exchange.expected \
	examples/timely/wordcount.expected \
	examples/timely/bfs.expected \
	examples/timely/unionfind.expected \
	examples/timely/pagerank.expected \
	examples/timely/hashjoin.expected \
	examples/timely/capture.expected \
	examples/timely/logging-send.expected \
	examples/timely/spill_compare.expected \
	examples/dd/degrees.expected \
	examples/dd/bfs.expected \
	examples/dd/monoid-bfs.expected \
	examples/dd/projekt.expected \
	examples/dd/compact.expected \
	examples/dd/stackoverflow.expected \
	examples/dogs/delta_query_wcoj.expected \
	examples/dogs/delta_query.expected \
	examples/dogs/delta_query2.expected \
	examples/dogs/ngo.expected \
	examples/dogs/dogsdogsdogs.expected \
	examples/dd/hello.expected \
	examples/dd/accumulate.expected \
	examples/dd/interpreted.expected \
	examples/dd/itembased_cf.expected \
	examples/dd/scc.expected \
	examples/dd/pagerank.expected \
	examples/dd/dynamic.expected \
	examples/dd/cursors.expected \
	examples/dd/multitemporal.expected \
	examples/dd/arrange.expected \
	examples/dd/graspan.expected \
	examples/dd/progress.expected \
	examples/dd/progress-w2.expected

# ----------------------------------------------------------------------
# Diagnostics examples (P11).  smoke runs the full in-process path
# (session + WebSocket server + Frame stream) over a map/probe
# dataflow; scc-bench runs strongly_connected (dtfl_algo_scc) under the
# diagnostics server over a bounded hashed edge stream.
# ----------------------------------------------------------------------
DIAG_EXAMPLE_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include

noinst_PROGRAMS += examples/diag/smoke examples/diag/scc-bench
examples_diag_smoke_SOURCES = examples/diag/smoke.c
examples_diag_smoke_CFLAGS = $(DIAG_EXAMPLE_CFLAGS)
examples_diag_smoke_LDADD = libdtfl.la
examples_diag_scc_bench_SOURCES = examples/diag/scc-bench.c
examples_diag_scc_bench_CFLAGS = $(DIAG_EXAMPLE_CFLAGS)
examples_diag_scc_bench_LDADD = libdtfl.la

check_PROGRAMS += examples/diag/smoke examples/diag/scc-bench
dist_check_SCRIPTS += tests/dist/test_examples_diag.sh
TESTS += tests/dist/test_examples_diag.sh
EXTRA_DIST += examples/diag/smoke.expected examples/diag/scc-bench.expected

# ----------------------------------------------------------------------
# Embedding skeleton: a worked PostgreSQL extension showing the four
# host hooks (alloc->palloc, panic->ereport, log->elog, threads->bgworker
# or the single-thread cooperative mode).  The PostgreSQL symbols are
# stubbed so it builds and runs with no server; the real mapping is in
# each hook (dtfl_embedding(7)).  A noinst example with its own smoke
# test.
# ----------------------------------------------------------------------
noinst_PROGRAMS += examples/embed/pg_extension
examples_embed_pg_extension_SOURCES = examples/embed/pg_extension.c
examples_embed_pg_extension_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
examples_embed_pg_extension_LDADD = libdtfl.la

check_PROGRAMS += examples/embed/pg_extension
dist_check_SCRIPTS += tests/dist/test_examples_embed.sh
TESTS += tests/dist/test_examples_embed.sh
EXTRA_DIST += examples/embed/pg_extension.expected

# ----------------------------------------------------------------------
# Shared-dataflow plugins (P14).  The four example dd_server plugins
# (degr_dist, reachability, neighborhood, random_graph).  Divergence from
# upstream: a plugin is a statically registered dtfl_dataflow_plugin
# (name + build callback), not a dlopened shared object, since library
# code may not dlopen (AGENTS.md section 5, dtfl/dd_server.h).  So the
# plugins are not standalone programs; their sources compile into the
# dtfl-server tool (tools/dtfl-server/local.mk) and the P14 unit test
# (src/dd/local.mk).  Listed here so they ship in a dist tarball even if
# the tool is removed, matching the examples/ layout (AGENTS.md 11).
EXTRA_DIST += \
	examples/server/plugins.h \
	examples/server/degr_dist.c \
	examples/server/reachability.c \
	examples/server/neighborhood.c \
	examples/server/random_graph.c

# ----------------------------------------------------------------------
# Incremental materialized views.  ivm_view maintains a join + GROUP BY
# view one commit at a time and checks every point-in-time read against
# a recomputation; txn_harness models PostgreSQL's transactions and
# transition tables in plain C and applies each committed view delta to
# a toy view table; sqlrow.h encodes SQL column values (NULL, integers,
# float8 with PostgreSQL's NaN and -0 rules) so they compare and group as
# SQL does, and sqlrow_check tests it.  Background:
# dev/notes/pg-incremental-views.md.
# ----------------------------------------------------------------------
noinst_PROGRAMS += examples/ivm/ivm_view examples/ivm/txn_harness \
	examples/ivm/sqlrow_check
examples_ivm_ivm_view_SOURCES = examples/ivm/ivm_view.c
examples_ivm_ivm_view_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
examples_ivm_ivm_view_LDADD = libdtfl.la
examples_ivm_txn_harness_SOURCES = examples/ivm/txn_harness.c
examples_ivm_txn_harness_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
examples_ivm_txn_harness_LDADD = libdtfl.la
examples_ivm_sqlrow_check_SOURCES = examples/ivm/sqlrow_check.c \
	examples/ivm/sqlrow.h
examples_ivm_sqlrow_check_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include
examples_ivm_sqlrow_check_LDADD = libdtfl.la -lm

check_PROGRAMS += examples/ivm/ivm_view examples/ivm/txn_harness \
	examples/ivm/sqlrow_check
dist_check_SCRIPTS += tests/dist/test_examples_ivm.sh
TESTS += tests/dist/test_examples_ivm.sh
EXTRA_DIST += examples/ivm/ivm_view.expected examples/ivm/txn_harness.expected \
	examples/ivm/sqlrow_check.expected
