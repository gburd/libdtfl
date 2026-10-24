/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/hashable.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/base/hash.c: the record hash used for exchange routing.
 *
 * Upstream's blanket `impl<T: Hash> Hashable for T` feeds the value
 * through std::hash::Hash into an FnvHasher.  libdtfl records are bytes
 * (PLAN D1), so the default hashes the record's encoding with 64-bit
 * FNV-1a (__dtfl_fnv1a in util.c).  The routing this produces is not
 * bit-compatible with Rust's: Hash writes integers little-endian and
 * prefixes slices with a usize length, while libdtfl encodes integers
 * big-endian and sign-flipped.  A cluster is therefore all-libdtfl
 * (PLAN D9).  Equal records still hash equal, which is all exchange
 * needs.
 */
#include "base_int.h"

#include "dtfl/record.h"

/* Hashable::hashed: the descriptor's hash, or FNV-1a of the bytes. */
uint64_t
dtfl_type_hash(const dtfl_type *t, const void *p, size_t len)
{
	if (t->hash != NULL)
		return t->hash(t, p, len);
	return __dtfl_fnv1a(p, len);
}
