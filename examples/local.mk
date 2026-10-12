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
	examples/timely/exchange

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

EXTRA_DIST += examples/timely/example_args.h
