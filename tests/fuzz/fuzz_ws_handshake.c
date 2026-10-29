/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_ws_handshake.c: the diagnostics server's request
 * parser (src/diag/ws_frame.c __dtfl_ws_handshake), which reads the
 * HTTP Upgrade request of any client that connects to the WebSocket
 * port.
 *
 * The input is the client's bytes, written to one end of a socketpair
 * whose write half is then shut, so the parser sees exactly those bytes
 * and then EOF.  On DTFL_OK the reply must be a whole 101 response with
 * a 28-character Sec-WebSocket-Accept.
 */
#include "fuzz.h"

#include "diag/diag_int.h"

#include <sys/socket.h>
#include <unistd.h>

/* The request buffer is 4 KiB; past that the parser must refuse. */
#define MAX_REQ 8192

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	int sv[2];
	int rc;
	char resp[512];
	ssize_t n;
	size_t got = 0;

	if (size > MAX_REQ)
		size = MAX_REQ;
	FUZZ_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	if (size > 0)
		FUZZ_ASSERT(write(sv[0], data, size) == (ssize_t)size);
	FUZZ_ASSERT(shutdown(sv[0], SHUT_WR) == 0);
	rc = __dtfl_ws_handshake(sv[1]);
	FUZZ_ASSERT(rc == DTFL_OK || rc == DTFL_EPROTO || rc == DTFL_EIO);
	(void)close(sv[1]);
	while ((n = read(sv[0], resp + got, sizeof resp - 1 - got)) > 0)
		got += (size_t)n;
	resp[got] = '\0';
	if (rc == DTFL_OK) {
		const char *acc = strstr(resp, "Sec-WebSocket-Accept: ");

		FUZZ_ASSERT(strncmp(resp, "HTTP/1.1 101 ", 13) == 0);
		FUZZ_ASSERT(acc != NULL);
		acc += strlen("Sec-WebSocket-Accept: ");
		FUZZ_ASSERT(strlen(acc) == 28 + 4);
		FUZZ_ASSERT(strcmp(acc + 28, "\r\n\r\n") == 0);
	} else {
		FUZZ_ASSERT(got == 0);
	}
	(void)close(sv[0]);
	return 0;
}
