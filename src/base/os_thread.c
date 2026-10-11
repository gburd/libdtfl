/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/os_thread.c: thread creation and join.  The only file in
 * src/ that may call pthread_create (AGENTS.md section 5).
 */
#if defined(__linux__)
#define _GNU_SOURCE	/* pthread_setname_np */
#endif

#include "base/os_thread.h"

#include <errno.h>
#include <string.h>

int
__dtfl_thread_spawn(const char *name, void *(*fn)(void *), void *arg,
    __dtfl_thread *out)
{
	int rc;

	rc = pthread_create(&out->tid, NULL, fn, arg);
	if (rc != 0)
		return __dtfl_err(DTFL_EIO, "pthread_create(%s): %s",
		    name != NULL ? name : "?", strerror(rc));
#if defined(__linux__)
	if (name != NULL) {
		char buf[16];	/* Linux limit, including the NUL */

		strncpy(buf, name, sizeof buf - 1);
		buf[sizeof buf - 1] = '\0';
		(void)pthread_setname_np(out->tid, buf);
	}
#endif
	return 0;
}

void *
__dtfl_thread_join(__dtfl_thread t)
{
	void *ret = NULL;
	int rc;

	rc = pthread_join(t.tid, &ret);
	if (rc != 0)
		__dtfl_panic("pthread_join: %s", strerror(rc));
	return ret;
}
