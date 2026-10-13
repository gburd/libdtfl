/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/diag/scc-bench.c: mirrors diagnostics/examples/scc-bench.rs.
 *
 * TODO (blocked): scc-bench runs strongly_connected (differential-
 * dataflow algorithms::graphs::scc) under the diagnostics console.  The
 * scc operator is not yet usable: it depends on the ord_neu / compaction
 * fix tracked separately (the scc ord_neu bug), owned by another worker.
 * Once scc lands, this example mirrors scc-bench.rs: register the diag
 * session, start the server on worker 0, build strongly_connected over a
 * generated edge stream, and run rounds of changes.  Until then it is a
 * placeholder that prints a one-line notice and exits, so the port
 * ledger row stays honestly todo rather than claiming a working binary.
 *
 * The working, scc-free counterpart is examples/diag/smoke.c, which runs
 * the full diagnostics path (session + WebSocket server + Frame stream)
 * against a map/probe dataflow.
 */
#include <stdio.h>

int
main(void)
{
	fprintf(stderr,
	    "scc-bench: not yet available (blocked on the scc ord_neu fix); "
	    "see examples/diag/smoke for the diagnostics path.\n");
	return 0;
}
