/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/diag/ws_frame.c: the Frame JSON builder and the RFC 6455
 * WebSocket primitives (SHA-1, base64, handshake, text framing).
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   diagnostics/src/server.rs
 * Copyright (c) 2019 Timely Dataflow Diagnostics contributors.
 *
 * server.rs serializes JsonUpdate values with serde_json and relies on
 * the tungstenite crate for the WebSocket handshake and framing.  This
 * file replaces both with the minimum built in: a hand-rolled JSON
 * writer that produces the exact Frame shape index.html consumes, plus
 * the small SHA-1 and base64 the RFC 6455 Sec-WebSocket-Accept needs
 * (PLAN, in-process diagnostics "built-in SHA-1 and base64").
 */
#define _GNU_SOURCE	/* MSG_NOSIGNAL */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "diag/diag_int.h"

/* ------------------------------------------------------------------ */
/* SHA-1 (RFC 3174)                                                    */
/* ------------------------------------------------------------------ */

static uint32_t
rol32(uint32_t v, int n)
{
	return (v << n) | (v >> (32 - n));
}

static void
sha1_block(uint32_t h[5], const unsigned char block[64])
{
	uint32_t w[80];
	uint32_t a, b, c, d, e;
	size_t i;

	for (i = 0; i < 16; i++)
		w[i] = ((uint32_t)block[4 * i] << 24) |
		    ((uint32_t)block[4 * i + 1] << 16) |
		    ((uint32_t)block[4 * i + 2] << 8) |
		    ((uint32_t)block[4 * i + 3]);
	for (i = 16; i < 80; i++)
		w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
	a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
	for (i = 0; i < 80; i++) {
		uint32_t f, k, t;

		if (i < 20) {
			f = (b & c) | ((~b) & d);
			k = 0x5A827999u;
		} else if (i < 40) {
			f = b ^ c ^ d;
			k = 0x6ED9EBA1u;
		} else if (i < 60) {
			f = (b & c) | (b & d) | (c & d);
			k = 0x8F1BBCDCu;
		} else {
			f = b ^ c ^ d;
			k = 0xCA62C1D6u;
		}
		t = rol32(a, 5) + f + e + k + w[i];
		e = d; d = c; c = rol32(b, 30); b = a; a = t;
	}
	h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void
__dtfl_sha1(const void *data, size_t len, unsigned char out[20])
{
	uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
		0xC3D2E1F0u };
	unsigned char block[64];
	uint64_t bitlen = (uint64_t)len * 8;
	const unsigned char *p = data;
	size_t i, off = 0, rem;

	/* Full 64-byte blocks. */
	for (off = 0; len - off >= 64; off += 64)
		sha1_block(h, p + off);

	/* Final block(s): remaining bytes, 0x80, zeros, 64-bit length. */
	rem = len - off;
	memcpy(block, p + off, rem);
	block[rem] = 0x80;
	if (rem <= 55) {
		memset(block + rem + 1, 0, 55 - rem);
		for (i = 0; i < 8; i++)
			block[56 + i] = (unsigned char)(bitlen >> (56 - 8 * i));
		sha1_block(h, block);
	} else {
		memset(block + rem + 1, 0, 63 - rem);
		sha1_block(h, block);
		memset(block, 0, 56);
		for (i = 0; i < 8; i++)
			block[56 + i] = (unsigned char)(bitlen >> (56 - 8 * i));
		sha1_block(h, block);
	}

	for (i = 0; i < 5; i++) {
		out[4 * i] = (unsigned char)(h[i] >> 24);
		out[4 * i + 1] = (unsigned char)(h[i] >> 16);
		out[4 * i + 2] = (unsigned char)(h[i] >> 8);
		out[4 * i + 3] = (unsigned char)(h[i]);
	}
}

/* ------------------------------------------------------------------ */
/* base64                                                              */
/* ------------------------------------------------------------------ */

