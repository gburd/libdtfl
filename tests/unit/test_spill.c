/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/spill.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * tests/unit/test_spill.c: the spill.rs unit tests, with its in-memory
 * MockStrategy and MockHandle, and the file-backed spill hook
 * (dtfl_config_set_spill_file) end to end on the ProcessBinary and
 * Cluster allocators.
 */
#define _POSIX_C_SOURCE 200809L	/* opendir in count_fds, nanosleep */

#include "dtfl_test.h"
#include "comm/zc_int.h"
#include "comm/init_int.h"
#include "base/os_thread.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* dtfl_test.h's panic hook is unused here: no spill test expects a panic. */
static void (*const unused_hook)(const char *, void *) DTFL_UNUSED =
    dtfl_test_panic_hook;

/* bytes_of: a read-only view of a fresh copy of data. */
static void
bytes_of(__dtfl_bytes *out, const void *data, size_t n)
{
	__dtfl_bytes tmp;

	__dtfl_bytes_new(&tmp, n);
	memcpy(tmp.ptr, data, n);
	__dtfl_bytes_extract_to(&tmp, n, out);
	__dtfl_bytes_release(&tmp);
}

typedef struct mock_handle {
	__dtfl_bytes_fetch base;
	__dtfl_bytes data;
} mock_handle;

static bool
mock_fetch(__dtfl_bytes_fetch *h, __dtfl_vec *out)
{
	mock_handle *m = (mock_handle *)h;

	__dtfl_vec_push(out, &m->data);
	__dtfl_free(m);
	return true;
}

static void
mock_handle_destroy(__dtfl_bytes_fetch *h)
{
	mock_handle *m = (mock_handle *)h;

	__dtfl_bytes_release(&m->data);
	__dtfl_free(m);
}

static const __dtfl_bytes_fetch_ops mock_handle_ops = {
	mock_fetch, mock_handle_destroy
};

/* MockStrategy::spill: every chunk becomes a MockHandle. */
static void
mock_spill(__dtfl_bytes_spill *s, __dtfl_vec *chunks, __dtfl_vec *handles)
{
	size_t i;

	(void)s;
	for (i = 0; i < chunks->len; i++) {
		mock_handle *m = __dtfl_alloc(sizeof *m);

		m->base.ops = &mock_handle_ops;
		m->data = *DTFL_VEC_AT(chunks, __dtfl_bytes, i);
		__dtfl_vec_push(handles, &m);
	}
	__dtfl_vec_clear(chunks);
}

static void
mock_strategy_destroy(__dtfl_bytes_spill *s)
{
	__dtfl_free(s);
}

static const __dtfl_bytes_spill_ops mock_strategy_ops = {
	mock_spill, mock_strategy_destroy
};

static __dtfl_bytes_spill *
mock_strategy(void)
{
	__dtfl_bytes_spill *s = __dtfl_alloc(sizeof *s);

	s->ops = &mock_strategy_ops;
	return s;
}

/* EagerPolicy: spills every resident entry but the last. */
typedef struct eager_policy {
	__dtfl_spill_policy base;
	__dtfl_bytes_spill *strategy;
} eager_policy;

static void
eager_apply(__dtfl_spill_policy *pp, __dtfl_queue_entries *queue)
{
	eager_policy *p = (eager_policy *)pp;
	size_t last = queue->len > 0 ? queue->len - 1 : 0, i, n;
	__dtfl_vec indices, bytes, handles;

	__dtfl_vec_init(&indices, sizeof(size_t));
	__dtfl_vec_init(&bytes, sizeof(__dtfl_bytes));
	__dtfl_vec_init(&handles, sizeof(__dtfl_bytes_fetch *));
	for (i = 0; i < queue->len; i++) {
		__dtfl_queue_entry *e = __dtfl_qe_at(queue, i);

		if (i == last)
			break;
		if (e->kind == DTFL_QE_BYTES) {
			__dtfl_vec_push(&indices, &i);
			__dtfl_bytes_clone(&e->u.bytes,
			    __dtfl_vec_push(&bytes, NULL));
		}
	}
	if (bytes.len > 0) {
		p->strategy->ops->spill(p->strategy, &bytes, &handles);
		n = DTFL_MIN(indices.len, handles.len);
		for (i = 0; i < n; i++) {
			__dtfl_queue_entry *e = __dtfl_qe_at(queue,
			    *DTFL_VEC_AT(&indices, size_t, i));

			__dtfl_qe_entry_drop(e);
			e->kind = DTFL_QE_PAGED;
			e->u.paged = *DTFL_VEC_AT(&handles,
			    __dtfl_bytes_fetch *, i);
		}
	}
	__dtfl_vec_free(&indices);
	__dtfl_vec_free(&bytes);
	__dtfl_vec_free(&handles);
}

