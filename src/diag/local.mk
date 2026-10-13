# SPDX-License-Identifier: MIT
# src/diag: the in-process diagnostics WebSocket server and the recorded
# log-stream replay used by the dtfl-diag tool (P11).  Ported from the
# dataflow-diag "diagnostics" crate (diagnostics/src/{lib,server,logging}.rs)
# and the "connect" crate (receive/replaywithshutdown.rs).

diag_sources = \
	src/diag/diag_int.h \
	src/diag/diag.c \
	src/diag/diag_log.c \
	src/diag/ws_frame.c \
	src/diag/ws_server.c \
	src/diag/replay_shutdown.c

libdtfl_la_SOURCES += $(diag_sources)
libdtfl_internal_la_SOURCES += $(diag_sources)
dtflinclude_HEADERS += include/dtfl/diag.h
dist_man_MANS += man/man3/dtfl_diag_session_new.3
EXTRA_DIST += src/diag/exports.sym

# Shipped browser assets: the single-file UI and the dtfl-diag graph
# template (served/generated verbatim, PLAN section 3).  ASCII only.
dist_pkgdata_DATA = \
	data/diagnostics.html \
	data/dataflow-graph.html

check_PROGRAMS += tests/unit/test_diag
TESTS += tests/unit/test_diag
tests_unit_test_diag_SOURCES = tests/unit/test_diag.c
tests_unit_test_diag_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_diag_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_ws_frame
TESTS += tests/unit/test_ws_frame
tests_unit_test_ws_frame_SOURCES = tests/unit/test_ws_frame.c
tests_unit_test_ws_frame_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ws_frame_LDADD = $(INTERNAL_LIBS)
