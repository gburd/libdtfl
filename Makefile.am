# SPDX-License-Identifier: MIT
# libdtfl top-level Makefile.am.  Non-recursive: each component directory
# owns a local.mk fragment that appends to the variables declared here
# (AGENTS.md section 11).  Keep this file to declarations and includes.

ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/src -I$(top_builddir)
AM_CFLAGS = $(DTFL_STD_CFLAGS) $(DTFL_WARN_CFLAGS)

lib_LTLIBRARIES = libdtfl.la
libdtfl_la_SOURCES =
libdtfl_la_CFLAGS = $(AM_CFLAGS) -fvisibility=hidden
libdtfl_la_LDFLAGS = -version-info $(DTFL_LT_VERSION) -no-undefined \
	-Wl,--version-script=$(top_srcdir)/src/libdtfl.map
EXTRA_libdtfl_la_DEPENDENCIES = $(top_srcdir)/src/libdtfl.map

dtflincludedir = $(includedir)/dtfl
dtflinclude_HEADERS =

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = dtfl.pc

noinst_HEADERS = tests/munit.h tests/dtfl_test.h tests/pbt/pbt.h
noinst_LTLIBRARIES = libmunit.la
libmunit_la_SOURCES = tests/munit.c
libmunit_la_CFLAGS = -std=c11 -w

# Tests.  Components append unit tests to check_PROGRAMS and TESTS.
TEST_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/tests -Wno-pedantic -Wno-write-strings -Wno-discarded-qualifiers -Wno-missing-prototypes
TEST_LIBS = libdtfl.la libmunit.la
PBT_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/tests -I$(top_srcdir)/tests/pbt $(HEGEL_CFLAGS) -Wno-pedantic -Wno-clobbered -Wno-missing-prototypes
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
EXTRA_DIST = bootstrap COPYING README AGENTS.md dev src/libdtfl.map \
	doc/snippets tests/corpus data

include src/base/local.mk
include src/log/local.mk
include src/comm/local.mk
include src/comm/zc_local.mk
include src/progress/local.mk
include src/timely/local.mk
include src/dd/local.mk
include tests/interop/local.mk

# Examples (noinst): included after the src fragments so the library and
# its public headers exist to link against (AGENTS.md section 11).
include examples/local.mk

# Sanitizer and interop targets (PLAN section 6).
check-asan:
	@echo "Configure a separate build with --enable-asan --enable-ubsan and run make check there."
	@exit 1
check-tsan:
	@echo "Configure a separate build with --enable-tsan and run make check there."
	@exit 1
.PHONY: check-asan check-tsan
