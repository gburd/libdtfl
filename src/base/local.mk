# SPDX-License-Identifier: MIT
# src/base: allocation and panic hooks, vectors, bytes, batches, hashing.

base_sources = \
	src/base/base_int.h \
	src/base/batch_int.h \
	src/base/batch.c \
	src/base/bytes.c \
	src/base/hash.c \
	src/base/os_alloc.c \
	src/base/record.c \
	src/base/sock_int.h \
	src/base/util.c

libdtfl_la_SOURCES += $(base_sources)
libdtfl_internal_la_SOURCES += $(base_sources)
dtflinclude_HEADERS += include/dtfl/base.h include/dtfl/dtfl.h \
	include/dtfl/record.h
dist_man_MANS += man/man3/dtfl_set_alloc_hooks.3 man/man3/dtfl_strerror.3 \
	man/man3/dtfl_type.3 man/man3/dtfl_put_u64.3 man/man7/dtfl.7
# A page documents several functions (its .Nm lines); install each
# other name as a link to the page, so man(1) and the pages' .Xr
# cross-references find it.  tests/dist/test_docs.sh checks that every
# .Xr target is a page or such a name.
install-data-hook: install-man-links
uninstall-hook: uninstall-man-links
install-man-links:
	@for f in $(dist_man_MANS); do \
	  case $$f in man/man3/*.3) ;; *) continue ;; esac; \
	  page=`basename $$f`; \
	  for n in `$(AWK) '$$1 == ".Nm" { sub(/,$$/, "", $$2); print $$2 }' \
	      "$(srcdir)/$$f"`; do \
	    [ "$$n.3" = "$$page" ] && continue; \
	    [ -f "$(srcdir)/man/man3/$$n.3" ] && continue; \
	    (cd "$(DESTDIR)$(man3dir)" && rm -f "$$n.3" && \
	      $(LN_S) "$$page" "$$n.3") || exit 1; \
	  done; \
	done
uninstall-man-links:
	@for f in $(dist_man_MANS); do \
	  case $$f in man/man3/*.3) ;; *) continue ;; esac; \
	  page=`basename $$f`; \
	  for n in `$(AWK) '$$1 == ".Nm" { sub(/,$$/, "", $$2); print $$2 }' \
	      "$(srcdir)/$$f"`; do \
	    [ "$$n.3" = "$$page" ] && continue; \
	    [ -f "$(srcdir)/man/man3/$$n.3" ] && continue; \
	    rm -f "$(DESTDIR)$(man3dir)/$$n.3"; \
	  done; \
	done
.PHONY: install-man-links uninstall-man-links
EXTRA_DIST += src/base/exports.sym

check_PROGRAMS += tests/unit/test_base
TESTS += tests/unit/test_base
tests_unit_test_base_SOURCES = tests/unit/test_base.c
tests_unit_test_base_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_base_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_smoke
TESTS += tests/pbt/pbt_smoke
tests_pbt_pbt_smoke_SOURCES = tests/pbt/pbt_smoke.c
tests_pbt_pbt_smoke_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_smoke_LDADD = $(INTERNAL_PBT_LIBS)

check_PROGRAMS += tests/unit/test_record tests/unit/test_bytes tests/unit/test_batch
TESTS += tests/unit/test_record tests/unit/test_bytes tests/unit/test_batch
tests_unit_test_record_SOURCES = tests/unit/test_record.c
tests_unit_test_record_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_record_LDADD = $(INTERNAL_LIBS)
tests_unit_test_bytes_SOURCES = tests/unit/test_bytes.c
tests_unit_test_bytes_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_bytes_LDADD = $(INTERNAL_LIBS)
tests_unit_test_batch_SOURCES = tests/unit/test_batch.c
tests_unit_test_batch_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_batch_LDADD = $(INTERNAL_LIBS)

check_PROGRAMS += tests/pbt/pbt_record tests/pbt/pbt_batch
TESTS += tests/pbt/pbt_record tests/pbt/pbt_batch
tests_pbt_pbt_record_SOURCES = tests/pbt/pbt_record.c
tests_pbt_pbt_record_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_record_LDADD = $(INTERNAL_PBT_LIBS)
tests_pbt_pbt_batch_SOURCES = tests/pbt/pbt_batch.c
tests_pbt_pbt_batch_CFLAGS = $(PBT_CFLAGS)
tests_pbt_pbt_batch_LDADD = $(INTERNAL_PBT_LIBS)

# Thread creation (the one pthread_create; AGENTS.md section 5).
os_thread_sources = src/base/os_thread.h src/base/os_thread.c
libdtfl_la_SOURCES += $(os_thread_sources)
libdtfl_internal_la_SOURCES += $(os_thread_sources)

# Embedding host hooks: logging sink and thread spawn (dtfl_embedding(7)).
dist_man_MANS += man/man3/dtfl_set_log_hook.3 man/man3/dtfl_set_thread_hooks.3 \
	man/man7/dtfl_embedding.7
check_PROGRAMS += tests/unit/test_hooks
TESTS += tests/unit/test_hooks
tests_unit_test_hooks_SOURCES = tests/unit/test_hooks.c
tests_unit_test_hooks_CFLAGS = $(TEST_CFLAGS)
tests_unit_test_hooks_LDADD = $(INTERNAL_LIBS)
