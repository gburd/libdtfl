// SPDX-License-Identifier: MIT
//
// dev/gen-fixtures.d/log_stream.rs: the timely log-record fixture (D9).
//
// libdtfl's log codec (src/timely/timely_log.c) encodes each TimelyEvent
// as the bincode-1 fixint little-endian value of the TimelyEvent enum:
// a u32 variant tag in declaration order, then the struct fields (usize
// as u64, String and Vec as a u64 length then elements, Duration as u64
// seconds then u32 nanoseconds).  This program emits, for a couple of
// variants, exactly bincode::serialize(&TimelyEvent::...) using real
// timely types at the pin, so the C encoder's bytes can be checked
// against upstream byte for byte (tests/interop/test_log_stream.c).
//
// Note on scope: the C functions (__dtfl_worker_log_operates, etc.)
// encode a BARE TimelyEvent, not the BatchLogger stream wrapper
// Event<Duration, Vec<(Duration, TimelyEvent)>>.  The public logging
// registration that would emit the wrapped stream is not wired through
// a public API yet, so this fixture proves the TimelyEvent record layout
// (the part the C codec actually produces), not the outer batch framing.
//
// Fixture format (so the C test can find each record without bincode):
//   u32 LE magic 0x4454464c ("DTFL")
//   u32 LE version = 1
//   u32 LE count
//   repeated count times: u32 LE record length, then that many bincode
//   bytes of one TimelyEvent.
// Records, in order:
//   0: Operates { id = 7, addr = [0, 2], name = "Map" }
//   1: Channels { id = 3, scope_addr = [0], source = (1, 0),
//                 target = (2, 1), typ = "u64" }
//   2: Schedule { id = 7, start_stop = Start }
//   3: Shutdown { id = 7 }
//   4: Messages { is_send = true, channel = 3, source = 0, target = 1,
//                 seq_no = 5, record_count = 42 }
//   5: Park(Some(1_500_000_000 ns)) via ParkEvent::Park

use std::io::Write;
use timely::logging::{
    ChannelsEvent, MessagesEvent, OperatesEvent, ParkEvent, ScheduleEvent, ShutdownEvent,
    StartStop, TimelyEvent,
};

fn emit(out: &mut Vec<u8>, ev: &TimelyEvent) {
    let bytes = bincode::serialize(ev).expect("bincode TimelyEvent");
    out.extend_from_slice(&(bytes.len() as u32).to_le_bytes());
    out.extend_from_slice(&bytes);
}

fn main() {
    let events = vec![
        TimelyEvent::Operates(OperatesEvent {
            id: 7,
            addr: vec![0, 2],
            name: "Map".to_string(),
        }),
        TimelyEvent::Channels(ChannelsEvent {
            id: 3,
            scope_addr: vec![0],
            source: (1, 0),
            target: (2, 1),
            typ: "u64".to_string(),
        }),
        TimelyEvent::Schedule(ScheduleEvent {
            id: 7,
            start_stop: StartStop::Start,
        }),
        TimelyEvent::Shutdown(ShutdownEvent { id: 7 }),
        TimelyEvent::Messages(MessagesEvent {
            is_send: true,
            channel: 3,
            source: 0,
            target: 1,
            seq_no: 5,
            record_count: 42,
        }),
        TimelyEvent::Park(ParkEvent::Park(Some(std::time::Duration::from_nanos(
            1_500_000_000,
        )))),
    ];

    let mut buf: Vec<u8> = Vec::new();
    buf.extend_from_slice(&0x4454464cu32.to_le_bytes()); // "DTFL"
    buf.extend_from_slice(&1u32.to_le_bytes());
    buf.extend_from_slice(&(events.len() as u32).to_le_bytes());
    for ev in &events {
        emit(&mut buf, ev);
    }
    std::io::stdout().write_all(&buf).unwrap();
}