static void
eager_destroy(__dtfl_spill_policy *pp)
{
	eager_policy *p = (eager_policy *)pp;

	p->strategy->ops->destroy(p->strategy);
}

static const __dtfl_spill_policy_ops eager_ops = { eager_apply, eager_destroy };

DTFL_TEST(allocator_zero_copy_spill__eager_policy_moves_middle_entries)
{
	eager_policy p = { { &eager_ops }, mock_strategy() };
	__dtfl_queue_entries queue;
	unsigned char buf[8];
	__dtfl_queue_entry e;
	int i;

	(void)params; (void)data;
	__dtfl_qe_init(&queue);
	for (i = 0; i < 4; i++) {
		memset(buf, i, sizeof buf);
		e.kind = DTFL_QE_BYTES;
		bytes_of(&e.u.bytes, buf, sizeof buf);
		__dtfl_qe_push_back(&queue, e);
	}
	p.base.ops->apply(&p.base, &queue);
	assert_int(__dtfl_qe_at(&queue, 0)->kind, ==, DTFL_QE_PAGED);
	assert_int(__dtfl_qe_at(&queue, 1)->kind, ==, DTFL_QE_PAGED);
	assert_int(__dtfl_qe_at(&queue, 2)->kind, ==, DTFL_QE_PAGED);
	assert_int(__dtfl_qe_at(&queue, 3)->kind, ==, DTFL_QE_BYTES);
	__dtfl_qe_free(&queue);
	p.base.ops->destroy(&p.base);
	return MUNIT_OK;
}

static void
noop_buzz(void *ctx)
{
	(void)ctx;
}

DTFL_TEST(allocator_zero_copy_spill__merge_queue_spill_roundtrip_mock)
{
	const size_t head_reserve = 128;
	__dtfl_spill_threshold *tp =
	    __dtfl_spill_threshold_new(mock_strategy());
	__dtfl_spill_prefetch *rp = __dtfl_spill_prefetch_new(head_reserve);
	__dtfl_merge_queue *writer, *reader;
	unsigned char expected[100 * 64], got[100 * 64], d[64];
	__dtfl_vec received;
	size_t before, off = 0, i, paged = 0;
	__dtfl_bytes b;

	(void)params; (void)data;
	tp->threshold_bytes = 512;
	tp->head_reserve_bytes = head_reserve;
	__dtfl_merge_queue_new_pair(noop_buzz, NULL, NULL, &tp->base,
	    &rp->base, &writer, &reader);

	for (i = 0; i < 100; i++) {
		memset(d, (int)(i % 251), sizeof d);
		memcpy(expected + 64 * i, d, sizeof d);
		bytes_of(&b, d, sizeof d);
		__dtfl_merge_queue_extend(writer, &b, 1);
	}

	__dtfl_vec_init(&received, sizeof(__dtfl_bytes));
	for (;;) {
		before = received.len;
		__dtfl_merge_queue_drain_into(reader, &received);
		if (received.len == before)
			break;
		paged++;
	}

	for (i = 0; i < received.len; i++) {
		__dtfl_bytes *r = DTFL_VEC_AT(&received, __dtfl_bytes, i);

		assert_size(off + r->len, <=, sizeof got);
		memcpy(got + off, r->ptr, r->len);
		off += r->len;
		__dtfl_bytes_release(r);
	}
	assert_size(off, ==, sizeof expected);
	assert_memory_equal(sizeof expected, expected, got);
	/* The reader needed several rounds: the writer really spilled. */
	assert_size(paged, >, 1);
	__dtfl_vec_free(&received);
	__dtfl_merge_queue_drop(writer, false);
	__dtfl_merge_queue_drop(reader, false);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* dtfl_config_set_spill_file end to end                               */
/* ------------------------------------------------------------------ */

#define CHUNK (16u << 10)		/* bytes per record payload */
#define NCHUNKS 256			/* per worker: 4 MiB */
#define THRESHOLD (64u << 10)
#define RESERVE (64u << 10)

static const char *
spill_dir(void)
{
	const char *d = getenv("TMPDIR");

	return d != NULL && d[0] != '\0' ? d : "/tmp";
}

/* Open descriptors of this process, or -1 without /proc. */
static int
count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;
	(void)closedir(d);
	return n - 1;	/* the DIR's own descriptor */
}

