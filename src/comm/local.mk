# SPDX-License-Identifier: MIT
# src/comm: the communication layer (timely_communication) and the
# simulation allocator.

comm_sources = \
	src/comm/comm_int.h \
	src/comm/init_int.h \
	src/comm/sim_int.h \
	src/comm/alloc_canary.c \
	src/comm/alloc_counters.c \
	src/comm/alloc_generic.c \
	src/comm/alloc_process.c \
	src/comm/alloc_sim.c \
	src/comm/alloc_thread.c \
	src/comm/allocator.c \
	src/comm/buzzer.c \
	src/comm/comm_log.c \
	src/comm/config.c \
	src/comm/initialize.c \
	src/comm/queue.c

libdtfl_la_SOURCES += $(comm_sources)
libdtfl_internal_la_SOURCES += $(comm_sources)
dtflinclude_HEADERS += include/dtfl/config.h
dist_man_MANS += man/man3/dtfl_config.3
EXTRA_DIST += src/comm/exports.sym

check_PROGRAMS += tests/unit/test_comm tests/unit/test_config
TESTS += tests/unit/test_comm tests/unit/test_config
tests_unit_test_comm_SOURCES = tests/unit/test_comm.c tests/unit/test_comm_msg.h
tests_unit_test_comm_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_comm_LDADD = $(INTERNAL_LIBS)
tests_unit_test_config_SOURCES = tests/unit/test_config.c
tests_unit_test_config_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_config_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_comm
TESTS += tests/pbt/pbt_comm
tests_pbt_pbt_comm_SOURCES = tests/pbt/pbt_comm.c
tests_pbt_pbt_comm_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_comm_LDADD = $(INTERNAL_PBT_LIBS)
