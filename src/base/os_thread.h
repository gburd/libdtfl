/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/os_thread.h: the one place libdtfl starts OS threads.
 *
 * Worker, network and connection threads all come from
 * __dtfl_thread_spawn so the API discipline gate can keep
 * pthread_create in a single file (AGENTS.md section 5).  A handle is
 * joined exactly once.
 */
#ifndef DTFL_OS_THREAD_H
#define DTFL_OS_THREAD_H

#include <pthread.h>

#include "base/base_int.h"

/*
 * A thread handle holds either a pthread_t (the default path) or the
 * opaque handle a host's thread hooks returned.  `hooked` says which,
 * so join takes the matching path.
 */
typedef struct __dtfl_thread {
	pthread_t tid;
	void *handle;
	bool hooked;
} __dtfl_thread;

/*
 * Starts fn(arg) on a new thread named `name` (truncated to what the OS
 * allows; informational only, and ignored on the hooked path).  Returns
 * 0 and fills *out, or DTFL_EIO with error detail if the OS or the host
 * hook refuses.
 */
int __dtfl_thread_spawn(const char *name, void *(*fn)(void *), void *arg,
    __dtfl_thread *out);

/* Waits for the thread to exit and returns what fn returned. */
void *__dtfl_thread_join(__dtfl_thread t);

/*
 * Forks a child process (DTFL_WORKERS_PROCESSES, ADR-0013).  In the
 * child, returns 0 and the child must end with __dtfl_process_exit;
 * in the parent, stores the child's id in *pid and returns 0; else
 * DTFL_EIO, or DTFL_ENOTSUP without fork(2).
 */
int __dtfl_process_fork(long *pid, bool *is_child);
/* Ends a forked child at once (_exit), with status 0 or 1. */
DTFL_NORETURN void __dtfl_process_exit(bool ok);
/*
 * Waits for child `pid`; returns 0 if it exited with status 0, else
 * DTFL_EIO with detail (the exit status or the signal).
 */
int __dtfl_process_wait(long pid);

#endif /* DTFL_OS_THREAD_H */
