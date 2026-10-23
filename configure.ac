# SPDX-License-Identifier: MIT
# libdtfl -- process this file with autoconf (via ./bootstrap).

AC_PREREQ([2.69])
AC_INIT([libdtfl], [0.0.1], [greg@burd.me], [libdtfl])
AC_CONFIG_SRCDIR([src/base/os_alloc.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([config.h])
AC_CANONICAL_HOST

AM_INIT_AUTOMAKE([1.15 foreign subdir-objects -Wall -Werror -Wno-unsupported parallel-tests color-tests])
AM_SILENT_RULES([yes])
AM_PROG_AR
LT_INIT([disable-static pic-only])

# libtool -version-info current:revision:age.  Bumped only at release
# (.kiro/steering/gnu-project.md).
AC_SUBST([DTFL_LT_VERSION], [0:0:0])

AC_PROG_CC
AC_PROG_CXX
AC_PROG_SED
AC_PROG_GREP
AC_PROG_EGREP
PKG_PROG_PKG_CONFIG

# -------------------------------------------------------------------------
# C11 is required: _Atomic, _Thread_local, _Static_assert, anonymous unions.
# -------------------------------------------------------------------------
AC_CACHE_CHECK([whether $CC accepts -std=c11], [dtfl_cv_c11],
	[save_CFLAGS="$CFLAGS"
	 CFLAGS="$CFLAGS -std=c11"
	 AC_COMPILE_IFELSE(
		[AC_LANG_PROGRAM([[@%:@include <stdatomic.h>
static _Thread_local int t;
static _Atomic int a;]],
			[[t = 1; atomic_fetch_add(&a, t);]])],
		[dtfl_cv_c11=yes], [dtfl_cv_c11=no])
	 CFLAGS="$save_CFLAGS"])
AS_IF([test "x$dtfl_cv_c11" != xyes],
	[AC_MSG_ERROR([a C11 compiler with atomics and thread-local storage is required])])
DTFL_STD_CFLAGS="-std=c11"
AC_SUBST([DTFL_STD_CFLAGS])

AX_PTHREAD([], [AC_MSG_ERROR([POSIX threads are required])])
LIBS="$PTHREAD_LIBS $LIBS"
CFLAGS="$CFLAGS $PTHREAD_CFLAGS"

AC_SEARCH_LIBS([dlopen], [dl dld])
AC_SEARCH_LIBS([clock_gettime], [rt])
AC_SEARCH_LIBS([socket], [socket])
AC_CHECK_HEADERS([linux/futex.h sys/syscall.h])
AC_C_BIGENDIAN

# -------------------------------------------------------------------------
# Warnings.  Zero warnings is the policy; -Werror is on unless the user
# asks for --disable-werror (packagers on newer compilers).
# -------------------------------------------------------------------------
AC_ARG_ENABLE([werror],
	[AS_HELP_STRING([--disable-werror], [do not treat warnings as errors])],
	[], [enable_werror=yes])
DTFL_WARN_CFLAGS="-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wcast-align -Wwrite-strings -Wno-unused-parameter"
AS_IF([test "x$enable_werror" = xyes],
	[DTFL_WARN_CFLAGS="$DTFL_WARN_CFLAGS -Werror"])
AC_SUBST([DTFL_WARN_CFLAGS])

# The test and property-test CFLAGS silence a few warnings in the
# vendored munit and the test helpers.  Some -Wno-* spellings are
# gcc-only (-Wno-discarded-qualifiers, -Wno-clobbered); clang errors on
# an unknown warning option under -Werror.  Probe each and keep only the
# ones this compiler accepts, so the tests build under both gcc and
# clang.
TEST_WNO_CFLAGS=""
for _f in -Wno-pedantic -Wno-write-strings -Wno-missing-prototypes \
          -Wno-discarded-qualifiers -Wno-clobbered \
          -Wno-unneeded-internal-declaration; do
	AC_MSG_CHECKING([whether $CC accepts $_f])
	_save_cflags="$CFLAGS"
	CFLAGS="$CFLAGS -Werror $_f"
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
		[TEST_WNO_CFLAGS="$TEST_WNO_CFLAGS $_f"; AC_MSG_RESULT([yes])],
		[AC_MSG_RESULT([no])])
	CFLAGS="$_save_cflags"
done
AC_SUBST([TEST_WNO_CFLAGS])

# -------------------------------------------------------------------------
# Symbol visibility.  GNU ld, gold, lld and the BSD linkers take a version
# script; Apple ld64 does not, and takes an exported-symbols list instead.
# Probe rather than switch on host_os, so any linker that accepts the
# version script uses it.  The probe links an executable, so its script
# must not hide anything: "local: *" hides environ, which FreeBSD's
# crt1.o exports to libc, and GNU ld then refuses the link.
# -------------------------------------------------------------------------
AC_CACHE_CHECK([whether the linker accepts --version-script],
	[dtfl_cv_ld_version_script],
	[save_LDFLAGS="$LDFLAGS"
	 printf 'V { global: *; };\n' > conftest.map
	 LDFLAGS="$LDFLAGS -Wl,--version-script=conftest.map"
	 AC_LINK_IFELSE([AC_LANG_PROGRAM([], [])],
		[dtfl_cv_ld_version_script=yes], [dtfl_cv_ld_version_script=no])
	 LDFLAGS="$save_LDFLAGS"
	 rm -f conftest.map])
AS_IF([test "x$dtfl_cv_ld_version_script" = xyes],
	[DTFL_SYMBOL_LDFLAGS='-Wl,--version-script=$(top_srcdir)/src/libdtfl.map'],
	[DTFL_SYMBOL_LDFLAGS='-Wl,-exported_symbols_list,$(top_srcdir)/src/libdtfl.exp'])
AC_SUBST([DTFL_SYMBOL_LDFLAGS])

# -------------------------------------------------------------------------
# Debug: owner-thread checks, leak checks at worker shutdown, debug asserts.
# -------------------------------------------------------------------------
AC_ARG_ENABLE([debug],
	[AS_HELP_STRING([--enable-debug], [enable debug assertions, owner-thread and leak checks])],
	[], [enable_debug=no])
AS_IF([test "x$enable_debug" = xyes],
	[AC_DEFINE([DTFL_DEBUG], [1], [Define to enable debug checks])
	 CFLAGS="$CFLAGS -g -O1 -fno-omit-frame-pointer"])

# -------------------------------------------------------------------------
# Sanitizers.  Each adds its flags to CFLAGS and LDFLAGS.
# -------------------------------------------------------------------------
DTFL_SAN_FLAGS=""
AC_ARG_ENABLE([asan],
	[AS_HELP_STRING([--enable-asan], [build with AddressSanitizer])],
	[AS_IF([test "x$enableval" = xyes], [DTFL_SAN_FLAGS="$DTFL_SAN_FLAGS -fsanitize=address"])])
AC_ARG_ENABLE([ubsan],
	[AS_HELP_STRING([--enable-ubsan], [build with UndefinedBehaviorSanitizer])],
	[AS_IF([test "x$enableval" = xyes], [DTFL_SAN_FLAGS="$DTFL_SAN_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined"])])
AC_ARG_ENABLE([tsan],
	[AS_HELP_STRING([--enable-tsan], [build with ThreadSanitizer])],
	[AS_IF([test "x$enableval" = xyes], [DTFL_SAN_FLAGS="$DTFL_SAN_FLAGS -fsanitize=thread"])])
# Fail here, not halfway through make, when the compiler has no runtime
# for a sanitizer (gcc on FreeBSD has no libtsan).
AS_IF([test -n "$DTFL_SAN_FLAGS"],
	[AC_CACHE_CHECK([whether $CC links with$DTFL_SAN_FLAGS], [dtfl_cv_san_link],
		[save_CFLAGS="$CFLAGS" save_LDFLAGS="$LDFLAGS"
		 CFLAGS="$CFLAGS $DTFL_SAN_FLAGS" LDFLAGS="$LDFLAGS $DTFL_SAN_FLAGS"
		 AC_LINK_IFELSE([AC_LANG_PROGRAM([], [])],
			[dtfl_cv_san_link=yes], [dtfl_cv_san_link=no])
		 CFLAGS="$save_CFLAGS" LDFLAGS="$save_LDFLAGS"])
	 AS_IF([test "x$dtfl_cv_san_link" != xyes],
		[AC_MSG_ERROR([$CC cannot link with$DTFL_SAN_FLAGS; its sanitizer runtime is missing on this platform (see config.log)])])])
# The tests of a sanitizer build run with address randomization off.
# Linux: ThreadSanitizer cannot map its shadow under high-entropy ASLR,
# so TSan tests run under setarch -R.  FreeBSD has no setarch; its
# proccontrol does the same, and gcc's libasan there refuses to start
# at all with ASLR on ("not compatible with enabled ASLR"), so every
# sanitizer build uses it.  The setting is inherited by child processes,
# so the shell-script tests' programs run without ASLR too.
DTFL_TEST_WRAPPER=""
AS_IF([test -n "$DTFL_SAN_FLAGS"],
	[AC_CHECK_PROG([SETARCH], [setarch], [setarch])
	 AC_CHECK_PROG([PROCCONTROL], [proccontrol], [proccontrol])
	 AS_CASE(["$DTFL_SAN_FLAGS"], [*thread*],
		[AS_IF([test -n "$SETARCH"], [DTFL_TEST_WRAPPER="setarch `uname -m` -R"])])
	 AS_IF([test -z "$DTFL_TEST_WRAPPER" && test -n "$PROCCONTROL"],
		[DTFL_TEST_WRAPPER="proccontrol -m aslr -s disable"])])
AC_SUBST([DTFL_TEST_WRAPPER])
AS_IF([test -n "$DTFL_SAN_FLAGS"],
	[CFLAGS="$CFLAGS $DTFL_SAN_FLAGS -fno-omit-frame-pointer -g"
	 LDFLAGS="$LDFLAGS $DTFL_SAN_FLAGS"])

# -------------------------------------------------------------------------
# Frame pointers.  Kept by default, so perf, bpftrace and other profilers
# can walk the stack of a release build without DWARF unwinding.
# --disable-frame-pointers lets the compiler omit them.  The flags go in
# AM_CFLAGS, ahead of the user's CFLAGS, so CFLAGS can still override.
# -------------------------------------------------------------------------
AC_ARG_ENABLE([frame-pointers],
	[AS_HELP_STRING([--disable-frame-pointers], [let the compiler omit frame pointers (kept by default, for profilers)])],
	[], [enable_frame_pointers=yes])
DTFL_FP_CFLAGS=""
AS_IF([test "x$enable_frame_pointers" = xyes],
	[for _f in -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer; do
		AC_MSG_CHECKING([whether $CC accepts $_f])
		_save_cflags="$CFLAGS"
		CFLAGS="$CFLAGS -Werror $_f"
		AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [])],
			[DTFL_FP_CFLAGS="$DTFL_FP_CFLAGS $_f"; AC_MSG_RESULT([yes])],
			[AC_MSG_RESULT([no])])
		CFLAGS="$_save_cflags"
	 done])
