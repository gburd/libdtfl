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

# Capture wire format: libdtfl's EventWriter framing against real timely.
check_PROGRAMS += tests/interop/test_capture_wire
TESTS += tests/interop/test_capture_wire
tests_interop_test_capture_wire_SOURCES = tests/interop/test_capture_wire.c
tests_interop_test_capture_wire_CFLAGS = $(TEST_CFLAGS)
tests_interop_test_capture_wire_LDADD = $(INTERNAL_LIBS)

# Timely log records: libdtfl's TimelyEvent bincode against real timely.
check_PROGRAMS += tests/interop/test_log_stream
TESTS += tests/interop/test_log_stream
tests_interop_test_log_stream_SOURCES = tests/interop/test_log_stream.c
tests_interop_test_log_stream_CFLAGS = $(TEST_CFLAGS)
tests_interop_test_log_stream_LDADD = $(INTERNAL_LIBS)

# Operator output oracle: join/count/reduce against real differential.
check_PROGRAMS += tests/interop/test_operator_oracle
TESTS += tests/interop/test_operator_oracle
tests_interop_test_operator_oracle_SOURCES = tests/interop/test_operator_oracle.c
tests_interop_test_operator_oracle_CFLAGS = $(TEST_CFLAGS)
tests_interop_test_operator_oracle_LDADD = $(INTERNAL_LIBS)

# Ship the fixtures and the generators in the distribution tarball.
EXTRA_DIST += \
	tests/interop/fixtures/ord_order.bin \
	tests/interop/fixtures/capture_wire.bin \
	tests/interop/fixtures/log_stream.bin \
	tests/interop/fixtures/operator_oracle.bin \
	dev/gen-fixtures.d/Cargo.toml \
	dev/gen-fixtures.d/Cargo.lock \
	dev/gen-fixtures.d/ord_order.rs \
	dev/gen-fixtures.d/capture_wire.rs \
	dev/gen-fixtures.d/log_stream.rs \
	dev/gen-fixtures.d/operator_oracle.rs
