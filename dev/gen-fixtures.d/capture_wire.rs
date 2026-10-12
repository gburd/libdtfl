// SPDX-License-Identifier: MIT
//
// dev/gen-fixtures.d/capture_wire.rs: the capture wire-format fixture
// (PLAN D9: capture = u64 LE length + bincode Event<T, C>).
//
// This program uses real timely at the pin (a path dependency on the
// pinned checkout) to capture a tiny stream through timely's own
// EventWriter, so the fixture bytes are exactly what upstream writes:
// for each event, a u64 LE serialized_size followed by the bincode-1
// Event<T, C>.  The modeled stream has a u64 timestamp and Vec<u64>
// records, so T is a bare u64 and C is a Vec<u64>.
//
// tests/interop/test_capture_wire.c feeds the SAME logical events through
// libdtfl's capture codec (src/timely/event.c __dtfl_event_encode) and
// checks what is genuinely shared with Rust:
//   - the u64 LE outer length framing;
//   - the u32 LE Event variant tag (Progress = 0, Messages = 1);
//   - the Progress body for a depth-1 u64 time: a u64 LE count then,
//     per entry, a u64 LE time word and an i64 LE delta -- byte for byte.
// The Messages payload is NOT byte-identical: libdtfl encodes the record
// batch in its own byte-arena layout (batch_int.h), while Rust bincodes
// the typed Vec<u64>.  The C test documents that divergence and asserts
// only the shared prefix (length, tag, time) for Messages.  Honesty over
// a green checkmark (PLAN D9: record bytes are libdtfl's).
//
// Fixture bytes are exactly timely's EventWriter output, nothing added:
//   Event 0: Progress(vec![(0, 1), (3, -1)])
//   Event 1: Messages(0, vec![10, 20, 30])

use std::io::Write;
use timely::dataflow::operators::capture::event::binary::EventWriter;
use timely::dataflow::operators::capture::event::{Event, EventPusher};

fn main() {
    let mut buf: Vec<u8> = Vec::new();
    {
        // EventWriter<T, C, W>: T = u64 time, C = Vec<u64> records.
        let mut w: EventWriter<u64, Vec<u64>, _> = EventWriter::new(&mut buf);
        w.push(Event::Progress(vec![(0u64, 1i64), (3u64, -1i64)]));
        w.push(Event::Messages(0u64, vec![10u64, 20u64, 30u64]));
    }
    std::io::stdout().write_all(&buf).unwrap();
}