AC_SUBST([DTFL_FP_CFLAGS])

# -------------------------------------------------------------------------
# USDT static probes (DTrace and SystemTap) through sys/sdt.h, from
# systemtap-sdt-dev on Debian.  Default: on when the header provides
# STAP_PROBEn (FreeBSD's kernel sys/sdt.h does not).  --enable-usdt
# fails without it; --disable-usdt compiles every probe to nothing.
# -------------------------------------------------------------------------
AC_ARG_ENABLE([usdt],
	[AS_HELP_STRING([--enable-usdt], [build the USDT static probes (default: when sys/sdt.h is usable)])],
	[], [enable_usdt=auto])
AS_IF([test "x$enable_usdt" != xno],
	[AC_CACHE_CHECK([for a usable sys/sdt.h], [dtfl_cv_sdt],
		[AC_COMPILE_IFELSE(
			[AC_LANG_PROGRAM([[@%:@include <sys/sdt.h>]],
				[[int x = 1; STAP_PROBE2(dtfl, conftest, x, &x);]])],
			[dtfl_cv_sdt=yes], [dtfl_cv_sdt=no])])
	 AS_IF([test "x$dtfl_cv_sdt" = xyes], [enable_usdt=yes],
		[test "x$enable_usdt" = xyes],
		[AC_MSG_ERROR([--enable-usdt needs a sys/sdt.h with STAP_PROBEn (systemtap-sdt-dev)])],
		[enable_usdt=no])])