/* xorshift64: worker `w`'s chunk payloads, reproducible by the checker. */
static uint64_t
rng_next(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static uint64_t
rng_seed(size_t w)
{
	return UINT64_C(0x9E3779B97F4A7C15) ^
	    ((uint64_t)w * UINT64_C(0xBF58476D1CE4E5B9));
}

/* Order-insensitive digest: the sum and xor of each record's FNV-1a. */
typedef struct digest {
	uint64_t sum;
	uint64_t xr;
	size_t recs;
	size_t bytes;
} digest;

static void
digest_add(digest *d, const void *p, size_t n)
{
	const unsigned char *b = p;
	uint64_t h = UINT64_C(0xcbf29ce484222325);
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ b[i]) * UINT64_C(0x100000001b3);
	d->sum += h;
	d->xr ^= h;
	d->recs++;
	d->bytes += n;
}

/* What worker w sends: NCHUNKS chunks of CHUNK bytes. */
static void
expected_digest(size_t w, digest *d)
{
	unsigned char *buf = malloc(CHUNK);
	uint64_t st = rng_seed(w);
	size_t c, i;

	if (buf == NULL)
		abort();
	memset(d, 0, sizeof *d);
	for (c = 0; c < NCHUNKS; c++) {
		for (i = 0; i < CHUNK; i += 8) {
			uint64_t v = rng_next(&st);

			memcpy(buf + i, &v, 8);
		}
		digest_add(d, buf, CHUNK);
	}
	free(buf);
}

typedef struct spill_run {
	size_t peers;
	atomic_size_t produced;	/* workers done producing, run-wide */
	digest got[4];		/* per global worker; its thread only */
	/*
	 * Cluster only: worker w's process factory.  The worker waits for
	 * it to report a spill before draining, because its recv queue is
	 * filled by the recv thread concurrently, not by the worker.
	 */
	const dtfl_config *wait_spill[4];
} spill_run;

typedef struct spill_worker {
	spill_run *run;
	size_t index;
	uint64_t target;
} spill_worker;

static uint64_t
route_target(const void *rec, size_t len, void *ctx)
{
	(void)rec; (void)len;
	return ((spill_worker *)ctx)->target;
}

static void
inspect_chunk(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	spill_worker *sw = ctx;
	size_t n;
	const void *p = dtfl_get_bytes(rec, &n);

	(void)time; (void)len;
	digest_add(&sw->run->got[sw->index], p, n);
}

typedef struct spill_build {
	dtfl_input *input;
	spill_worker *sw;
} spill_build;

static void
spill_dataflow(dtfl_scope *scope, void *arg)
{
	spill_build *b = arg;
	dtfl_stream *s = dtfl_input_to_stream(b->input, scope);

	s = dtfl_stream_exchange(s, route_target, b->sw);
	(void)dtfl_stream_inspect(s, inspect_chunk, DTFL_CTX(b->sw, NULL));
}

/*
 * Sends NCHUNKS chunks to the next worker, then waits until every worker
 * has finished producing before returning to step.  Nobody drains until
 * all data is queued, so each queue holds 4 MiB, far past the 128 KiB
 * the policy allows resident.
 */
static void
spill_worker_fn(dtfl_worker *w, void *ctx)
{
	spill_run *run = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	unsigned char *rec = malloc(8 + CHUNK);
	spill_worker *sw = calloc(1, sizeof *sw);
	spill_build b;
	uint64_t st;
	size_t c, i;

	if (rec == NULL || sw == NULL)
		abort();
	sw->run = run;
	sw->index = dtfl_worker_index(w);
	sw->target = (sw->index + 1) % dtfl_worker_peers(w);
	b.input = dtfl_input_new(dtfl_type_bytes());
	b.sw = sw;
	(void)dtfl_worker_dataflow(w, "spill", tc, spill_dataflow, &b);
	st = rng_seed(sw->index);
	for (c = 0; c < NCHUNKS; c++) {
		unsigned char *p = rec + 8;

		for (i = 0; i < CHUNK; i += 8) {
			uint64_t v = rng_next(&st);

			memcpy(p + i, &v, 8);
		}
		dtfl_put_u64(rec, CHUNK);
		dtfl_input_send(b.input, rec, 8 + CHUNK);
	}
	dtfl_input_close(b.input);
	atomic_fetch_add(&run->produced, 1);
	while (atomic_load(&run->produced) < run->peers)
		sched_yield();
	if (run->wait_spill[sw->index] != NULL) {
		uint64_t t0 = __dtfl_clock_ns();
		__dtfl_spill_file_stats ss;

		do {
			__dtfl_spill_file_fn_stats(__dtfl_config_spill(
			    run->wait_spill[sw->index]), &ss);
			if (ss.files > 0)
				break;
			struct timespec nap = { 0, 1000000L };

			(void)nanosleep(&nap, NULL);
		} while (__dtfl_clock_ns() - t0 < UINT64_C(20000000000));
	}
	while (dtfl_worker_step_or_park(w, UINT64_MAX))
		;
	free(rec);
	free(sw);
	dtfl_time_class_free(tc);
}

