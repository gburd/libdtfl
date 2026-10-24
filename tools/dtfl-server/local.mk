# SPDX-License-Identifier: MIT
# tools/dtfl-server/local.mk: the dtfl-server command-line tool, the
# dd_server binary (P14).  Built on the public dtfl_* API only (AGENTS.md
# section 5, the api_discipline gate).  Ported from the differential-dataflow
# `server` crate (server/src/bin/dd_server.rs).
#
# The four example plugins are statically linked in (the divergence from
# dlopen in dtfl/dd_server.h): the tool compiles examples/server/*.c and
# selects a plugin by name, so a session names a plugin, not a shared object.

bin_PROGRAMS += tools/dtfl-server/dtfl-server

tools_dtfl_server_dtfl_server_SOURCES = \
	tools/dtfl-server/main.c \
	examples/server/plugins.h \
	examples/server/degr_dist.c \
	examples/server/reachability.c \
	examples/server/neighborhood.c \
	examples/server/random_graph.c
tools_dtfl_server_dtfl_server_CFLAGS = $(AM_CFLAGS) \
	-I$(top_srcdir)/include \
	-I$(top_srcdir)/examples/server
tools_dtfl_server_dtfl_server_LDADD = libdtfl.la

dist_man_MANS += man/man1/dtfl-server.1

# The example session scripts ship as data (command streams, verbatim),
# installed under pkgdatadir, and the smoke test replays them.
ddserversessiondir = $(pkgdatadir)/server/sessions
dist_ddserversession_DATA = \
	tools/dtfl-server/sessions/degr_dist.txt \
	tools/dtfl-server/sessions/reachability.txt \
	tools/dtfl-server/sessions/shared.txt

# dtfl-server session replay (dist tier): the tool must be built, the
# session scripts and expected transcripts ship.  A hang is a bug: the test
# runs under a wall-clock timeout.
dist_check_SCRIPTS += tests/dist/test_dd_server_session.sh
TESTS += tests/dist/test_dd_server_session.sh
EXTRA_DIST += \
	tests/upstream/server/sessions/degr_dist.expected \
	tests/upstream/server/sessions/reachability.expected \
	tests/upstream/server/sessions/shared.expected
