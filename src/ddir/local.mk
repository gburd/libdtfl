# SPDX-License-Identifier: MIT
# src/ddir: the Differential Dataflow Intermediate Representation (DDIR).
# A small language for building differential dataflows at runtime: a text
# program parses to an AST, lowers to a scope-tree IR, and renders into a
# live differential dataflow via the vec backend.  Ported from the
# differential-dataflow `interactive` crate (P13 core: ir, parse, scope_ir,
# lower, backend, public entry; server/explain/tool are a follow-up).

ddir_sources = \
	src/ddir/ddir_int.h \
	src/ddir/parse_int.h \
	src/ddir/backend_int.h \
	src/ddir/ir.c \
	src/ddir/parse.c \
	src/ddir/parse_pipe.c \
	src/ddir/parse_applicative.c \
	src/ddir/scope_ir.c \
	src/ddir/lower.c \
	src/ddir/backend.c \
	src/ddir/backend_vec.c \
	src/ddir/ddir.c

libdtfl_la_SOURCES += $(ddir_sources)
libdtfl_internal_la_SOURCES += $(ddir_sources)
dtflinclude_HEADERS += include/dtfl/ddir.h
EXTRA_DIST += src/ddir/exports.sym

dist_man_MANS += man/man3/dtfl_ddir_parse.3

check_PROGRAMS += tests/unit/test_ddir
TESTS += tests/unit/test_ddir
tests_unit_test_ddir_SOURCES = tests/unit/test_ddir.c
tests_unit_test_ddir_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_ddir_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_scope_ir
TESTS += tests/unit/test_scope_ir
tests_unit_test_scope_ir_SOURCES = tests/unit/test_scope_ir.c
tests_unit_test_scope_ir_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_scope_ir_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_lower
TESTS += tests/unit/test_lower
tests_unit_test_lower_SOURCES = tests/unit/test_lower.c
tests_unit_test_lower_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_lower_LDADD = $(INTERNAL_LIBS)
