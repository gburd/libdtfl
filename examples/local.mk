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
	examples/timely/loopdemo \
	examples/timely/unionfind \
	examples/timely/bfs \
	examples/timely/pagerank

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

EXTRA_DIST += examples/timely/example_args.h
