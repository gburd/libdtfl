/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_log_walk.h: re-encode a decoded log batch
 * (tests/fuzz/fuzz_log_walk.c).
 */
#ifndef DTFL_FUZZ_LOG_WALK_H
#define DTFL_FUZZ_LOG_WALK_H

#include "fuzz.h"

#include "base/base_int.h"
#include "dtfl/log.h"

/* Appends u32 length + bincode record for each decoded event of b. */
void fuzz_log_walk(const dtfl_log_batch *b, __dtfl_vec *out);

#endif /* DTFL_FUZZ_LOG_WALK_H */