/* Every worker received exactly what its predecessor sent. */
static MunitResult
check_digests(spill_run *run)
{
	size_t w;

	for (w = 0; w < run->peers; w++) {
		digest want;

		expected_digest((w + run->peers - 1) % run->peers, &want);
		assert_size(run->got[w].recs, ==, want.recs);
		assert_size(run->got[w].bytes, ==, want.bytes);
		assert_uint64(run->got[w].sum, ==, want.sum);
		assert_uint64(run->got[w].xr, ==, want.xr);
	}
	return MUNIT_OK;
}

/* Everything spilled came back, with no write failures. */
static void
check_spilled(const dtfl_config *cfg, const char *what, uint64_t min_bytes)
{
	__dtfl_spill_file_stats st;

	__dtfl_spill_file_fn_stats(__dtfl_config_spill(cfg), &st);
	munit_logf(MUNIT_LOG_INFO, "%s: %llu files, %llu chunks (%llu bytes) "
	    "spilled, %llu fetched", what, (unsigned long long)st.files,
	    (unsigned long long)st.spilled_chunks,
	    (unsigned long long)st.spilled_bytes,
	    (unsigned long long)st.fetched_chunks);
	assert_uint64(st.failed_spills, ==, 0);
	assert_uint64(st.files, >, 0);
	assert_uint64(st.spilled_bytes, >=, min_bytes);
	assert_uint64(st.fetched_chunks, ==, st.spilled_chunks);
}

DTFL_TEST(spill_file__config_errors)
{
	dtfl_config *cfg = dtfl_config_new();

	(void)params; (void)data;
	assert_int(dtfl_config_set_spill_file(cfg, "/nonexistent/dtfl-spill",
	    THRESHOLD, RESERVE), ==, DTFL_EIO);
	assert_null(__dtfl_config_spill(cfg));
	assert_int(dtfl_config_set_spill_file(cfg, spill_dir(), THRESHOLD, 0),
	    ==, DTFL_EINVAL);
	assert_int(dtfl_config_set_spill_file(cfg, "", THRESHOLD, RESERVE),
	    ==, DTFL_EINVAL);
	assert_int(dtfl_config_set_spill_file(cfg, spill_dir(), THRESHOLD,
	    RESERVE), ==, 0);
	assert_not_null(__dtfl_config_spill(cfg));
	/* Replacing and removing release the old factory (ASan checks). */
	assert_int(dtfl_config_set_spill_file(cfg, spill_dir(), 1, 1), ==, 0);
	assert_int(dtfl_config_set_spill_file(cfg, NULL, 0, 0), ==, 0);
	assert_null(__dtfl_config_spill(cfg));
	assert_int(dtfl_config_set_spill_file(cfg, spill_dir(), THRESHOLD,
	    RESERVE), ==, 0);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

/*
 * Two ProcessBinary workers each push 4 MiB to the other through an
 * Exchange with a 64 KiB threshold: the bytes arrive intact, the
 * strategy spilled at least a megabyte and paged all of it back, and no
 * spill file descriptor is left open.
 */
DTFL_TEST(spill_file__process_binary_exchange)
{
	dtfl_config *cfg = dtfl_config_new();
	spill_run *run = calloc(1, sizeof *run);
	int fds_before = count_fds(), fds_after;

	(void)params; (void)data;
	if (run == NULL)
		abort();
	run->peers = 2;
	atomic_init(&run->produced, 0);
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS_BINARY), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 2), ==, 0);
	assert_int(dtfl_config_set_spill_file(cfg, spill_dir(), THRESHOLD,
	    RESERVE), ==, 0);
	assert_int(dtfl_execute(cfg, spill_worker_fn, run, NULL), ==, 0);
	/*
	 * The producer is the queue's writer and nobody drains until both
	 * finish, so each 4 MiB queue (slab-sized, about 1 MiB entries)
	 * spills everything but its head and tail entries.
	 */
	check_spilled(cfg, "process-binary", (uint64_t)2 << 20);
	fds_after = count_fds();
	if (fds_before >= 0)
		assert_int(fds_after, ==, fds_before);
	dtfl_config_free(cfg);
	(void)check_digests(run);
	free(run);
	return MUNIT_OK;
}

