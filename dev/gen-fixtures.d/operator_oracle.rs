// SPDX-License-Identifier: MIT
//
// dev/gen-fixtures.d/operator_oracle.rs: the operator-output oracle (D9,
// PLAN section 6 tier 7).  This is the strongest fidelity check: a real
// differential-dataflow computation at the pin produces the consolidated
// output updates (record, time, diff), and the C interop test
// (tests/interop/test_operator_oracle.c) runs the SAME computation in
// libdtfl and asserts the multiset of updates is identical, byte for byte
// on the output records.
//
// It uses real differential-dataflow 0.25.1 and timely 0.31.0 (the pins,
// pulled from crates.io by exact version).  Each computation drives an
// InputSession across epochs with insertions and retractions, consolidates
// the output (so the multiset is canonical), captures the inner stream,
// and extracts the (record, time, diff) triples.
//
// Records are emitted in libdtfl's order-preserving D1 encoding (big-endian
// integers, i64 with the sign bit flipped: dtfl_put_u64 / dtfl_put_i64), so
// the C test, which collects records in that same encoding, compares bytes
// directly.  The record encoding is libdtfl's, not Rust bincode; what this
// proves is that the C operators compute the SAME consolidated multiset of
// (record, time, diff) as Rust, which is the operator-correctness claim.
//
// Covered: join, count, reduce(min with distinct-count diff).
//
// Fixture format (all framing integers little-endian):
//   u32 magic 0x4454464e ("DTFN")
//   u32 version = 1
//   u32 section count = 3
//   per section:
//     u32 name length, name bytes ("join" / "count" / "reduce")
//     u32 update count
//     per update:
//       u32 record length, record bytes (libdtfl D1 encoding)
//       u64 time
//       i64 diff (two's complement, little-endian)
// Updates appear sorted by (record bytes, time) so the file is canonical.

use differential_dataflow::input::InputSession;
use timely::dataflow::operators::{capture::Extract, Capture};

// libdtfl order-preserving encodings (dtfl/record.c), so the C side reads
// the record bytes with the identical layout.
fn enc_u64(v: u64, out: &mut Vec<u8>) {
    out.extend_from_slice(&v.to_be_bytes());
}
fn enc_i64(v: i64, out: &mut Vec<u8>) {
    // big-endian with the sign bit flipped: (v as u64) ^ (1 << 63).
    out.extend_from_slice(&((v as u64) ^ (1u64 << 63)).to_be_bytes());
}

struct Update {
    rec: Vec<u8>,
    time: u64,
    diff: i64,
}

struct Section {
    name: &'static str,
    updates: Vec<Update>,
}

fn finish(mut updates: Vec<Update>) -> Vec<Update> {
    // Sort for a canonical file; the C test compares as a multiset so the
    // order is cosmetic, but a stable file keeps the fixture reproducible.
    updates.sort_by(|a, b| a.rec.cmp(&b.rec).then(a.time.cmp(&b.time)));
    updates
}

// ---- join: (key, v1) join (key, v2) -> (key, (v1, v2)) ---------------
fn section_join() -> Section {
    let extracted = timely::execute_directly(move |worker| {
        let mut in1: InputSession<u64, (u64, u64), isize> = InputSession::new();
        let mut in2: InputSession<u64, (u64, u64), isize> = InputSession::new();
        let (probe, cap) = worker.dataflow::<u64, _, _>(|scope| {
            let c1 = in1.to_collection(scope);
            let c2 = in2.to_collection(scope);
            let (probe, r) = c1.join(c2).consolidate().probe();
            (probe, r.inner.capture())
        });
        // epoch 0: c1 = {(0,10),(1,20)}; c2 = {(0,100),(1,200)}.
        in1.insert((0, 10));
        in1.insert((1, 20));
        in2.insert((0, 100));
        in2.insert((1, 200));
        in1.advance_to(1);
        in2.advance_to(1);
        in1.flush();
        in2.flush();
        worker.step_while(|| probe.less_than(in1.time()) || probe.less_than(in2.time()));
        // epoch 1: retract (1,20) from c1, insert (1,21); add (2,30) to c1
        // and (2,300) to c2 so a new key appears.
        in1.remove((1, 20));
        in1.insert((1, 21));
        in1.insert((2, 30));
        in2.insert((2, 300));
        in1.advance_to(2);
        in2.advance_to(2);
        in1.flush();
        in2.flush();
        worker.step_while(|| probe.less_than(in1.time()) || probe.less_than(in2.time()));
        in1.close();
        in2.close();
        cap
    })
    .extract();

    let mut updates = Vec::new();
    for (_cap, batch) in &extracted {
        for ((key, (v1, v2)), time, diff) in batch {
            let mut rec = Vec::new();
            enc_u64(*key, &mut rec);
            enc_u64(*v1, &mut rec);
            enc_u64(*v2, &mut rec);
            updates.push(Update {
                rec,
                time: *time,
                diff: *diff as i64,
            });
        }
    }
    Section {
        name: "join",
        updates: finish(updates),
    }
}

