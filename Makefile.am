# SPDX-License-Identifier: MIT
# libdtfl top-level Makefile.am.  Non-recursive: each component directory
# owns a local.mk fragment that appends to the variables declared here
# (AGENTS.md section 11).  Keep this file to declarations and includes.

ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src -I$(top_builddir)
AM_CFLAGS = $(DTFL_STD_CFLAGS) $(DTFL_WARN_CFLAGS) $(DTFL_FP_CFLAGS)

lib_LTLIBRARIES = libdtfl.la
libdtfl_la_SOURCES =
libdtfl_la_CFLAGS = $(AM_CFLAGS) -fvisibility=hidden
libdtfl_la_LDFLAGS = -version-info $(DTFL_LT_VERSION) -no-undefined \
	$(DTFL_SYMBOL_LDFLAGS)
EXTRA_libdtfl_la_DEPENDENCIES = $(top_srcdir)/src/libdtfl.map \
	$(top_srcdir)/src/libdtfl.exp

dtflincludedir = $(includedir)/dtfl
dtflinclude_HEADERS =

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = dtfl.pc

noinst_HEADERS = tests/munit.h tests/dtfl_test.h tests/pbt/pbt.h \
	tests/pbt/pbt_sim.h
noinst_LTLIBRARIES = libmunit.la
libmunit_la_SOURCES = tests/munit.c
libmunit_la_CFLAGS = -std=c11 -w

# Tests.  Components append unit tests to check_PROGRAMS and TESTS.
TEST_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/tests $(TEST_WNO_CFLAGS)
TEST_LIBS = libdtfl.la libmunit.la
PBT_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/tests -I$(top_srcdir)/tests/pbt $(HEGEL_CFLAGS) $(TEST_WNO_CFLAGS)
PBT_LIBS = libdtfl.la $(HEGEL_LIBS)
# Tests that need library-internal (hidden) symbols link the convenience
# library of all objects instead of the shared library.  Components use
# these as e.g. `tests_unit_test_x_LDADD = $(INTERNAL_LIBS)`.
INTERNAL_LIBS = libdtfl_internal.la libmunit.la
INTERNAL_PBT_LIBS = libdtfl_internal.la $(HEGEL_LIBS)
noinst_LTLIBRARIES += libdtfl_internal.la
libdtfl_internal_la_SOURCES =
libdtfl_internal_la_CFLAGS = $(AM_CFLAGS)

check_PROGRAMS =
noinst_PROGRAMS =
TESTS =
CLEANFILES =
dist_check_SCRIPTS = \
	tests/dist/test_api_discipline.sh \
	tests/dist/test_symbols.sh \
	tests/dist/test_headers.sh \
	tests/dist/test_ledger.sh \
	tests/dist/test_docs.sh \
	tests/dist/test_completion.sh
TESTS += $(dist_check_SCRIPTS)
# ThreadSanitizer cannot map its shadow memory under the high-entropy
# ASLR of newer kernels (vm.mmap_rnd_bits > 28): it aborts with
# "unexpected memory mapping".  setarch -R disables randomization for
# the test process only.  Configure sets DTFL_TEST_WRAPPER for TSan builds.
LOG_COMPILER = $(DTFL_TEST_WRAPPER)
AM_TESTS_ENVIRONMENT = \
	top_srcdir='$(abs_top_srcdir)'; export top_srcdir; \
	CC='$(CC)'; export CC; CXX='$(CXX)'; export CXX; NM='$(NM)'; export NM;

dist_man_MANS =
EXTRA_DIST = bootstrap COPYING README.md NEWS AUTHORS THANKS ChangeLog \
	AGENTS.md debian flake.nix flake.lock dev src/libdtfl.map src/libdtfl.exp \
	doc/snippets tests/corpus data

include src/base/local.mk
include src/log/local.mk
include src/comm/local.mk
include src/comm/zc_local.mk
include src/progress/local.mk
include src/timely/local.mk
include src/dd/local.mk
include src/ddir/local.mk
include src/algo/local.mk
include src/dogs/local.mk
include src/diag/local.mk
include tests/interop/local.mk
include bench/local.mk

# Tools (bin_PROGRAMS): command-line programs built on the public API
# only.  Included after the src fragments so the library exists to link.
include tools/local.mk
include tools/dtfl-ddir/local.mk
include tools/dtfl-server/local.mk

# Examples (noinst): included after the src fragments so the library and
# its public headers exist to link against (AGENTS.md section 11).
include examples/local.mk

# The Texinfo manual (info_TEXINFOS + libdtfl_TEXINFOS).
include doc/local.mk

# Sanitizer and interop targets (PLAN section 6).
check-asan:
	@echo "Configure a separate build with --enable-asan --enable-ubsan and run make check there."
	@exit 1
check-tsan:
	@echo "Configure a separate build with --enable-tsan and run make check there."
	@exit 1

# Simulator coverage (P16.7): the simulator-capable properties under
# DTFL_COMM_SIM with drawn seeds and faults.  DTFL_SIM_SEEDS (default
# 2000) cases per property, in DTFL_SIM_JOBS parallel shards.
SIM_PBT = tests/pbt/pbt_comm tests/pbt/pbt_join tests/pbt/pbt_reduce \
	tests/pbt/pbt_ops_notify tests/pbt/pbt_bfs
EXTRA_DIST += tests/pbt/check_sim.sh
check-sim: $(SIM_PBT)
	$(SHELL) $(top_srcdir)/tests/pbt/check_sim.sh
.PHONY: check-asan check-tsan check-sim
