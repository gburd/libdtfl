# SPDX-License-Identifier: MIT
# src/comm (zero-copy layer): MessageHeader framing, byte slabs, merge
# queues, spill policies, serializing push/pull and the ProcessBinary
# allocator.

zc_sources = \
	src/comm/zc_int.h \
	src/comm/networking.c \
	src/comm/zc_bytes_slab.c \
	src/comm/zc_bytes_exchange.c \
	src/comm/spill.c \
	src/comm/zc_push_pull.c \
	src/comm/zc_tcp.c \
	src/comm/zc_allocator.c \
	src/comm/zc_initialize.c \
	src/comm/zc_allocator_process.c

libdtfl_la_SOURCES += $(zc_sources)
libdtfl_internal_la_SOURCES += $(zc_sources)

check_PROGRAMS += tests/unit/test_wire tests/unit/test_spill tests/unit/test_tcp
TESTS += tests/unit/test_wire tests/unit/test_spill tests/unit/test_tcp
tests_unit_test_wire_SOURCES = tests/unit/test_wire.c
tests_unit_test_wire_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_wire_LDADD = $(INTERNAL_LIBS)
tests_unit_test_spill_SOURCES = tests/unit/test_spill.c
tests_unit_test_spill_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_spill_LDADD = $(INTERNAL_LIBS)
tests_unit_test_tcp_SOURCES = tests/unit/test_tcp.c tests/unit/test_comm_msg.h
tests_unit_test_tcp_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_tcp_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_wire
TESTS += tests/pbt/pbt_wire
tests_pbt_pbt_wire_SOURCES = tests/pbt/pbt_wire.c
tests_pbt_pbt_wire_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_wire_LDADD = $(INTERNAL_PBT_LIBS)
