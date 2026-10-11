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

typedef struct __dtfl_thread {
	pthread_t tid;
} __dtfl_thread;

/*
 * Starts fn(arg) on a new thread named `name` (truncated to what the OS
 * allows; informational only).  Returns 0 and fills *out, or DTFL_EIO
 * with error detail if the OS refuses.
 */
int __dtfl_thread_spawn(const char *name, void *(*fn)(void *), void *arg,
    __dtfl_thread *out);

/* Waits for the thread to exit and returns what fn returned. */
void *__dtfl_thread_join(__dtfl_thread t);

#endif /* DTFL_OS_THREAD_H */