size_t
__dtfl_base64_encode(const void *data, size_t len, char *out)
{
	static const char tbl[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const unsigned char *p = data;
	size_t i, o = 0;

	for (i = 0; i + 3 <= len; i += 3) {
		uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) |
		    p[i + 2];

		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = tbl[(v >> 6) & 0x3f];
		out[o++] = tbl[v & 0x3f];
	}
	if (len - i == 1) {
		uint32_t v = (uint32_t)p[i] << 16;

		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = '=';
		out[o++] = '=';
	} else if (len - i == 2) {
		uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);

		out[o++] = tbl[(v >> 18) & 0x3f];
		out[o++] = tbl[(v >> 12) & 0x3f];
		out[o++] = tbl[(v >> 6) & 0x3f];
		out[o++] = '=';
	}
	out[o] = '\0';
	return o;
}

/* ------------------------------------------------------------------ */
/* WebSocket handshake and framing (RFC 6455)                          */
/* ------------------------------------------------------------------ */

/* The magic GUID from RFC 6455 section 1.3. */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

static bool
send_all(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t off = 0;

	while (off < len) {
		ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		off += (size_t)n;
	}
	return true;
}

int
__dtfl_ws_handshake(int fd)
{
	char req[4096];
	size_t used = 0;
	const char *keyhdr = "Sec-WebSocket-Key:";
	char key[128], concat[256], resp[256];
	unsigned char digest[20];
	char accept[64];
	const char *line, *end;
	size_t keylen;

	/* Read the request headers until the blank line (CRLFCRLF). */
	for (;;) {
		ssize_t n;

		if (used >= sizeof req - 1)
			return __dtfl_err(DTFL_EPROTO,
			    "websocket request too large");
		n = recv(fd, req + used, sizeof req - 1 - used, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return __dtfl_err(DTFL_EIO, "websocket recv: %s",
			    strerror(errno));
		}
		if (n == 0)
			return __dtfl_err(DTFL_EPROTO,
			    "websocket client closed during handshake");
		used += (size_t)n;
		req[used] = '\0';
		if (strstr(req, "\r\n\r\n") != NULL)
			break;
	}

	/* Find the Sec-WebSocket-Key header (case-insensitive). */
	line = req;
	key[0] = '\0';
	while (line != NULL && *line != '\0') {
		if (strncasecmp(line, keyhdr, strlen(keyhdr)) == 0) {
			const char *v = line + strlen(keyhdr);

			while (*v == ' ' || *v == '\t')
				v++;
			end = strstr(v, "\r\n");
			keylen = end != NULL ? (size_t)(end - v) : strlen(v);
			if (keylen >= sizeof key)
				keylen = sizeof key - 1;
			memcpy(key, v, keylen);
			key[keylen] = '\0';
			break;
		}
		line = strstr(line, "\r\n");
		if (line != NULL)
			line += 2;
	}
	if (key[0] == '\0')
		return __dtfl_err(DTFL_EPROTO,
		    "websocket request missing Sec-WebSocket-Key");

	/* accept = base64(sha1(key + GUID)). */
	snprintf(concat, sizeof concat, "%s%s", key, WS_GUID);
	__dtfl_sha1(concat, strlen(concat), digest);
	__dtfl_base64_encode(digest, sizeof digest, accept);

	snprintf(resp, sizeof resp,
	    "HTTP/1.1 101 Switching Protocols\r\n"
	    "Upgrade: websocket\r\n"
	    "Connection: Upgrade\r\n"
	    "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	if (!send_all(fd, resp, strlen(resp)))
		return __dtfl_err(DTFL_EIO, "websocket handshake send: %s",
		    strerror(errno));
	return DTFL_OK;
}

int
__dtfl_ws_send_text(int fd, const char *data, size_t len)
{
	unsigned char hdr[10];
	size_t hlen = 0;

	hdr[0] = 0x81;	/* FIN + text opcode */
	if (len < 126) {
		hdr[1] = (unsigned char)len;
		hlen = 2;
	} else if (len <= 0xffff) {
		hdr[1] = 126;
		hdr[2] = (unsigned char)(len >> 8);
		hdr[3] = (unsigned char)(len);
		hlen = 4;
	} else {
		int i;

		hdr[1] = 127;
		for (i = 0; i < 8; i++)
			hdr[2 + i] = (unsigned char)((uint64_t)len >>
			    (56 - 8 * i));
		hlen = 10;
	}
	if (!send_all(fd, hdr, hlen) || !send_all(fd, data, len))
		return __dtfl_err(DTFL_EIO, "websocket send: %s",
		    strerror(errno));
	return DTFL_OK;
}

void
__dtfl_ws_send_close(int fd)
{
	unsigned char frame[2] = { 0x88, 0x00 };	/* FIN + close, 0 len */

	(void)send_all(fd, frame, sizeof frame);
}

/* ------------------------------------------------------------------ */
/* Frame JSON builder                                                  */
/* ------------------------------------------------------------------ */

const char *
__dtfl_diag_stat_kind_str(__dtfl_diag_stat_kind k)
{
	switch (k) {
	case DIAG_STAT_ELAPSED:
		return "Elapsed";
	case DIAG_STAT_MESSAGES:
		return "Messages";
	case DIAG_STAT_ARRANGEMENT_BATCHES:
		return "ArrangementBatches";
	case DIAG_STAT_ARRANGEMENT_RECORDS:
		return "ArrangementRecords";
	case DIAG_STAT_SHARING:
		return "Sharing";
	case DIAG_STAT_BATCHER_RECORDS:
		return "BatcherRecords";
	case DIAG_STAT_BATCHER_SIZE:
		return "BatcherSize";
	case DIAG_STAT_BATCHER_CAPACITY:
		return "BatcherCapacity";
	case DIAG_STAT_BATCHER_ALLOCATIONS:
		return "BatcherAllocations";
	default:
		return "Unknown";
	}
}

void
__dtfl_diag_json_escape(__dtfl_vec *buf, const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	for (; *p != '\0'; p++) {
		char tmp[8];

		switch (*p) {
		case '"':
			__dtfl_vec_push_n(buf, "\\\"", 2);
			break;
		case '\\':
			__dtfl_vec_push_n(buf, "\\\\", 2);
			break;
		case '\n':
			__dtfl_vec_push_n(buf, "\\n", 2);
			break;
		case '\r':
			__dtfl_vec_push_n(buf, "\\r", 2);
			break;
		case '\t':
			__dtfl_vec_push_n(buf, "\\t", 2);
			break;
		default:
			if (*p < 0x20) {
				snprintf(tmp, sizeof tmp, "\\u%04x", *p);
				__dtfl_vec_push_n(buf, tmp, strlen(tmp));
			} else {
				__dtfl_vec_push(buf, p);
			}
			break;
		}
	}
}

static void
append_str(__dtfl_vec *buf, const char *s)
{
	__dtfl_vec_push_n(buf, s, strlen(s));
}

static void
append_usizes(__dtfl_vec *buf, const size_t *v, size_t n)
{
	size_t i;
	char tmp[32];

	__dtfl_vec_push_n(buf, "[", 1);
	for (i = 0; i < n; i++) {
		snprintf(tmp, sizeof tmp, "%zu", v[i]);
		append_str(buf, tmp);
		if (i + 1 < n)
			__dtfl_vec_push_n(buf, ",", 1);
	}
	__dtfl_vec_push_n(buf, "]", 1);
}

void
__dtfl_diag_frame_begin(__dtfl_diag_frame *f)
{
	__dtfl_vec_init(&f->buf, 1);
	f->count = 0;
}

static void
frame_sep(__dtfl_diag_frame *f)
{
	if (f->count > 0)
		__dtfl_vec_push_n(&f->buf, ",", 1);
	f->count++;
}

void
__dtfl_diag_frame_add_operator(__dtfl_diag_frame *f, size_t id,
    const char *name, const size_t *addr, size_t addr_len, int64_t diff)
{
	char tmp[32];

	frame_sep(f);
	append_str(&f->buf, "{\"type\":\"Operator\",\"id\":");
	snprintf(tmp, sizeof tmp, "%zu", id);
	append_str(&f->buf, tmp);
	append_str(&f->buf, ",\"name\":\"");
	__dtfl_diag_json_escape(&f->buf, name != NULL ? name : "");
	append_str(&f->buf, "\",\"addr\":");
	append_usizes(&f->buf, addr, addr_len);
	append_str(&f->buf, ",\"diff\":");
	snprintf(tmp, sizeof tmp, "%lld", (long long)diff);
	append_str(&f->buf, tmp);
	append_str(&f->buf, "}");
}

void
__dtfl_diag_frame_add_channel(__dtfl_diag_frame *f, size_t id,
    const size_t *scope, size_t scope_len, size_t sn, size_t sp,
    size_t tn, size_t tp, int64_t diff)
{
	char tmp[32];

	frame_sep(f);
	append_str(&f->buf, "{\"type\":\"Channel\",\"id\":");
	snprintf(tmp, sizeof tmp, "%zu", id);
	append_str(&f->buf, tmp);
	append_str(&f->buf, ",\"scope_addr\":");
	append_usizes(&f->buf, scope, scope_len);
	append_str(&f->buf, ",\"source\":[");
	snprintf(tmp, sizeof tmp, "%zu", sn);
	append_str(&f->buf, tmp);
	__dtfl_vec_push_n(&f->buf, ",", 1);
	snprintf(tmp, sizeof tmp, "%zu", sp);
	append_str(&f->buf, tmp);
	append_str(&f->buf, "],\"target\":[");
	snprintf(tmp, sizeof tmp, "%zu", tn);
	append_str(&f->buf, tmp);
	__dtfl_vec_push_n(&f->buf, ",", 1);
	snprintf(tmp, sizeof tmp, "%zu", tp);
	append_str(&f->buf, tmp);
	append_str(&f->buf, "],\"diff\":");
	snprintf(tmp, sizeof tmp, "%lld", (long long)diff);
	append_str(&f->buf, tmp);
	append_str(&f->buf, "}");
}

void
__dtfl_diag_frame_add_stat(__dtfl_diag_frame *f, __dtfl_diag_stat_kind kind,
    size_t id, int64_t diff)
{
	char tmp[32];

	frame_sep(f);
	append_str(&f->buf, "{\"type\":\"Stat\",\"kind\":\"");
	append_str(&f->buf, __dtfl_diag_stat_kind_str(kind));
	append_str(&f->buf, "\",\"id\":");
	snprintf(tmp, sizeof tmp, "%zu", id);
	append_str(&f->buf, tmp);
	append_str(&f->buf, ",\"diff\":");
	snprintf(tmp, sizeof tmp, "%lld", (long long)diff);
	append_str(&f->buf, tmp);
	append_str(&f->buf, "}");
}

char *
__dtfl_diag_frame_finish(__dtfl_diag_frame *f, uint64_t ts_us)
{
	__dtfl_vec out;
	char head[64];
	char nul = '\0';
	char *s;

	if (f->count == 0) {
		__dtfl_vec_free(&f->buf);
		return NULL;
	}
	__dtfl_vec_init(&out, 1);
	snprintf(head, sizeof head, "{\"type\":\"Frame\",\"ts_us\":%llu,"
	    "\"updates\":[", (unsigned long long)ts_us);
	__dtfl_vec_push_n(&out, head, strlen(head));
	__dtfl_vec_push_n(&out, f->buf.data, f->buf.len);
	__dtfl_vec_push_n(&out, "]}", 2);
	__dtfl_vec_push(&out, &nul);
	s = (char *)out.data;	/* ownership transfers to caller */
	__dtfl_vec_free(&f->buf);
	return s;
}

void
__dtfl_diag_frame_free(__dtfl_diag_frame *f)
{
	__dtfl_vec_free(&f->buf);
	f->count = 0;
}
