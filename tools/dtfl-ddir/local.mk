# SPDX-License-Identifier: MIT
# tools/dtfl-ddir/local.mk: the dtfl-ddir command-line tool, built on the
# public dtfl_* API only (AGENTS.md section 5, the api_discipline gate).
# Three subcommands follow the three upstream example binaries: run
# (ddir_vec), server (ddir_server), explain (dump_explain).  Ported from
# the differential-dataflow interactive crate (P13).

if DTFL_DDIR
bin_PROGRAMS += tools/dtfl-ddir/dtfl-ddir

tools_dtfl_ddir_dtfl_ddir_SOURCES = \
	tools/dtfl-ddir/ddir_tool.h \
	tools/dtfl-ddir/main.c \
	tools/dtfl-ddir/common.c \
	tools/dtfl-ddir/run.c \
	tools/dtfl-ddir/server_cmd.c \
	tools/dtfl-ddir/explain_cmd.c
tools_dtfl_ddir_dtfl_ddir_CFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/include \
	-I$(top_srcdir)/tools/dtfl-ddir \
	-DDTFL_PKGDATADIR='"$(pkgdatadir)"'
tools_dtfl_ddir_dtfl_ddir_LDADD = libdtfl.la

dist_man_MANS += man/man1/dtfl-ddir.1

# The example DDIR programs and server sessions ship as data (DDIR source
# text, shipped verbatim), installed under pkgdatadir.
ddirexampledir = $(pkgdatadir)/ddir/programs
dist_ddirexample_DATA = \
	tools/dtfl-ddir/programs/reach.ddir \
	tools/dtfl-ddir/programs/reach.ddp \
	tools/dtfl-ddir/programs/binders.ddp \
	tools/dtfl-ddir/programs/unnest.ddp \
	tools/dtfl-ddir/programs/stable.ddp \
	tools/dtfl-ddir/programs/stable.ddir \
	tools/dtfl-ddir/programs/adt.ddp \
	tools/dtfl-ddir/programs/kcore.ddir \
	tools/dtfl-ddir/programs/scc.ddp \
	tools/dtfl-ddir/programs/scc.ddir

ddirserverprogdir = $(pkgdatadir)/ddir/server/programs
dist_ddirserverprog_DATA = \
	tools/dtfl-ddir/server/programs/echo.ddp \
	tools/dtfl-ddir/server/programs/clock_watch.ddp \
	tools/dtfl-ddir/server/programs/producer.ddp \
	tools/dtfl-ddir/server/programs/count_gen.ddp \
	tools/dtfl-ddir/server/programs/reach_import.ddp \
	tools/dtfl-ddir/server/programs/rand_reach.ddp \
	tools/dtfl-ddir/server/programs/reach_gen.ddp \
	tools/dtfl-ddir/server/programs/arrange_idem.ddp

ddirsessiondir = $(pkgdatadir)/ddir/server/sessions
dist_ddirsession_DATA = \
	tools/dtfl-ddir/server/sessions/clock.txt \
	tools/dtfl-ddir/server/sessions/derived.txt \
	tools/dtfl-ddir/server/sessions/peek.txt \
	tools/dtfl-ddir/server/sessions/values.txt \
	tools/dtfl-ddir/server/sessions/drop.txt \
	tools/dtfl-ddir/server/sessions/generated.txt \
	tools/dtfl-ddir/server/sessions/shared_trace.txt \
	tools/dtfl-ddir/server/sessions/arrange_idem.txt

# dtfl-ddir smoke and server-session replay tests (dist tier): the tool
# must be built, the data files and expected transcripts ship.
dist_check_SCRIPTS += tests/dist/test_ddir_smoke.sh
dist_check_SCRIPTS += tests/dist/test_ddir_server.sh
TESTS += tests/dist/test_ddir_smoke.sh
TESTS += tests/dist/test_ddir_server.sh
EXTRA_DIST += \
	tests/upstream/ddir/sessions/clock.expected \
	tests/upstream/ddir/sessions/derived.expected \
	tests/upstream/ddir/sessions/peek.expected \
	tests/upstream/ddir/sessions/values.expected \
	tests/upstream/ddir/sessions/drop.expected \
	tests/upstream/ddir/sessions/generated.expected \
	tests/upstream/ddir/sessions/shared_trace.expected \
	tests/upstream/ddir/sessions/arrange_idem.expected
endif