/* The same exchange without the hook: nothing to spill, same bytes. */
DTFL_TEST(spill_file__process_binary_no_hook)
{
	dtfl_config *cfg = dtfl_config_new();
	spill_run *run = calloc(1, sizeof *run);

	(void)params; (void)data;
	if (run == NULL)
		abort();
	run->peers = 2;
	atomic_init(&run->produced, 0);
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS_BINARY), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 2), ==, 0);
	assert_int(dtfl_execute(cfg, spill_worker_fn, run, NULL), ==, 0);
	dtfl_config_free(cfg);
	(void)check_digests(run);
	free(run);
	return MUNIT_OK;
}

/* Binds a loopback socket to an ephemeral port and returns the port. */
static int
pick_port(void)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0), port;

	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	/* 127.0.0.1: POSIX.1-2008 has no INADDR_LOOPBACK (FreeBSD hides it). */
	sa.sin_addr.s_addr = htonl(0x7f000001);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
		(void)close(fd);
		return -1;
	}
	port = ntohs(sa.sin_port);
	(void)close(fd);
	return port;
}

typedef struct cluster_proc {
	dtfl_config *cfg;
	spill_run *run;
	int rc;
} cluster_proc;

static void *
cluster_proc_main(void *v)
{
	cluster_proc *p = v;

	p->rc = dtfl_execute(p->cfg, spill_worker_fn, p->run, NULL);
	return NULL;
}

/*
 * A two-process loopback cluster, one worker each, every byte crossing
 * TCP.  The worker-side send queues and the recv-thread side queues
 * both carry policies; the recv queues fill while the receiving worker
 * is still producing, so each process spills.
 */
DTFL_TEST(spill_file__cluster_exchange)
{
	char addrbuf[2][32];
	const char *addrs[2];
	cluster_proc procs[2];
	__dtfl_thread t[2];
	spill_run *run = calloc(1, sizeof *run);
	size_t i;

	(void)params; (void)data;
	if (run == NULL)
		abort();
	for (i = 0; i < 2; i++) {
		int port = pick_port();

		if (port < 0) {
			free(run);
			return MUNIT_SKIP;
		}
		snprintf(addrbuf[i], sizeof addrbuf[i], "127.0.0.1:%d", port);
		addrs[i] = addrbuf[i];
	}
	if (strcmp(addrs[0], addrs[1]) == 0) {
		free(run);
		return MUNIT_SKIP;
	}
	run->peers = 2;
	atomic_init(&run->produced, 0);
	for (i = 0; i < 2; i++) {
		procs[i].cfg = dtfl_config_new();
		procs[i].run = run;
		procs[i].rc = -1;
		assert_int(dtfl_config_set_mode(procs[i].cfg,
		    DTFL_COMM_CLUSTER), ==, 0);
		assert_int(dtfl_config_set_threads(procs[i].cfg, 1), ==, 0);
		assert_int(dtfl_config_set_cluster(procs[i].cfg, i, addrs, 2),
		    ==, 0);
		assert_int(dtfl_config_set_spill_file(procs[i].cfg,
		    spill_dir(), THRESHOLD, RESERVE), ==, 0);
		run->wait_spill[i] = procs[i].cfg;
	}
	for (i = 0; i < 2; i++)
		assert_int(__dtfl_thread_spawn("proc", cluster_proc_main,
		    &procs[i], &t[i]), ==, 0);
	for (i = 0; i < 2; i++)
		(void)__dtfl_thread_join(t[i]);
	for (i = 0; i < 2; i++) {
		assert_int(procs[i].rc, ==, 0);
		check_spilled(procs[i].cfg, i == 0 ? "cluster p0" :
		    "cluster p1", 1);
		dtfl_config_free(procs[i].cfg);
	}
	(void)check_digests(run);
	free(run);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(allocator_zero_copy_spill__eager_policy_moves_middle_entries),
	DTFL_T(allocator_zero_copy_spill__merge_queue_spill_roundtrip_mock),
	DTFL_T(spill_file__config_errors),
	DTFL_T(spill_file__process_binary_exchange),
	DTFL_T(spill_file__process_binary_no_hook),
	DTFL_T(spill_file__cluster_exchange),
	DTFL_T_END
};

DTFL_TEST_MAIN("spill", tests)
