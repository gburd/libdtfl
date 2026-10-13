# SPDX-License-Identifier: MIT
# tools/local.mk: command-line programs built on the public dtfl_* API
# only (AGENTS.md section 5, the api_discipline gate).  Installed to
# bindir.  Ported from dataflow-diag's tdiag crate (P11).

bin_PROGRAMS = tools/dtfl-diag/dtfl-diag

TOOL_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include \
	-I$(top_srcdir)/tools/dtfl-diag \
	-DDTFL_PKGDATADIR='"$(pkgdatadir)"'

tools_dtfl_diag_dtfl_diag_SOURCES = \
	tools/dtfl-diag/diag_cmd.h \
	tools/dtfl-diag/diag_graph.h \
	tools/dtfl-diag/main.c \
	tools/dtfl-diag/connect.c \
	tools/dtfl-diag/diag_graph.c \
	tools/dtfl-diag/graph.c \
	tools/dtfl-diag/profile.c \
	tools/dtfl-diag/arrangements.c
tools_dtfl_diag_dtfl_diag_CFLAGS = $(TOOL_CFLAGS)
tools_dtfl_diag_dtfl_diag_LDADD = libdtfl.la

dist_man_MANS += man/man1/dtfl-diag.1

# dtfl-diag graph reads the committed log-stream fixture and
# reconstructs the operator graph (P11 exit criterion).  The tool must
# be built for the test; the fixture and its generator ship in dist.
dist_check_SCRIPTS += tests/dist/test_diag_graph.sh
TESTS += tests/dist/test_diag_graph.sh
EXTRA_DIST += tests/interop/fixtures/diag_graph_stream.bin \
	tests/interop/fixtures/diag_profile_stream.bin \
	tests/interop/fixtures/diag_differential_stream.bin \
	tests/interop/gen_diag_stream.c
