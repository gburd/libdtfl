/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/log_codec_int.h: bincode-1 fixint little-endian writers.
 *
 * The encoding Rust's bincode 1.x uses by default, which every log and
 * capture stream at the pin uses (PLAN D9).  Each appends to a byte
 * vector (element size 1).
 */
#ifndef DTFL_LOG_CODEC_INT_H
#define DTFL_LOG_CODEC_INT_H

#include "base/base_int.h"

void __dtfl_bc_u8(__dtfl_vec *o, uint8_t v);
void __dtfl_bc_u32(__dtfl_vec *o, uint32_t v);
void __dtfl_bc_u64(__dtfl_vec *o, uint64_t v);
/* String: u64 length then UTF-8 bytes. */
void __dtfl_bc_str(__dtfl_vec *o, const char *s);
/* Vec<usize>: u64 length then u64 elements. */
void __dtfl_bc_usizes(__dtfl_vec *o, const size_t *v, size_t n);
/* Duration from nanoseconds: u64 seconds then u32 nanoseconds. */
void __dtfl_bc_duration(__dtfl_vec *o, uint64_t ns);

#endif /* DTFL_LOG_CODEC_INT_H */
