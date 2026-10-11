/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/record.h: record type descriptors (PLAN D1).
 *
 * A record is a byte string.  A dtfl_type says how long a value is, how
 * values compare (which must equal the Rust type's derived Ord), how
 * they hash for exchange, and how to print one.  The built-in integer
 * descriptors use order-preserving encodings, so their cmp is memcmp;
 * bytes and string compare their payloads, not their length prefixes.
 *
 * Descriptors are immutable and may be shared between threads.  The
 * built-ins are static and never freed; composite descriptors made with
 * dtfl_type_tuple() and friends are reference counted.
 */
#ifndef DTFL_RECORD_H
#define DTFL_RECORD_H

#include <stdio.h>

#include "dtfl/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtfl_type dtfl_type;

struct dtfl_type {
	const char *name;
	/* Byte length of every value, or 0 if values vary in length. */
	size_t fixed_size;
	/*
	 * For variable-length types: the length of the value starting at p,
	 * given `avail` readable bytes.  Returns SIZE_MAX if the bytes are
	 * not a valid value (0 is a valid length: unit, empty tuples).
	 * NULL when fixed_size != 0, or when the value is always the whole
	 * remaining span (a user type used only as the last field).
	 */
	size_t (*len)(const dtfl_type *t, const void *p, size_t avail);
	/* Three-way compare.  NULL means memcmp then length. */
	int (*cmp)(const dtfl_type *t, const void *a, size_t alen,
	    const void *b, size_t blen);
	/* Hash for exchange routing.  NULL means FNV-1a of the bytes. */
	uint64_t (*hash)(const dtfl_type *t, const void *p, size_t len);
	/* Prints a value; NULL prints hex.  Returns fprintf's result. */
	int (*fmt)(const dtfl_type *t, FILE *f, const void *p, size_t len);
	/* Composite descriptors: fields; user descriptors: anything. */
	void *ctx;
	/* Releases ctx when a reference-counted descriptor is freed. */
	void (*ctx_free)(void *ctx);
	/* Internal; zero in user descriptors. */
	void *internal;
};

/* Built-in descriptors (static). */
DTFL_API const dtfl_type *dtfl_type_unit(void);	/* () : 0 bytes */
DTFL_API const dtfl_type *dtfl_type_u8(void);
DTFL_API const dtfl_type *dtfl_type_u16(void);
DTFL_API const dtfl_type *dtfl_type_u32(void);
DTFL_API const dtfl_type *dtfl_type_u64(void);
DTFL_API const dtfl_type *dtfl_type_i8(void);
DTFL_API const dtfl_type *dtfl_type_i16(void);
DTFL_API const dtfl_type *dtfl_type_i32(void);
DTFL_API const dtfl_type *dtfl_type_i64(void);
/*
 * bytes and string: a u64 big-endian length then the bytes, so they can
 * appear anywhere in a tuple.  Order: lexicographic over the bytes, then
 * length, as Rust's Vec<u8> and String.
 */
DTFL_API const dtfl_type *dtfl_type_bytes(void);
DTFL_API const dtfl_type *dtfl_type_string(void);

/*
 * Composite descriptors (reference counted, freed with dtfl_type_free).
 * tuple: fields in order, compared field by field.
 * option: one tag byte (0 None, 1 Some) then the value; None < Some.
 * vec: u64 big-endian count then the elements; compared element by
 *      element, then by count, as Rust's Vec Ord.
 */
DTFL_API const dtfl_type *dtfl_type_tuple(const dtfl_type *const *fields,
    size_t n);
DTFL_API const dtfl_type *dtfl_type_pair(const dtfl_type *a, const dtfl_type *b);
DTFL_API const dtfl_type *dtfl_type_option(const dtfl_type *inner);
DTFL_API const dtfl_type *dtfl_type_vec(const dtfl_type *elem);

/*
 * Wraps a caller-defined descriptor so the library can reference count
 * it.  The library copies *def; ctx_free (if set) runs when the last
 * reference goes away.
 */
DTFL_API const dtfl_type *dtfl_type_new(const dtfl_type *def);

DTFL_API const dtfl_type *dtfl_type_retain(const dtfl_type *t);
/* No-op for the static built-ins. */
DTFL_API void dtfl_type_free(const dtfl_type *t);

/* Operations through a descriptor. */
DTFL_API int dtfl_type_cmp(const dtfl_type *t, const void *a, size_t alen,
    const void *b, size_t blen);
DTFL_API uint64_t dtfl_type_hash(const dtfl_type *t, const void *p, size_t len);
DTFL_API int dtfl_type_fmt(const dtfl_type *t, FILE *f, const void *p,
    size_t len);
/*
 * Length of the value at p (fixed_size, len(), or avail), or SIZE_MAX if
 * the bytes are not a valid value within avail.
 */
DTFL_API size_t dtfl_type_value_len(const dtfl_type *t, const void *p,
    size_t avail);

/* Number of fields in a tuple descriptor, and the i'th field. */
DTFL_API size_t dtfl_type_tuple_len(const dtfl_type *t);
DTFL_API const dtfl_type *dtfl_type_tuple_field(const dtfl_type *t, size_t i);
/*
 * Locates field i of a tuple value.  Returns the field's offset and
 * stores its length in *flen.  Returns SIZE_MAX if the value is
 * malformed.
 */
DTFL_API size_t dtfl_type_tuple_offset(const dtfl_type *t, const void *p,
    size_t len, size_t i, size_t *flen);

/*
 * Order-preserving encodings for the built-in integer types.  `put`
 * writes into buf (which must have room) and returns bytes written;
 * `get` reads a value of the type's width.
 */
DTFL_API size_t dtfl_put_u8(void *buf, uint8_t v);
DTFL_API size_t dtfl_put_u16(void *buf, uint16_t v);
DTFL_API size_t dtfl_put_u32(void *buf, uint32_t v);
DTFL_API size_t dtfl_put_u64(void *buf, uint64_t v);
DTFL_API size_t dtfl_put_i8(void *buf, int8_t v);
DTFL_API size_t dtfl_put_i16(void *buf, int16_t v);
DTFL_API size_t dtfl_put_i32(void *buf, int32_t v);
DTFL_API size_t dtfl_put_i64(void *buf, int64_t v);
DTFL_API uint8_t dtfl_get_u8(const void *buf);
DTFL_API uint16_t dtfl_get_u16(const void *buf);
DTFL_API uint32_t dtfl_get_u32(const void *buf);
DTFL_API uint64_t dtfl_get_u64(const void *buf);
DTFL_API int8_t dtfl_get_i8(const void *buf);
DTFL_API int16_t dtfl_get_i16(const void *buf);
DTFL_API int32_t dtfl_get_i32(const void *buf);
DTFL_API int64_t dtfl_get_i64(const void *buf);
/* bytes/string values: writes the length prefix and the bytes. */
DTFL_API size_t dtfl_put_bytes(void *buf, const void *p, size_t n);
/* Reads a bytes/string value; stores the payload length in *n. */
DTFL_API const void *dtfl_get_bytes(const void *buf, size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_RECORD_H */
