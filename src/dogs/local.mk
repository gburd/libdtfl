# SPDX-License-Identifier: MIT
# src/dogs: delta queries and worst-case-optimal joins (dogsdogsdogs).
# The AltNeu helpers and differential calculus, the propose/validate/count
# operators built on lookup_map, the half joins, and the CollectionIndex /
# generic-join driver.  Included after src/dd/local.mk: it builds on the
# arrange, join and collection layers there.

dogs_sources = \
	src/dogs/dogs_int.h \
	src/dogs/altneu.c \
	src/dogs/calculus.c \
	src/dogs/lookup_map.c \
	src/dogs/propose.c \
	src/dogs/validate.c \
	src/dogs/count.c \
	src/dogs/extend.c

libdtfl_la_SOURCES += $(dogs_sources)
libdtfl_internal_la_SOURCES += $(dogs_sources)
dtflinclude_HEADERS += include/dtfl/dogs.h
EXTRA_DIST += src/dogs/exports.sym

dist_man_MANS += man/man3/dtfl_collection_delay_neu.3
dist_man_MANS += man/man3/dtfl_collection_index_new.3
dist_man_MANS += man/man3/dtfl_dogs_propose.3

check_PROGRAMS += tests/upstream/dogs/lookup_map_regression
TESTS += tests/upstream/dogs/lookup_map_regression
tests_upstream_dogs_lookup_map_regression_SOURCES = \
	tests/upstream/dogs/lookup_map_regression.c
tests_upstream_dogs_lookup_map_regression_CFLAGS = $(TEST_CFLAGS)
tests_upstream_dogs_lookup_map_regression_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/unit/test_dogs
TESTS += tests/unit/test_dogs
tests_unit_test_dogs_SOURCES = tests/unit/test_dogs.c
tests_unit_test_dogs_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_dogs_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_dogs
TESTS += tests/pbt/pbt_dogs
tests_pbt_pbt_dogs_SOURCES = tests/pbt/pbt_dogs.c
tests_pbt_pbt_dogs_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_dogs_LDADD = $(INTERNAL_PBT_LIBS)
