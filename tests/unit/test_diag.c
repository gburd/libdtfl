/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_diag.c: the in-process diagnostics session and
 * WebSocket server (dtfl/diag.h, src/diag/).
 *
 * Checks:
 *   diag__frame_over_ws   start the server on 127.0.0.1 ephemeral port,
 *                         run a tiny dataflow with the session
 *                         subscribed, step it so the operators log,
 *                         connect a raw client, do the RFC 6455
 *                         handshake, read the snapshot Frame, and assert
 *                         it is well-formed JSON naming the operators
 *                         (Operator updates) and at least one channel
 *                         (Channel update).
 *   diag__bind_loopback   the default bind is 127.0.0.1 (D12): a connect
 *                         to 127.0.0.1 at the server's port succeeds.
 *   diag__slow_handshake  a client that sends its Upgrade request well
 *                         after the server accepted it still gets the
 *                         101: the accepted socket blocks, bounded by
 *                         its I/O timeout, even where accept(2) copies
 *                         the listener's O_NONBLOCK (Darwin, the BSDs).
 *
 * Every socket call has a timeout so a stuck accept or read fails the
 * test instead of hanging the suite.
 */
#include "dtfl_test.h"

#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"
#include "dtfl/diag.h"

#include "base/os_sync.h"	/* __dtfl_sleep_ns */
#include "timely/scope_int.h"	/* struct dtfl_worker { __dtfl_worker *w; } */

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* A tiny, long-lived dataflow                                         */
/* ------------------------------------------------------------------ */

static void
inspect_noop(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx; (void)time; (void)rec; (void)len;
}

/*
 * An input-fed dataflow: the operators stay live as long as the input
 * is open, so the diagnostics snapshot a connecting client reads shows
 * them (a shut-down operator is retracted from the live state, exactly
 * as upstream's DD collection retracts it).  The caller keeps the input
 * in build_ctx and closes it after reading the snapshot.
 */
typedef struct build_ctx { dtfl_input *in; } build_ctx;

static void
build_two_ops(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s;

	c->in = dtfl_input_new(dtfl_type_u64());
	s = dtfl_input_to_stream(c->in, scope);
	(void)dtfl_stream_inspect(s, inspect_noop, DTFL_NOCTX);
}

/* ------------------------------------------------------------------ */
/* A minimal WebSocket client                                          */
/* ------------------------------------------------------------------ */

/* Connects to 127.0.0.1:port with a short timeout.  Returns fd or -1. */
static int
connect_client(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa;
	struct timeval tv = { 5, 0 };

	if (fd < 0)
		return -1;
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		(void)close(fd);
		return -1;
	}
	return fd;
}

