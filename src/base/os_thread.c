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

/*
 * Thread hooks are process-wide and set before the first
 * dtfl_execute(), like the allocation and panic hooks.  Reads use
 * acquire so a worker thread sees the fully written structure.
 */
static dtfl_thread_hooks thread_hooks;
static atomic_bool thread_hooks_set;

int
dtfl_set_thread_hooks(const dtfl_thread_hooks *h)
{
	if (h == NULL || (h->spawn == NULL && h->join == NULL)) {
		atomic_store_explicit(&thread_hooks_set, false,
		    memory_order_release);
		return DTFL_OK;
	}
	if (h->spawn == NULL || h->join == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "thread hooks need both spawn and join");
	thread_hooks = *h;
	atomic_store_explicit(&thread_hooks_set, true, memory_order_release);
	return DTFL_OK;
}

int
__dtfl_thread_spawn(const char *name, void *(*fn)(void *), void *arg,
    __dtfl_thread *out)
{
	int rc;

	out->hooked = false;
	out->handle = NULL;
	if (atomic_load_explicit(&thread_hooks_set, memory_order_acquire)) {
		rc = thread_hooks.spawn(fn, arg, &out->handle, thread_hooks.ctx);
		if (rc != 0)
			return __dtfl_err(DTFL_EIO, "thread hook spawn(%s): %s",
			    name != NULL ? name : "?", dtfl_strerror(rc));
		out->hooked = true;
		return 0;
	}
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

	if (t.hooked) {
		rc = thread_hooks.join(t.handle, &ret, thread_hooks.ctx);
		if (rc != 0)
			__dtfl_panic("thread hook join: %s", dtfl_strerror(rc));
		return ret;
	}
	rc = pthread_join(t.tid, &ret);
	if (rc != 0)
		__dtfl_panic("pthread_join: %s", strerror(rc));
	return ret;
}