AS_IF([test "x$enable_usdt" = xyes],
	[AC_DEFINE([DTFL_USDT], [1], [Define to build the USDT static probes])])

# -------------------------------------------------------------------------
# Columnar fixed-width-key fast path (experiment).  Off by default.  When
# enabled, the trace's key container stores fixed-width, memcmp-ordered keys
# as a flat stride column and seeks them with a strided compare, instead of
# the faithful byte-arena-plus-offsets container.  This is a deliberate
# non-fidelity divergence from PLAN D2 (one container shape) kept behind a
# flag; it must produce byte-identical results.
# -------------------------------------------------------------------------
AC_ARG_ENABLE([columnar],
	[AS_HELP_STRING([--enable-columnar], [enable the fixed-width-key columnar trace fast path (non-fidelity experiment, off by default)])],
	[], [enable_columnar=no])
AS_IF([test "x$enable_columnar" = xyes],
	[AC_DEFINE([DTFL_COLUMNAR], [1], [Define to enable the columnar fixed-width-key trace fast path])])

# -------------------------------------------------------------------------
# Optional components.
# -------------------------------------------------------------------------
AC_ARG_ENABLE([diag],
	[AS_HELP_STRING([--disable-diag], [omit the WebSocket diagnostics])],
	[], [enable_diag=yes])
