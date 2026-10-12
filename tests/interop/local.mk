# SPDX-License-Identifier: MIT
# tests/interop: fidelity checks against fixtures produced by the pinned
# Rust crates (dev/gen-fixtures).  These are the only tests that compare
# libdtfl against real upstream output rather than a re-derived C oracle
# (PLAN sections 6, 9).  The fixtures are committed, so make check needs
# no cargo; regenerate with dev/gen-fixtures when a pin moves.

check_PROGRAMS += tests/interop/test_ord_order
TESTS += tests/interop/test_ord_order
tests_interop_test_ord_order_SOURCES = tests/interop/test_ord_order.c
tests_interop_test_ord_order_CFLAGS = $(TEST_CFLAGS)
tests_interop_test_ord_order_LDADD = $(INTERNAL_LIBS)

# Ship the fixtures and the generator in the distribution tarball.
EXTRA_DIST += \
	tests/interop/fixtures/ord_order.bin \
	dev/gen-fixtures.d/Cargo.toml \
	dev/gen-fixtures.d/ord_order.rs
