/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/embed/forked_workers.c: workers in separate processes.
 *
 * The parent makes a socketpair(2) between every pair of workers, then
 * forks one child per worker.  Each child keeps its own ends, wraps them
 * in the built-in transport (dtfl_transport_socket) and joins the
 * execution as worker i of n with dtfl_execute_worker, which starts no
 * thread.  This is the shape a host uses when it creates the processes
 * itself: a PostgreSQL extension does the same with background workers
 * and shm_mq in place of fork and sockets (dtfl_embedding(7)).
 *
 * The dataflow is a word count in miniature: every worker contributes
 * the numbers 0 .. 9999, an exchange by value sends each to worker
 * (value % n), and each worker reports how many it received and their
 * sum through a pipe.  The parent prints the totals, which do not depend
 * on n, and exits non-zero if any child failed or was misrouted.
 *
 * Usage: forked_workers [workers]   (default 4, at most 16)
 * dtfl_config_set_workers(cfg, DTFL_WORKERS_PROCESSES) makes
 * dtfl_execute do all of this itself; this example spells it out.
 */
#define _POSIX_C_SOURCE 200809L

#include "dtfl/config.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define RECORDS 10000
#define MAXW 16

typedef struct state {
	dtfl_input *input;
	dtfl_probe *probe;
	uint64_t count, sum, misrouted;
	size_t index, peers;
} state;

static uint64_t
by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
tally(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	state *s = ctx;
	uint64_t v = dtfl_get_u64(rec);

	(void)time; (void)len;
	if (v % s->peers != s->index)
		s->misrouted++;
	s->count++;
	s->sum += v;
}

static void
build(dtfl_scope *scope, void *arg)
{
	state *s = arg;
	dtfl_stream *st = dtfl_input_to_stream(s->input, scope);

	st = dtfl_stream_exchange(st, by_value, NULL);
	st = dtfl_stream_inspect(st, tally, DTFL_CTX(s, NULL));
	dtfl_stream_probe_with(st, s->probe);
}

static void
worker(dtfl_worker *w, void *arg)
{
	state *s = arg;
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t i, one = 1;

	s->index = dtfl_worker_index(w);
	s->peers = dtfl_worker_peers(w);
	s->input = dtfl_input_new(dtfl_type_u64());
	s->probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "forked", tc, build, s);
	for (i = 0; i < RECORDS; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, i);
		dtfl_input_send(s->input, buf, sizeof buf);
	}
	dtfl_input_advance_to(s->input, &one);
	/* Park in the transport while the other processes catch up. */
	while (dtfl_probe_less_than(s->probe, dtfl_input_time(s->input)))
		(void)dtfl_worker_step_or_park(w, UINT64_MAX);
	dtfl_input_close(s->input);
	dtfl_probe_free(s->probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	size_t n = argc > 1 ? (size_t)strtoul(argv[1], NULL, 10) : 4, i, j;
	int mesh[MAXW][MAXW], rd[MAXW];
	pid_t pids[MAXW];
	uint64_t count = 0, sum = 0, misrouted = 0;
	int failed = 0;

	if (n == 0 || n > MAXW) {
		fprintf(stderr, "usage: %s [workers 1..%d]\n", argv[0], MAXW);
		return 2;
	}
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			mesh[i][j] = -1;
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++) {
			int sv[2];

			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
				perror("socketpair");
				return 1;
			}
			mesh[i][j] = sv[0];
			mesh[j][i] = sv[1];
		}
	for (i = 0; i < n; i++) {
		int p[2];

		if (pipe(p) != 0 || (pids[i] = fork()) < 0) {
			perror("fork");
			return 1;
		}
		if (pids[i] == 0) {
			dtfl_transport t;
			state s;
			int rc;

			close(p[0]);
			for (j = 0; j < n; j++) {
				size_t k;

				for (k = 0; k < n; k++)
					if (j != i && mesh[j][k] >= 0)
						close(mesh[j][k]);
			}
			memset(&s, 0, sizeof s);
			rc = dtfl_transport_socket(mesh[i], n, i, &t);
			if (rc == 0) {
				rc = dtfl_execute_worker(NULL, i, n, &t, worker,
				    &s);
				t.destroy(t.ctx);
			}
			if (rc != 0)
				fprintf(stderr, "worker %zu: %s: %s\n", i,
				    dtfl_strerror(rc), dtfl_last_error_detail());
			if (write(p[1], &s, sizeof s) != (ssize_t)sizeof s)
				_exit(1);
			_exit(rc != 0);
		}
		close(p[1]);
		rd[i] = p[0];
	}
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			if (mesh[i][j] >= 0)
				close(mesh[i][j]);
	for (i = 0; i < n; i++) {
		state s;
		int status;

		memset(&s, 0, sizeof s);
		if (read(rd[i], &s, sizeof s) != (ssize_t)sizeof s)
			failed = 1;
		close(rd[i]);
		if (waitpid(pids[i], &status, 0) != pids[i] ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			failed = 1;
		count += s.count;
		sum += s.sum;
		misrouted += s.misrouted;
	}
	/* Divide by n so the printed line is the same for any n. */
	printf("records per worker %" PRIu64 ", sum per worker %" PRIu64
	    ", misrouted %" PRIu64 "\n", count / n, sum / n, misrouted);
	return failed || misrouted != 0 ||
	    count != (uint64_t)n * RECORDS ? 1 : 0;
}
