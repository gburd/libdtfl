# SPDX-License-Identifier: MIT
# tests/fuzz: the libFuzzer tier (PLAN section 6 item 8).
#
# One target per decoder of untrusted bytes.  Each is a
# LLVMFuzzerTestOneInput over the internal API, linked against the
# convenience library, and built only by `make fuzz` (EXTRA_PROGRAMS,
# so plain `make` and `make check` never build them).  They need a
# build configured --enable-fuzz with clang; elsewhere `make fuzz`
# prints SKIP and exits 77.
#
# The committed seed corpus, and every crash reproducer, lives in
# tests/corpus/fuzz/<target>/.  tests/fuzz/run_fuzz.sh replays it and
# then fuzzes each target for DTFL_FUZZ_SECONDS (default 60).

FUZZ_TARGETS = \
	tests/fuzz/fuzz_frames \
	tests/fuzz/fuzz_message \
	tests/fuzz/fuzz_progress \
	tests/fuzz/fuzz_event \
	tests/fuzz/fuzz_time \
	tests/fuzz/fuzz_log \
	tests/fuzz/fuzz_diag_replay \
	tests/fuzz/fuzz_ws_handshake \
	tests/fuzz/fuzz_ddir_parse \
	tests/fuzz/fuzz_ddir_value \
	tests/fuzz/fuzz_record

EXTRA_PROGRAMS = $(FUZZ_TARGETS) tests/fuzz/gen_seeds
CLEANFILES += $(FUZZ_TARGETS) tests/fuzz/gen_seeds
noinst_HEADERS += tests/fuzz/fuzz.h tests/fuzz/fuzz_log_walk.h
EXTRA_DIST += tests/fuzz/run_fuzz.sh

FUZZ_TCFLAGS = $(AM_CFLAGS) -I$(top_srcdir)/tests/fuzz $(TEST_WNO_CFLAGS)
FUZZ_LINK = -fsanitize=fuzzer
FUZZ_TLIBS = libdtfl_internal.la

tests_fuzz_fuzz_frames_SOURCES = tests/fuzz/fuzz_frames.c
tests_fuzz_fuzz_frames_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_frames_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_frames_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_message_SOURCES = tests/fuzz/fuzz_message.c
tests_fuzz_fuzz_message_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_message_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_message_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_progress_SOURCES = tests/fuzz/fuzz_progress.c
tests_fuzz_fuzz_progress_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_progress_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_progress_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_event_SOURCES = tests/fuzz/fuzz_event.c
tests_fuzz_fuzz_event_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_event_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_event_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_time_SOURCES = tests/fuzz/fuzz_time.c
tests_fuzz_fuzz_time_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_time_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_time_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_log_SOURCES = tests/fuzz/fuzz_log.c tests/fuzz/fuzz_log_walk.c
tests_fuzz_fuzz_log_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_log_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_log_LDADD = $(FUZZ_TLIBS)

# dtfl-diag's input path: the public replay, then the tool's own graph
# model and channel rewiring (tools/dtfl-diag), and the in-process
# diagnostics server's Frame builder.
tests_fuzz_fuzz_diag_replay_SOURCES = tests/fuzz/fuzz_diag_replay.c \
	tests/fuzz/fuzz_log_walk.c \
	tools/dtfl-diag/diag_graph.c tools/dtfl-diag/graph.c \
	tools/dtfl-diag/profile.c tools/dtfl-diag/arrangements.c \
	tools/dtfl-diag/connect.c
tests_fuzz_fuzz_diag_replay_CFLAGS = $(FUZZ_TCFLAGS) \
	-I$(top_srcdir)/tools/dtfl-diag -DDTFL_PKGDATADIR='"$(pkgdatadir)"'
tests_fuzz_fuzz_diag_replay_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_diag_replay_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_ws_handshake_SOURCES = tests/fuzz/fuzz_ws_handshake.c
tests_fuzz_fuzz_ws_handshake_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_ws_handshake_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_ws_handshake_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_ddir_parse_SOURCES = tests/fuzz/fuzz_ddir_parse.c
tests_fuzz_fuzz_ddir_parse_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_ddir_parse_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_ddir_parse_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_ddir_value_SOURCES = tests/fuzz/fuzz_ddir_value.c
tests_fuzz_fuzz_ddir_value_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_ddir_value_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_ddir_value_LDADD = $(FUZZ_TLIBS)

tests_fuzz_fuzz_record_SOURCES = tests/fuzz/fuzz_record.c
tests_fuzz_fuzz_record_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_fuzz_record_LDFLAGS = $(FUZZ_LINK)
tests_fuzz_fuzz_record_LDADD = $(FUZZ_TLIBS)

# Writes the seed corpus from the library's own encoders and the
# committed fixtures.  A plain program (no libFuzzer); run it with
# `make fuzz-seeds` after a wire format changes, and commit the result.
tests_fuzz_gen_seeds_SOURCES = tests/fuzz/gen_seeds.c
tests_fuzz_gen_seeds_CFLAGS = $(FUZZ_TCFLAGS)
tests_fuzz_gen_seeds_LDADD = $(FUZZ_TLIBS)

fuzz:
	@if test "x$(DTFL_ENABLE_FUZZ)" != xyes; then \
		echo "SKIP: make fuzz needs a build configured with" \
		    "--enable-fuzz (clang with libFuzzer)"; \
		exit 77; \
	fi
	$(MAKE) $(AM_MAKEFLAGS) $(FUZZ_TARGETS)
	$(SHELL) $(top_srcdir)/tests/fuzz/run_fuzz.sh $(FUZZ_TARGETS)

fuzz-seeds: tests/fuzz/gen_seeds
	./tests/fuzz/gen_seeds $(top_srcdir)

.PHONY: fuzz fuzz-seeds
