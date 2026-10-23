#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/run-test.sh: the LOG_COMPILER for every make check test.
#
# Runs one test under a time limit so a hang is a clean FAIL instead of
# a stalled make.  DTFL_TEST_TIMEOUT sets the limit in seconds (default
# 600; 0 disables it).  A test that exceeds it is killed, its log says
# so, and it exits 124, which the automake driver reports as FAIL.  Any
# DTFL_TEST_WRAPPER configure chose (setarch -R under TSan) still runs
# inside the limit.  Exit 77 (SKIP) and 99 (hard error) pass through.
limit="${DTFL_TEST_TIMEOUT:-600}"
case $limit in
''|*[!0-9]*) echo "run-test: DTFL_TEST_TIMEOUT must be a number of seconds" >&2; exit 99 ;;
esac
if [ "$limit" -eq 0 ] || ! command -v timeout >/dev/null 2>&1; then
	exec ${DTFL_TEST_WRAPPER:-} "$@"
fi
# --kill-after: a test that ignores SIGTERM (a stuck pthread_join) dies.
${DTFL_TEST_WRAPPER:-} timeout --kill-after=10 "$limit" "$@"
rc=$?
if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
	echo "run-test: FAIL: $* did not finish within ${limit}s (DTFL_TEST_TIMEOUT)" >&2
	exit 124
fi
exit "$rc"
