/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ws_frame.c: the WebSocket handshake primitives and
 * the Frame JSON builder (src/diag/ws_frame.c).
 *
 * Checks:
 *   wsframe__sha1       SHA-1 against the RFC 3174 known-answer vectors.
 *   wsframe__base64     base64 against known vectors, every pad length.
 *   wsframe__accept     the RFC 6455 Sec-WebSocket-Accept example.
 *   wsframe__operator   an Operator update serializes to the exact JSON
 *                       index.html consumes.
 *   wsframe__channel    a Channel update serializes with source/target
 *                       as 2-element arrays and the scope address.
 *   wsframe__stat       a Stat update names its kind.
 *   wsframe__empty      a frame with no updates finishes as NULL.
 */
#include "dtfl_test.h"

#include "diag/diag_int.h"

#include <string.h>

static void
hex(const unsigned char *d, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++)
		snprintf(out + 2 * i, 3, "%02x", d[i]);
}

DTFL_TEST(wsframe__sha1)
{
	unsigned char digest[20];
	char h[41];

	(void)params; (void)data;

	/* RFC 3174 / FIPS 180-1 known answers. */
	__dtfl_sha1("abc", 3, digest);
	hex(digest, 20, h);
	assert_string_equal(h, "a9993e364706816aba3e25717850c26c9cd0d89d");

	__dtfl_sha1("", 0, digest);
	hex(digest, 20, h);
	assert_string_equal(h, "da39a3ee5e6b4b0d3255bfef95601890afd80709");

	__dtfl_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	    56, digest);
	hex(digest, 20, h);
	assert_string_equal(h, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

	/* A long message whose remainder (1000 % 64 = 40) fits one pad. */
	{
		char buf[1000];

		memset(buf, 'a', sizeof buf);
		__dtfl_sha1(buf, 1000, digest);
		hex(digest, 20, h);
		assert_string_equal(h,
		    "291e9a6c66994949b57ba5e650361e98fc36b1ba");
	}

	/* 60 bytes: remainder 60 > 55, forcing a second padding block. */
	{
		char buf[60];

		memset(buf, 'a', sizeof buf);
		__dtfl_sha1(buf, 60, digest);
		hex(digest, 20, h);
		assert_string_equal(h,
		    "13d956033d9af449bfe2c4ef78c17c20469c4bf1");
	}
	return MUNIT_OK;
}

DTFL_TEST(wsframe__base64)
{
	char out[64];

	(void)params; (void)data;

	__dtfl_base64_encode("", 0, out);
	assert_string_equal(out, "");
	__dtfl_base64_encode("f", 1, out);
	assert_string_equal(out, "Zg==");
	__dtfl_base64_encode("fo", 2, out);
	assert_string_equal(out, "Zm8=");
	__dtfl_base64_encode("foo", 3, out);
	assert_string_equal(out, "Zm9v");
	__dtfl_base64_encode("foobar", 6, out);
	assert_string_equal(out, "Zm9vYmFy");
	return MUNIT_OK;
}

DTFL_TEST(wsframe__accept)
{
	/* The RFC 6455 section 1.3 example. */
	const char *key = "dGhlIHNhbXBsZSBub25jZQ==";
	char concat[128], accept[64];
	unsigned char digest[20];

	(void)params; (void)data;
	snprintf(concat, sizeof concat,
	    "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
	__dtfl_sha1(concat, strlen(concat), digest);
	__dtfl_base64_encode(digest, 20, accept);
	assert_string_equal(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	return MUNIT_OK;
}

DTFL_TEST(wsframe__operator)
{
	__dtfl_diag_frame f;
	size_t addr[2] = { 0, 2 };
	char *s;

	(void)params; (void)data;
	__dtfl_diag_frame_begin(&f);
	__dtfl_diag_frame_add_operator(&f, 7, "Map", addr, 2, 1);
	s = __dtfl_diag_frame_finish(&f, 1500);
	assert_not_null(s);
	assert_string_equal(s,
	    "{\"type\":\"Frame\",\"ts_us\":1500,\"updates\":["
	    "{\"type\":\"Operator\",\"id\":7,\"name\":\"Map\","
	    "\"addr\":[0,2],\"diff\":1}]}");
	__dtfl_free(s);
	return MUNIT_OK;
}

DTFL_TEST(wsframe__channel)
{
	__dtfl_diag_frame f;
	size_t scope[1] = { 0 };
	char *s;

	(void)params; (void)data;
	__dtfl_diag_frame_begin(&f);
	__dtfl_diag_frame_add_channel(&f, 3, scope, 1, 1, 0, 2, 1, 1);
	s = __dtfl_diag_frame_finish(&f, 0);
	assert_not_null(s);
	assert_string_equal(s,
	    "{\"type\":\"Frame\",\"ts_us\":0,\"updates\":["
	    "{\"type\":\"Channel\",\"id\":3,\"scope_addr\":[0],"
	    "\"source\":[1,0],\"target\":[2,1],\"diff\":1}]}");
	__dtfl_free(s);
	return MUNIT_OK;
}

DTFL_TEST(wsframe__stat)
{
	__dtfl_diag_frame f;
	char *s;

	(void)params; (void)data;
	__dtfl_diag_frame_begin(&f);
	__dtfl_diag_frame_add_stat(&f, DIAG_STAT_ELAPSED, 7, 42);
	s = __dtfl_diag_frame_finish(&f, 0);
	assert_not_null(s);
	assert_string_equal(s,
	    "{\"type\":\"Frame\",\"ts_us\":0,\"updates\":["
	    "{\"type\":\"Stat\",\"kind\":\"Elapsed\",\"id\":7,"
	    "\"diff\":42}]}");
	__dtfl_free(s);

	assert_string_equal(__dtfl_diag_stat_kind_str(DIAG_STAT_MESSAGES),
	    "Messages");
	assert_string_equal(
	    __dtfl_diag_stat_kind_str(DIAG_STAT_ARRANGEMENT_RECORDS),
	    "ArrangementRecords");
	return MUNIT_OK;
}

DTFL_TEST(wsframe__empty)
{
	__dtfl_diag_frame f;

	(void)params; (void)data;
	__dtfl_diag_frame_begin(&f);
	assert_null(__dtfl_diag_frame_finish(&f, 0));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(wsframe__sha1),
	DTFL_T(wsframe__base64),
	DTFL_T(wsframe__accept),
	DTFL_T(wsframe__operator),
	DTFL_T(wsframe__channel),
	DTFL_T(wsframe__stat),
	DTFL_T(wsframe__empty),
	DTFL_T_END
};

DTFL_TEST_MAIN("ws_frame", tests)
