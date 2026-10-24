/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_transport.c: dtfl_transport_socket, the built-in
 * byte transport (ADR-0013), against the contract in dtfl_transport(3):
 * bytes arrive in order, an empty recv is "none now", a full socket is
 * a short send, wait times out and wake ends it from another thread,
 * and shutdown turns into DTFL_EOF at the peer.
 */
#define _POSIX_C_SOURCE 200809L

#include "dtfl_test.h"

#include "dtfl/config.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Two transports, 0 and 1, joined by one socketpair. */
static void
pair(dtfl_transport *t0, dtfl_transport *t1)
{
	int sv[2], f0[2], f1[2];

	assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
	f0[0] = -1; f0[1] = sv[0];
	f1[0] = sv[1]; f1[1] = -1;
	assert_int(dtfl_transport_socket(f0, 2, 0, t0), ==, 0);
	assert_int(dtfl_transport_socket(f1, 2, 1, t1), ==, 0);
}

DTFL_TEST(transport_socket_moves_bytes_in_order)
{
	dtfl_transport a, b;
	unsigned char out[4096], in[4096];
	size_t sent, got, total = 0, i;

	(void)params; (void)data;
	pair(&a, &b);
	for (i = 0; i < sizeof out; i++)
		out[i] = (unsigned char)(i * 7);
	/* Nothing yet: zero bytes, not an error. */
	assert_int(b.recv(b.ctx, 0, in, sizeof in, &got), ==, 0);
	assert_size(got, ==, 0);
	assert_int(a.send(a.ctx, 1, out, sizeof out, &sent), ==, 0);
	assert_size(sent, ==, sizeof out);
	while (total < sizeof out) {
		b.wait(b.ctx, UINT64_C(1000000000));
		assert_int(b.recv(b.ctx, 0, in + total, sizeof in - total,
		    &got), ==, 0);
		total += got;
	}
	assert_memory_equal(sizeof out, in, out);
	/* Shutdown becomes end of stream at the peer. */
	assert_int(a.shutdown(a.ctx, 1), ==, 0);
	b.wait(b.ctx, UINT64_C(1000000000));
	assert_int(b.recv(b.ctx, 0, in, sizeof in, &got), ==, DTFL_EOF);
	/* Bad peers are refused, not crashed on. */
	assert_int(a.send(a.ctx, 0, out, 1, &sent), ==, DTFL_EINVAL);
	assert_int(a.recv(a.ctx, 5, in, 1, &got), ==, DTFL_EINVAL);
	a.destroy(a.ctx);
	b.destroy(b.ctx);
	return MUNIT_OK;
}

DTFL_TEST(transport_socket_full_is_a_short_send)
{
	dtfl_transport a, b;
	static unsigned char buf[1 << 16];
	size_t sent = 1, got, rounds = 0;
	uint64_t t0;

	(void)params; (void)data;
	pair(&a, &b);
	/* Fill the socket until a send takes nothing. */
	while (sent > 0) {
		assert_int(a.send(a.ctx, 1, buf, sizeof buf, &sent), ==, 0);
		assert_size(++rounds, <, 10000);
	}
	/* The sender's wait for room times out while nobody reads. */
	t0 = now_ns();
	a.wait(a.ctx, UINT64_C(20000000));
	assert_ullong(now_ns() - t0, >=, UINT64_C(15000000));
	/*
	 * Once the reader drains it, the writer's wait returns at once, not
	 * at its 5 s timeout: the short send made wait poll for POLLOUT.
	 */
	do {
		assert_int(b.recv(b.ctx, 0, buf, sizeof buf, &got), ==, 0);
	} while (got > 0);
	t0 = now_ns();
	a.wait(a.ctx, UINT64_C(5000000000));
	assert_ullong(now_ns() - t0, <, UINT64_C(1000000000));
	assert_int(a.send(a.ctx, 1, buf, 1, &sent), ==, 0);
	assert_size(sent, ==, 1);
	a.destroy(a.ctx);
	b.destroy(b.ctx);
	return MUNIT_OK;
}

static void *
waker(void *arg)
{
	dtfl_transport *t = arg;
	struct timespec d = { 0, 30000000 };	/* 30 ms */

	nanosleep(&d, NULL);
	t->wake(t->ctx);
	return NULL;
}

DTFL_TEST(transport_socket_wake_ends_wait)
{
	dtfl_transport a, b;
	pthread_t th;
	uint64_t t0, dt;

	(void)params; (void)data;
	pair(&a, &b);
	t0 = now_ns();
	assert_int(pthread_create(&th, NULL, waker, &a), ==, 0);
	a.wait(a.ctx, UINT64_MAX);
	dt = now_ns() - t0;
	pthread_join(th, NULL);
	assert_ullong(dt, <, UINT64_C(5000000000));
	/* A wake before the wait makes the next wait return at once. */
	a.wake(a.ctx);
	t0 = now_ns();
	a.wait(a.ctx, UINT64_C(5000000000));
	assert_ullong(now_ns() - t0, <, UINT64_C(1000000000));
	a.destroy(a.ctx);
	b.destroy(b.ctx);
	return MUNIT_OK;
}

DTFL_TEST(transport_socket_rejects_bad_arguments)
{
	dtfl_transport t;
	int fds[2] = { -1, -1 };

	(void)params; (void)data;
	assert_int(dtfl_transport_socket(NULL, 2, 0, &t), ==, DTFL_EINVAL);
	assert_int(dtfl_transport_socket(fds, 2, 2, &t), ==, DTFL_EINVAL);
	/* The peer's descriptor must be open. */
	assert_int(dtfl_transport_socket(fds, 2, 0, &t), ==, DTFL_EINVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(transport_socket_moves_bytes_in_order),
	DTFL_T(transport_socket_full_is_a_short_send),
	DTFL_T(transport_socket_wake_ends_wait),
	DTFL_T(transport_socket_rejects_bad_arguments),
	DTFL_T_END
};

DTFL_TEST_MAIN("transport", tests)