AM_CONDITIONAL([DTFL_DIAG], [test "x$enable_diag" = xyes])
AC_ARG_ENABLE([ddir],
	[AS_HELP_STRING([--disable-ddir], [omit the DDIR interpreter])],
	[], [enable_ddir=yes])
AM_CONDITIONAL([DTFL_DDIR], [test "x$enable_ddir" = xyes])
AC_ARG_ENABLE([server],
	[AS_HELP_STRING([--disable-server], [omit the dlopen plugin server])],
	[], [enable_server=yes])
AM_CONDITIONAL([DTFL_SERVER], [test "x$enable_server" = xyes])

# -------------------------------------------------------------------------
# hegel (property tests).  Off by default; property suites report SKIP.
# -------------------------------------------------------------------------
AC_ARG_WITH([hegel],
	[AS_HELP_STRING([--with-hegel@<:@=PREFIX@:>@], [enable hegel property tests (libhegel from hegeldev/hegel-rust)])],
	[], [with_hegel=no])
HEGEL_CFLAGS=""
HEGEL_LIBS=""
AS_IF([test "x$with_hegel" = xyes],
	[PKG_CHECK_MODULES([HEGEL], [hegel], [],
		[AC_MSG_ERROR([pkg-config cannot find hegel; pass --with-hegel=PREFIX])])],
	[test "x$with_hegel" != xno],
	[HEGEL_CFLAGS="-I$with_hegel/include"
	 HEGEL_LIBS="-L$with_hegel/lib -lhegel -Wl,-rpath,$with_hegel/lib"])
AS_IF([test "x$with_hegel" != xno],
	[AC_DEFINE([DTFL_HAVE_HEGEL], [1], [Define when hegel property tests are enabled])])
AC_SUBST([HEGEL_CFLAGS])
AC_SUBST([HEGEL_LIBS])
AM_CONDITIONAL([DTFL_HEGEL], [test "x$with_hegel" != xno])

# -------------------------------------------------------------------------
# Documentation tools (optional at build time; gates report SKIP).
# -------------------------------------------------------------------------
AC_CHECK_PROGS([MANDOC], [mandoc], [])
AC_CHECK_PROGS([NM], [nm], [nm])

AC_CONFIG_FILES([Makefile dtfl.pc])
AC_OUTPUT

AC_MSG_NOTICE([
libdtfl $PACKAGE_VERSION
  compiler:     $CC
  CFLAGS:       $CFLAGS
  debug:        $enable_debug
  sanitizers:   ${DTFL_SAN_FLAGS:-none}
  hegel:        $with_hegel
  diagnostics:  $enable_diag
  ddir:         $enable_ddir
  server:       $enable_server
  columnar:     $enable_columnar
  usdt probes:  $enable_usdt
  frame ptrs:   ${DTFL_FP_CFLAGS:-omitted}
])
