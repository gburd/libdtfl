/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/probes_int.h: USDT static probes (DTrace and SystemTap).
 *
 * DTFL_PROBEn(name, args...) places probe `name` of provider "dtfl" with
 * n arguments.  With --enable-usdt (DTFL_USDT) each expands to
 * sys/sdt.h's STAP_PROBEn: one nop at the site and an ELF note
 * (.note.stapsdt) saying where each argument lives.  No semaphore is
 * used, so an untraced probe costs the nop plus placing its arguments
 * where the note says (free under gcc, a few register moves under
 * clang).
 * Without DTFL_USDT each expands to an empty statement and its
 * arguments are not evaluated, so arguments must have no side effects
 * and no variable may exist only to feed a probe.
 *
 * Pass every argument as uint64_t, const void * or const char *, so its
 * width is the same everywhere.  The probe list, with arguments, is in
 * doc/diagnostics.texi; tests/dist/test_usdt.sh checks the library's
 * notes against it, so add a probe to both.
 */
#ifndef DTFL_PROBES_INT_H
#define DTFL_PROBES_INT_H

#include "config.h"

#ifdef DTFL_USDT
/*
 * On x86 clang satisfies sys/sdt.h's default "nor" constraint by
 * spilling every argument to the stack; "nr" keeps them in registers.
 */
#if defined(__clang__) && defined(__x86_64__) && \
    !defined(STAP_SDT_ARG_CONSTRAINT)
#define STAP_SDT_ARG_CONSTRAINT nr
#endif
#include <sys/sdt.h>

/*
 * sys/sdt.h passes an empty variadic argument internally, which clang's
 * -Wpedantic reports as a C23 extension.  Silence it at the probe only.
 * The pragmas must be in each replacement list, not around a macro
 * argument: an argument is expanded before the pragmas take effect.
 */
#ifdef __clang__
#define DTFL_PROBE_PUSH_						\
	_Pragma("clang diagnostic push")				\
	_Pragma("clang diagnostic ignored \"-Wpedantic\"")
#define DTFL_PROBE_POP_	_Pragma("clang diagnostic pop")
#else
#define DTFL_PROBE_PUSH_
#define DTFL_PROBE_POP_
#endif

#define DTFL_PROBE1(n, a)						\
	DTFL_PROBE_PUSH_ STAP_PROBE1(dtfl, n, a) DTFL_PROBE_POP_
#define DTFL_PROBE2(n, a, b)						\
	DTFL_PROBE_PUSH_ STAP_PROBE2(dtfl, n, a, b) DTFL_PROBE_POP_
#define DTFL_PROBE3(n, a, b, c)						\
	DTFL_PROBE_PUSH_ STAP_PROBE3(dtfl, n, a, b, c) DTFL_PROBE_POP_
#define DTFL_PROBE4(n, a, b, c, d)					\
	DTFL_PROBE_PUSH_ STAP_PROBE4(dtfl, n, a, b, c, d) DTFL_PROBE_POP_
#define DTFL_PROBE5(n, a, b, c, d, e)					\
	DTFL_PROBE_PUSH_ STAP_PROBE5(dtfl, n, a, b, c, d, e) DTFL_PROBE_POP_

#else /* !DTFL_USDT */

#define DTFL_PROBE1(n, a)			do { } while (0)
#define DTFL_PROBE2(n, a, b)			do { } while (0)
#define DTFL_PROBE3(n, a, b, c)			do { } while (0)
#define DTFL_PROBE4(n, a, b, c, d)		do { } while (0)
#define DTFL_PROBE5(n, a, b, c, d, e)		do { } while (0)

#endif /* DTFL_USDT */

#endif /* DTFL_PROBES_INT_H */
