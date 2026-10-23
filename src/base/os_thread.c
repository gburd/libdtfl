/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/os_thread.c: thread and process creation and join.  The only
 * file in src/ that may call pthread_create, fork or waitpid (AGENTS.md
 * section 5, ADR-0013).
 */
#if defined(__linux__)
#define _GNU_SOURCE	/* pthread_setname_np */
#endif

#include "base/os_thread.h"

#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

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
	/* Darwin's pthread_setname_np names only the calling thread; skipped. */
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

int
__dtfl_process_fork(long *pid, bool *is_child)
{
#ifdef HAVE_FORK
	pid_t p = fork();

	if (p < 0)
		return __dtfl_err(DTFL_EIO, "fork: %s", strerror(errno));
	*is_child = p == 0;
	*pid = (long)p;
	return 0;
#else
	(void)pid; (void)is_child;
	return __dtfl_err(DTFL_ENOTSUP, "fork(2) is not available");
#endif
}

void
__dtfl_process_exit(bool ok)
{
	_exit(ok ? 0 : 1);
}

int
__dtfl_process_wait(long pid)
{
#ifdef HAVE_FORK
	int status;
	pid_t r;

	do {
		r = waitpid((pid_t)pid, &status, 0);
	} while (r < 0 && errno == EINTR);
	if (r < 0)
		return __dtfl_err(DTFL_EIO, "waitpid(%ld): %s", pid,
		    strerror(errno));
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return 0;
	if (WIFSIGNALED(status))
		return __dtfl_err(DTFL_EIO, "worker process %ld killed by "
		    "signal %d", pid, WTERMSIG(status));
	return __dtfl_err(DTFL_EIO, "worker process %ld exited with "
	    "status %d", pid, WEXITSTATUS(status));
#else
	(void)pid;
	return __dtfl_err(DTFL_ENOTSUP, "fork(2) is not available");
#endif
}
