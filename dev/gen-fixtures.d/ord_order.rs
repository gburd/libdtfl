// SPDX-License-Identifier: MIT
//
// dev/gen-fixtures.d/ord_order.rs: the Ord-ordering fixture (PLAN D1).
//
// libdtfl's dtfl_type cmp must reproduce Rust's derived Ord for the type
// it models, because reduce hands its logic the values sorted by cmp and
// the built-in order-preserving encodings depend on it.  This program
// builds a set of composite values, sorts them by Rust's derived Ord,
// and emits each one in libdtfl's order-preserving encoding, in that
// sorted order.  The C interop test (tests/interop/test_ord_order.c)
// decodes the records, sorts them by dtfl_type_cmp, and asserts the
// permutation is the identity: that is, dtfl_type_cmp agrees with Rust
// Ord on every pair, and the encoding is correct.
//
// The modeled type is (i64, String, Option<u32>), which exercises the
// signed big-endian-with-flipped-sign-bit integer encoding, a
// variable-length string with a length prefix, and the Option tag byte
// (None < Some, upsert.rs tie rule).
//
// Fixture format (all integers big-endian, matching libdtfl on the wire):
//   u32  magic 0x4454464f ("DTFO")
//   u32  version = 1
//   u32  count
//   repeated count times:
//     u32 encoded length, then that many bytes of the libdtfl encoding.
// Records appear in Rust Ord-sorted order.

use std::io::{self, Write};

#[derive(Clone, PartialEq, Eq, PartialOrd, Ord)]
struct Rec {
    a: i64,
    b: String,
    c: Option<u32>,
}

// libdtfl order-preserving encodings (PLAN D1).
fn enc_i64(v: i64, out: &mut Vec<u8>) {
    // big-endian with the sign bit flipped: (v as u64) ^ (1<<63)
    let u = (v as u64) ^ (1u64 << 63);
    out.extend_from_slice(&u.to_be_bytes());
}
fn enc_u32(v: u32, out: &mut Vec<u8>) {
    out.extend_from_slice(&v.to_be_bytes());
}
fn enc_string(s: &str, out: &mut Vec<u8>) {
    // variable-length field: big-endian u64 length, then the raw bytes
    // (string compares as raw bytes, which for UTF-8 agrees with Rust's
    // byte-wise Ord on str).  libdtfl uses a u64 length prefix
    // (src/base/record.c), so the fixture must match that width.
    out.extend_from_slice(&(s.len() as u64).to_be_bytes());
    out.extend_from_slice(s.as_bytes());
}
fn enc_option_u32(v: &Option<u32>, out: &mut Vec<u8>) {
    // tag byte: None = 0, Some = 1 (so None < Some), then the value.
    match v {
        None => out.push(0),
        Some(x) => {
            out.push(1);
            enc_u32(*x, out);
        }
    }
}
fn encode(r: &Rec) -> Vec<u8> {
    let mut out = Vec::new();
    enc_i64(r.a, &mut out);
    enc_string(&r.b, &mut out);
    enc_option_u32(&r.c, &mut out);
    out
}

fn main() -> io::Result<()> {
    // A deterministic spread of values across every edge case the
    // encoding must get right: negative/zero/positive i64 including the
    // extremes, empty and prefix strings (so "a" < "ab"), and None vs
    // Some at the same prefix.
    let ints = [i64::MIN, -2, -1, 0, 1, 2, i64::MAX];
    let strs = ["", "a", "ab", "b", "\u{00}", "\u{7f}"];
    let opts = [None, Some(0u32), Some(1), Some(u32::MAX)];

    let mut recs = Vec::new();
    for &a in &ints {
        for s in &strs {
            for c in &opts {
                recs.push(Rec { a, b: s.to_string(), c: c.clone() });
            }
        }
    }
    recs.sort(); // Rust's derived Ord: a, then b, then c.

    let mut buf = Vec::new();
    buf.extend_from_slice(&0x4454464fu32.to_be_bytes());
    buf.extend_from_slice(&1u32.to_be_bytes());
    buf.extend_from_slice(&(recs.len() as u32).to_be_bytes());
    for r in &recs {
        let e = encode(r);
        buf.extend_from_slice(&(e.len() as u32).to_be_bytes());
        buf.extend_from_slice(&e);
    }
    io::stdout().write_all(&buf)
}
