// SPDX-License-Identifier: MIT
//
// dev/gen-fixtures.d/fuzz_oracle.rs: Rust-computed answers for a fixed
// seed set, anchoring the count/distinct fuzz oracle to real differential
// (PLAN section 6 tiers 4 and 7).
//
// tests/pbt/pbt_oracle.c drives a broad random search of count and distinct
// against a from-scratch C model.  That is a self-check (C against C).  To
// make it a RUST cross-check on a sample, this generator replays a FIXED
// set of seeds through real differential-dataflow at the pin and emits the
// consolidated output updates.  tests/interop/test_fuzz_oracle.c replays
// the IDENTICAL seeded input streams through libdtfl and asserts the output
// multiset equals these Rust answers.
//
// The input stream for a seed is produced by a splitmix64 PRNG and a
// generation rule that the C side reproduces bit for bit (same seed, same
// stream).  Constraints (kept small so a case finishes well under a second):
//   keys in 0..8, updates <= 16, epochs <= 3, |diff| in 1..=3.
//
// Two operators per seed: count and distinct.  Records are libdtfl's D1
// encoding (big-endian u64 key, count value as sign-flipped big-endian i64),
// matching dtfl_put_u64 / dtfl_put_i64 so the C side compares bytes.
//
// Fixture format (framing little-endian):
//   u32 magic 0x4454465a ("DTFZ")
//   u32 version = 1
//   u32 seed count
//   per seed:
//     u64 seed
//     one count section then one distinct section, each:
//       u32 update count
//       per update: u32 reclen, reclen record bytes, u64 time, i64 diff

use differential_dataflow::input::InputSession;
use timely::dataflow::operators::{capture::Extract, Capture};

// ---- shared PRNG and stream generation (mirrored in pbt_oracle.c) ----
const NKEYS: u64 = 8;
const MAX_UPDATES: u32 = 16;
const MAX_EPOCHS: u64 = 3;

fn splitmix64(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9e3779b97f4a7c15);
    let mut z = *state;
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
    z ^ (z >> 31)
}

// One generated update: (key, epoch, diff).  Must match the C generator.
struct Gen {
    key: u64,
    epoch: u64,
    diff: i64,
}

fn gen_stream(seed: u64) -> Vec<Gen> {
    let mut st = seed;
    let n = (splitmix64(&mut st) % (MAX_UPDATES as u64 + 1)) as usize;
    let mut v = Vec::with_capacity(n);
    let mut epoch = 0u64;
    for _ in 0..n {
        let key = splitmix64(&mut st) % NKEYS;
        // epoch is non-decreasing: 0 or advance by up to 1, capped.
        if splitmix64(&mut st) % 2 == 0 && epoch + 1 < MAX_EPOCHS {
            epoch += 1;
        }
        let mag = (splitmix64(&mut st) % 3 + 1) as i64; // 1..=3
        let diff = if splitmix64(&mut st) % 2 == 0 { mag } else { -mag };
        v.push(Gen { key, epoch, diff });
    }
    v
}

fn enc_u64(v: u64, out: &mut Vec<u8>) {
    out.extend_from_slice(&v.to_be_bytes());
}
fn enc_i64(v: i64, out: &mut Vec<u8>) {
    out.extend_from_slice(&((v as u64) ^ (1u64 << 63)).to_be_bytes());
}

struct Update {
    rec: Vec<u8>,
    time: u64,
    diff: i64,
}

fn sort_updates(mut u: Vec<Update>) -> Vec<Update> {
    u.sort_by(|a, b| a.rec.cmp(&b.rec).then(a.time.cmp(&b.time)));
    u
}

// Run count on a seeded stream; return the consolidated output updates.
fn run_count(stream: &[Gen]) -> Vec<Update> {
    let stream: Vec<Gen> = stream
        .iter()
        .map(|g| Gen {
            key: g.key,
            epoch: g.epoch,
            diff: g.diff,
        })
        .collect();
    let extracted = timely::execute_directly(move |worker| {
        let mut input: InputSession<u64, u64, isize> = InputSession::new();
        let (probe, cap) = worker.dataflow::<u64, _, _>(|scope| {
            let c = input.to_collection(scope);
            let (probe, r) = c.count().consolidate().probe();
            (probe, r.inner.capture())
        });
        // Feed the stream epoch by epoch, stepping to each epoch frontier.
        let mut cur = 0u64;
        for g in &stream {
            if g.epoch > cur {
                cur = g.epoch;
                input.advance_to(cur);
                input.flush();
                worker.step_while(|| probe.less_than(input.time()));
            }
            input.update(g.key, g.diff as isize);
        }
        input.advance_to(cur + 1);
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
            enc_i64(*count as i64, &mut rec);
            updates.push(Update {
                rec,
                time: *time,
                diff: *diff as i64,
            });
        }
    }
    sort_updates(updates)
}

// Run distinct on a seeded stream; return the consolidated output updates.
fn run_distinct(stream: &[Gen]) -> Vec<Update> {
    let stream: Vec<Gen> = stream
        .iter()
        .map(|g| Gen {
            key: g.key,
            epoch: g.epoch,
            diff: g.diff,
        })
        .collect();
    let extracted = timely::execute_directly(move |worker| {
        let mut input: InputSession<u64, u64, isize> = InputSession::new();
        let (probe, cap) = worker.dataflow::<u64, _, _>(|scope| {
            let c = input.to_collection(scope);
            let (probe, r) = c.distinct().consolidate().probe();
            (probe, r.inner.capture())
        });
        let mut cur = 0u64;
        for g in &stream {
            if g.epoch > cur {
                cur = g.epoch;
                input.advance_to(cur);
                input.flush();
                worker.step_while(|| probe.less_than(input.time()));
            }
            input.update(g.key, g.diff as isize);
        }
        input.advance_to(cur + 1);
        input.flush();
        worker.step_while(|| probe.less_than(input.time()));
        input.close();
        cap
    })
    .extract();
    let mut updates = Vec::new();
    for (_cap, batch) in &extracted {
        // distinct emits the bare key with diff +/- 1.
        for (key, time, diff) in batch {
            let mut rec = Vec::new();
            enc_u64(*key, &mut rec);
            updates.push(Update {
                rec,
                time: *time,
                diff: *diff as i64,
            });
        }
    }
    sort_updates(updates)
}

fn write_section(buf: &mut Vec<u8>, updates: &[Update]) {
    buf.extend_from_slice(&(updates.len() as u32).to_le_bytes());
    for u in updates {
        buf.extend_from_slice(&(u.rec.len() as u32).to_le_bytes());
        buf.extend_from_slice(&u.rec);
        buf.extend_from_slice(&u.time.to_le_bytes());
        buf.extend_from_slice(&u.diff.to_le_bytes());
    }
}

fn main() {
    use std::io::Write;

    // A fixed, modest seed set.  Deterministic, so the fixture is stable.
    let seeds: Vec<u64> = (1u64..=32).map(|i| i.wrapping_mul(0x100000001b3)).collect();

    let mut buf: Vec<u8> = Vec::new();
    buf.extend_from_slice(&0x4454465au32.to_le_bytes()); // "DTFZ"
    buf.extend_from_slice(&1u32.to_le_bytes());
    buf.extend_from_slice(&(seeds.len() as u32).to_le_bytes());
    for &seed in &seeds {
        let stream = gen_stream(seed);
        let c = run_count(&stream);
        let d = run_distinct(&stream);
        buf.extend_from_slice(&seed.to_le_bytes());
        write_section(&mut buf, &c);
        write_section(&mut buf, &d);
    }
    std::io::stdout().write_all(&buf).unwrap();
}