// ---- count: key -> (key, count) --------------------------------------
fn section_count() -> Section {
    let extracted = timely::execute_directly(move |worker| {
        let mut input: InputSession<u64, u64, isize> = InputSession::new();
        let (probe, cap) = worker.dataflow::<u64, _, _>(|scope| {
            let c = input.to_collection(scope);
            let (probe, r) = c.count().consolidate().probe();
            (probe, r.inner.capture())
        });
        // epoch 0: keys 0,1,1,1,2,2  -> counts (0,1),(1,3),(2,2).
        input.insert(0);
        input.insert(1);
        input.insert(1);
        input.insert(1);
        input.insert(2);
        input.insert(2);
        input.advance_to(1);
        input.flush();
        worker.step_while(|| probe.less_than(input.time()));
        // epoch 1: remove two 1s (count 3 -> 1) and remove both 2s
        // (count 2 -> 0, so key 2 disappears).
        input.remove(1);
        input.remove(1);
        input.remove(2);
        input.remove(2);
        input.advance_to(2);
        input.flush();
        worker.step_while(|| probe.less_than(input.time()));
        input.close();
        cap
    })
    .extract();

    let mut updates = Vec::new();
    for (_cap, batch) in &extracted {
        for ((key, count), time, diff) in batch {
            let mut rec = Vec::new();
            enc_u64(*key, &mut rec);
            // count() emits the summed input diff as the value; libdtfl
            // renders it as an i64 (dtfl_diff_i64), big-endian sign-flipped.
            enc_i64(*count as i64, &mut rec);
            updates.push(Update {
                rec,
                time: *time,
                diff: *diff as i64,
            });
        }
    }
    Section {
        name: "count",
        updates: finish(updates),
    }
}

// ---- reduce(min): (key, val) -> (key, min val), diff = #distinct -----
fn section_reduce() -> Section {
    let extracted = timely::execute_directly(move |worker| {
        let mut input: InputSession<u64, (u64, u64), isize> = InputSession::new();
        let (probe, cap) = worker.dataflow::<u64, _, _>(|scope| {
            let c = input.to_collection(scope);
            // The values arrive sorted; emit the smallest with a diff equal
            // to the number of distinct values (upstream reduce.rs idiom and
            // the libdtfl smallest_logic in tests/upstream/dd/reduce.c).
            let (probe, r) = c
                .reduce(|_k, s, t| {
                    t.push((*s[0].0, s.len() as isize));
                })
                .consolidate()
                .probe();
            (probe, r.inner.capture())
        });
        // epoch 0: key 0 -> {5,3,9}; key 1 -> {2}.
        input.insert((0, 5));
        input.insert((0, 3));
        input.insert((0, 9));
        input.insert((1, 2));
        input.advance_to(1);
        input.flush();
        worker.step_while(|| probe.less_than(input.time()));
        // epoch 1: retract (0,3) so key 0's min becomes 5 over {5,9}.
        input.remove((0, 3));
        input.advance_to(2);
        input.flush();
        worker.step_while(|| probe.less_than(input.time()));
        input.close();
        cap
    })
    .extract();

    let mut updates = Vec::new();
    for (_cap, batch) in &extracted {
        for ((key, minval), time, diff) in batch {
            let mut rec = Vec::new();
            enc_u64(*key, &mut rec);
            enc_u64(*minval, &mut rec);
            updates.push(Update {
                rec,
                time: *time,
                diff: *diff as i64,
            });
        }
    }
    Section {
        name: "reduce",
        updates: finish(updates),
    }
}

fn main() {
    use std::io::Write;

    let sections = vec![section_join(), section_count(), section_reduce()];

    let mut buf: Vec<u8> = Vec::new();
    buf.extend_from_slice(&0x4454464eu32.to_le_bytes()); // "DTFN"
    buf.extend_from_slice(&1u32.to_le_bytes());
    buf.extend_from_slice(&(sections.len() as u32).to_le_bytes());
    for s in &sections {
        buf.extend_from_slice(&(s.name.len() as u32).to_le_bytes());
        buf.extend_from_slice(s.name.as_bytes());
        buf.extend_from_slice(&(s.updates.len() as u32).to_le_bytes());
        for u in &s.updates {
            buf.extend_from_slice(&(u.rec.len() as u32).to_le_bytes());
            buf.extend_from_slice(&u.rec);
            buf.extend_from_slice(&u.time.to_le_bytes());
            buf.extend_from_slice(&u.diff.to_le_bytes());
        }
    }
    std::io::stdout().write_all(&buf).unwrap();
}
