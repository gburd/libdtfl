/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/alloc_canary.c: a helper that reports when something has
 * been dropped.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/canary.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Rust reports from Drop; C reports from the explicit drop call that
 * the owner (a byte allocator's puller) makes when it is destroyed.
 */
#include "comm/comm_int.h"

void
__dtfl_canary_drop(__dtfl_canary *c)
{
	__dtfl_vec_push(c->queue, &c->index);
	c->queue = NULL;
}
