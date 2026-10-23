#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_api_discipline.sh: library code rules (AGENTS.md section 5).
#
# 1. No raw allocation in src/ outside src/base/os_alloc.c.
# 2. No pthread_create in src/ outside src/base/os_thread.c.
# 3. No getenv, signal/sigaction, or getopt anywhere in src/.
# 3a. No lock, wait, yield, sleep, clock or thread-identity call in src/
#     outside src/base/os_sync.c, so the sync hooks reach every one
#     (ADR-0013).
# 4. Examples and tools include only public headers (dtfl/*.h).
# A line carrying /* DTFL_RAW_OK: reason */ is exempt.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$top" || exit 99
fail=0

check() {
	desc="$1"; pattern="$2"; shift 2
	# Lines that are comment text (start with *, / * or //) are not code.
	hits=$(grep -nE "$pattern" "$@" 2>/dev/null | grep -v 'DTFL_RAW_OK' \
	    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|/\*|//)' || true)
	if [ -n "$hits" ]; then
		echo "[api] FAIL: $desc"
		echo "$hits" | sed 's/^/    /'
		fail=1
	fi
}

srcs=$(find src -name '*.c' -o -name '*.h' | sort)
alloc_srcs=$(echo "$srcs" | grep -v '^src/base/os_alloc.c$')
thread_srcs=$(echo "$srcs" | grep -v '^src/base/os_thread.c$')
sync_srcs=$(echo "$srcs" | grep -v '^src/base/os_sync.c$')

check "raw allocation outside src/base/os_alloc.c" \
	'(^|[^_a-zA-Z0-9.>])(malloc|calloc|realloc|free|strdup|strndup)[[:space:]]*\(' $alloc_srcs
check "pthread_create outside src/base/os_thread.c" \
	'pthread_create[[:space:]]*\(' $thread_srcs
check "lock, wait, sleep, clock or thread identity outside src/base/os_sync.c" \
	'(^|[^_a-zA-Z0-9])(pthread_mutex_[a-z]+|pthread_cond_[a-z]+|pthread_rwlock_[a-z]+|pthread_spin_[a-z]+|sem_(wait|post|init|timedwait)|sched_yield|nanosleep|usleep|clock_gettime|pthread_self|pthread_equal|mtx_[a-z]+|cnd_[a-z]+|thrd_yield|thrd_sleep)[[:space:]]*\(|SYS_futex' $sync_srcs
check "environment, signal or getopt use in library code" \
	'(^|[^_a-zA-Z0-9])(getenv|secure_getenv|setenv|signal|sigaction|getopt|getopt_long)[[:space:]]*\(' $srcs

consumers=$(find examples tools -name '*.c' -o -name '*.h' 2>/dev/null | sort)
if [ -n "$consumers" ]; then
	check "examples/tools include internal headers" \
		'#[[:space:]]*include[[:space:]]*"[^"]*_int\.h"|#[[:space:]]*include[[:space:]]*"(src|\.\./src)/' $consumers
	check "examples/tools use internal __dtfl_ symbols" '__dtfl_' $consumers
fi

[ $fail -eq 0 ] && echo "[api] OK"
exit $fail