/* Sends an RFC 6455 Upgrade request and checks for the 101 response. */
static bool
ws_client_handshake(int fd)
{
	const char *req =
	    "GET / HTTP/1.1\r\n"
	    "Host: localhost\r\n"
	    "Upgrade: websocket\r\n"
	    "Connection: Upgrade\r\n"
	    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
	    "Sec-WebSocket-Version: 13\r\n\r\n";
	char resp[1024];
	size_t used = 0;

	if (send(fd, req, strlen(req), 0) < 0)
		return false;
	/*
	 * Read the response headers one byte at a time until the blank
	 * line, so no bytes of the following WebSocket frame are consumed
	 * (the server sends the snapshot frame right after the 101).
	 */
	for (;;) {
		ssize_t n = recv(fd, resp + used, 1, 0);

		if (n != 1)
			return false;
		used++;
		if (used >= sizeof resp)
			return false;
		if (used >= 4 && memcmp(resp + used - 4, "\r\n\r\n", 4) == 0)
			break;
	}
	resp[used] = '\0';
	if (strstr(resp, "101") == NULL)
		return false;
	/* The accept for this key per RFC 6455. */
	return strstr(resp, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL;
}

/*
 * Reads one unmasked server text frame into buf (NUL-terminated).
 * Returns the payload length, or -1 on error/timeout.  Only short
 * frames (< 126 bytes) and medium frames (126, 16-bit length) are
 * handled, which covers the diagnostics Frames in this test.
 */
static ssize_t
ws_client_read_text(int fd, char *buf, size_t cap)
{
	unsigned char hdr[2];
	size_t payload_len, off = 0;
	ssize_t n;

	if (recv(fd, hdr, 2, MSG_WAITALL) != 2)
		return -1;
	if ((hdr[0] & 0x0f) != 0x1)	/* text opcode */
		return -1;
	payload_len = hdr[1] & 0x7f;
	if (payload_len == 126) {
		unsigned char ext[2];

		if (recv(fd, ext, 2, MSG_WAITALL) != 2)
			return -1;
		payload_len = ((size_t)ext[0] << 8) | ext[1];
	}
	if (payload_len >= cap)
		return -1;
	while (off < payload_len) {
		n = recv(fd, buf + off, payload_len - off, 0);
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	buf[payload_len] = '\0';
	return (ssize_t)payload_len;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

DTFL_TEST(diag__frame_over_ws)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diag_session *s;
	dtfl_diag_server *srv = NULL;
	build_ctx c = { 0 };
	uint16_t port;
	int fd;
	char frame[8192];
	ssize_t len;
	int steps;

	(void)params; (void)data;

	s = dtfl_diag_session_new(w);
	assert_not_null(s);
	assert_int(dtfl_diag_server_start(s, "127.0.0.1", 0, &srv), ==,
	    DTFL_OK);
	port = dtfl_diag_server_port(srv);
	assert_uint16(port, !=, 0);

	/*
	 * Build the input-fed dataflow and step it so the Operates and
	 * Channels events are logged into the session state.  The input
	 * stays open, so the operators are live when the client connects
	 * (a shut-down operator is retracted from the live state, as it is
	 * from upstream's DD collection; a browser connects to a running
	 * computation).
	 */
	assert_int(dtfl_worker_dataflow(w, "diag", tc, build_two_ops, &c), ==,
	    0);
	for (steps = 0; steps < 3; steps++)
		(void)dtfl_worker_step(w);

	/* Connect a raw client and do the handshake. */
	fd = connect_client(port);
	assert_int(fd, >=, 0);
	assert_true(ws_client_handshake(fd));

	/*
	 * Read exactly one frame, the full-state snapshot the server
	 * replays on connect.  The socket has a 5s receive timeout, so a
	 * missing snapshot fails the read instead of hanging; reading just
	 * one frame keeps the stream aligned (a partial header read under
	 * a retry loop would desync the framing).  The server delivers the
	 * snapshot from its own thread, so no stepping is needed here.
	 */
	(void)steps;
	len = ws_client_read_text(fd, frame, sizeof frame);
	assert_int((int)len, >, 0);

	/* The Frame is well-formed and names the operators and a channel. */
	assert_not_null(strstr(frame, "\"type\":\"Frame\""));
	assert_not_null(strstr(frame, "\"updates\":["));
	assert_not_null(strstr(frame, "\"type\":\"Operator\""));
	assert_not_null(strstr(frame, "\"name\":\"Input\""));
	assert_not_null(strstr(frame, "\"name\":\"InspectBatch\""));
	assert_not_null(strstr(frame, "\"type\":\"Channel\""));
	assert_not_null(strstr(frame, "\"source\":["));
	assert_not_null(strstr(frame, "\"target\":["));

	(void)close(fd);
	dtfl_input_close(c.in);
	while (dtfl_worker_step(w))
		;
	dtfl_diag_server_stop(srv);
	dtfl_diag_session_free(s);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

DTFL_TEST(diag__bind_loopback)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_diag_session *s = dtfl_diag_session_new(w);
	dtfl_diag_server *srv = NULL;
	uint16_t port;
	int fd;

	(void)params; (void)data;
	assert_not_null(s);
	/* NULL iface defaults to the loopback (D12). */
	assert_int(dtfl_diag_server_start(s, NULL, 0, &srv), ==, DTFL_OK);
	port = dtfl_diag_server_port(srv);
	assert_uint16(port, !=, 0);

	fd = connect_client(port);
	assert_int(fd, >=, 0);	/* loopback accepts the connection */
	(void)close(fd);

	dtfl_diag_server_stop(srv);
	dtfl_diag_session_free(s);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

DTFL_TEST(diag__slow_handshake)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_diag_session *s = dtfl_diag_session_new(w);
	dtfl_diag_server *srv = NULL;
	int fd;

	(void)params; (void)data;
	assert_not_null(s);
	assert_int(dtfl_diag_server_start(s, NULL, 0, &srv), ==, DTFL_OK);
	fd = connect_client(dtfl_diag_server_port(srv));
	assert_int(fd, >=, 0);
	/* Three of the serve loop's 100 ms passes: it has accepted by now. */
	__dtfl_sleep_ns(300000000ULL);
	assert_true(ws_client_handshake(fd));
	(void)close(fd);

	dtfl_diag_server_stop(srv);
	dtfl_diag_session_free(s);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(diag__frame_over_ws),
	DTFL_T(diag__bind_loopback),
	DTFL_T(diag__slow_handshake),
	DTFL_T_END
};

DTFL_TEST_MAIN("diag", tests)
