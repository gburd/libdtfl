/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/config.c: dtfl_config, and the command-line parser that
 * replaces getopts.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/initialize.rs (Config, install_options, from_matches)
 *   timely/src/worker.rs (ProgressMode, Config::install_options)
 *   timely/src/execute.rs (Config::from_args)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The parser follows getopts 0.2 as timely configures it (every option
 * Optional, FloatingFrees): "-w4", "-w 4", "--threads=4", "--threads 4",
 * clustered flags ("-rz", "-zw4"), "--" ends option parsing and is
 * dropped, and an option given twice is an error.  One deliberate
 * difference, documented in dtfl_config(3): an option timely does not
 * know is kept for the caller with the free arguments, where getopts
 * fails with UnrecognizedOption, because a C program parses its own
 * flags from what is left.
 */
#include "comm/init_int.h"
#include "comm/zc_int.h"

#include <errno.h>
#include <stdio.h>

struct dtfl_config {
	dtfl_comm_mode mode;
	size_t threads;
	size_t process;
	char **addresses;	/* naddresses strings, owned */
	size_t naddresses;
	bool report;
	bool zerocopy;		/* Cluster { zerocopy } (-z with -n > 1) */
	dtfl_progress_mode progress_mode;
	uint64_t seed;
	char *timely_log_addr;		/* owned or NULL */
	char *differential_log_addr;	/* owned or NULL */
	__dtfl_vec log_subs;		/* __dtfl_log_sub, streams owned */
	__dtfl_spill_fn *spill;		/* one reference, or NULL */
	int64_t idle_merge_effort;	/* differential Config; 0 is None */
};

dtfl_config *
dtfl_config_new(void)
{
	dtfl_config *cfg = __dtfl_calloc(1, sizeof *cfg);

	cfg->mode = DTFL_COMM_THREAD;
	cfg->threads = 1;
	cfg->progress_mode = DTFL_PROGRESS_DEMAND;
	__dtfl_vec_init(&cfg->log_subs, sizeof(__dtfl_log_sub));
	return cfg;
}

static void
free_addresses(char **addresses, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		__dtfl_free(addresses[i]);
	__dtfl_free(addresses);
}

void
dtfl_config_free(dtfl_config *cfg)
{
	size_t i;

	if (cfg == NULL)
		return;
	free_addresses(cfg->addresses, cfg->naddresses);
	__dtfl_free(cfg->timely_log_addr);
	__dtfl_free(cfg->differential_log_addr);
	for (i = 0; i < cfg->log_subs.len; i++) {
		__dtfl_log_sub *s = DTFL_VEC_AT(&cfg->log_subs, __dtfl_log_sub, i);

		if (s->ctx_free != NULL)
			s->ctx_free(s->ctx);
		__dtfl_free((void *)(uintptr_t)s->stream);
	}
	__dtfl_vec_free(&cfg->log_subs);
	__dtfl_spill_fn_release(cfg->spill);
	__dtfl_free(cfg);
}

int
dtfl_config_set_mode(dtfl_config *cfg, dtfl_comm_mode mode)
{
	switch (mode) {
	case DTFL_COMM_THREAD:
	case DTFL_COMM_PROCESS:
	case DTFL_COMM_PROCESS_BINARY:
	case DTFL_COMM_CLUSTER:
	case DTFL_COMM_SIM:
		cfg->mode = mode;
		return 0;
	}
	return __dtfl_err(DTFL_EINVAL, "unknown communication mode %d",
	    (int)mode);
}

int
dtfl_config_set_threads(dtfl_config *cfg, size_t threads)
{
	if (threads == 0)
		return __dtfl_err(DTFL_EINVAL, "threads must be at least 1");
	cfg->threads = threads;
	return 0;
}

int
dtfl_config_set_cluster(dtfl_config *cfg, size_t process,
    const char *const *addresses, size_t naddresses)
{
	char **copy;
	size_t i;

	if (addresses == NULL || naddresses == 0)
		return __dtfl_err(DTFL_EINVAL, "a cluster needs addresses");
	if (process >= naddresses)
		return __dtfl_err(DTFL_EINVAL,
		    "process %zu out of range for %zu addresses", process,
		    naddresses);
	for (i = 0; i < naddresses; i++)
		if (addresses[i] == NULL)
			return __dtfl_err(DTFL_EINVAL, "address %zu is NULL", i);
	copy = __dtfl_calloc(naddresses, sizeof *copy);
	for (i = 0; i < naddresses; i++)
		copy[i] = __dtfl_strdup(addresses[i]);
	free_addresses(cfg->addresses, cfg->naddresses);
	cfg->addresses = copy;
	cfg->naddresses = naddresses;
	cfg->process = process;
	return 0;
}

void
dtfl_config_set_report(dtfl_config *cfg, bool report)
{
	cfg->report = report;
}

void
dtfl_config_set_progress_mode(dtfl_config *cfg, dtfl_progress_mode mode)
{
	cfg->progress_mode = mode;
}

void
dtfl_config_set_seed(dtfl_config *cfg, uint64_t seed)
{
	cfg->seed = seed;
}

dtfl_comm_mode
dtfl_config_mode(const dtfl_config *cfg)
{
	return cfg->mode;
}

size_t
dtfl_config_threads(const dtfl_config *cfg)
{
	return cfg->threads;
}

size_t
dtfl_config_process(const dtfl_config *cfg)
{
	return cfg->process;
}

size_t
dtfl_config_processes(const dtfl_config *cfg)
{
	return cfg->naddresses > 0 ? cfg->naddresses : 1;
}

const char *
dtfl_config_address(const dtfl_config *cfg, size_t i)
{
	return i < cfg->naddresses ? cfg->addresses[i] : NULL;
}

bool
dtfl_config_report(const dtfl_config *cfg)
{
	return cfg->report;
}

dtfl_progress_mode
dtfl_config_progress_mode(const dtfl_config *cfg)
{
	return cfg->progress_mode;
}

uint64_t
dtfl_config_seed(const dtfl_config *cfg)
{
	return cfg->seed;
}

/*
 * differential Config::idle_merge_effort (lib.rs).  Upstream's
 * Option<isize> is a positive effort or 0 for None; a negative effort,
 * which upstream would cast to a huge usize, is refused.
 */
int
dtfl_config_set_idle_merge_effort(dtfl_config *cfg, int64_t effort)
{
	if (cfg == NULL || effort < 0)
		return __dtfl_err(DTFL_EINVAL, "dtfl_config_set_idle_merge_effort: "
		    "NULL config or negative effort");
	cfg->idle_merge_effort = effort;
	return 0;
}

int64_t
dtfl_config_idle_merge_effort(const dtfl_config *cfg)
{
	return cfg->idle_merge_effort;
}

/*
 * Hooks::spill with the example's file-backed factory.  Upstream keeps
 * hooks beside the configuration; here they ride on it, and
 * dtfl_config_from_args leaves them alone.
 */
int
dtfl_config_set_spill_file(dtfl_config *cfg, const char *dir,
    size_t threshold_bytes, size_t head_reserve_bytes)
{
	__dtfl_spill_fn *f = NULL;
	int rc;

	if (cfg == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_config_set_spill_file: "
		    "NULL config");
	if (dir != NULL && (rc = __dtfl_spill_file_fn_new(dir,
	    threshold_bytes, head_reserve_bytes, &f)) != 0)
		return rc;
	__dtfl_spill_fn_release(cfg->spill);
	cfg->spill = f;
	return 0;
}

__dtfl_spill_fn *
__dtfl_config_spill(const dtfl_config *cfg)
{
	return cfg->spill;
}

bool
__dtfl_config_zerocopy(const dtfl_config *cfg)
{
	return cfg->zerocopy;
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

enum { OPT_W, OPT_P, OPT_N, OPT_H, OPT_R, OPT_Z, OPT_PROGRESS, NOPTS };

/* Config::install_options (communication) and worker::Config's. */
static const struct {
	char shortname;		/* '\0': long only */
	const char *longname;
	bool hasarg;
} options[NOPTS] = {
	[OPT_W] = { 'w', "threads", true },
	[OPT_P] = { 'p', "process", true },
	[OPT_N] = { 'n', "processes", true },
	[OPT_H] = { 'h', "hostfile", true },
	[OPT_R] = { 'r', "report", false },
	[OPT_Z] = { 'z', "zerocopy", false },
	[OPT_PROGRESS] = { '\0', "progress-mode", true },
};

typedef struct matches {
	bool present[NOPTS];
	const char *val[NOPTS];	/* argument, borrowed from argv */
} matches;

static int
find_short(char c)
{
	int i;

	for (i = 0; i < NOPTS; i++)
		if (options[i].shortname != '\0' && options[i].shortname == c)
			return i;
	return -1;
}

static int
find_long(const char *name, size_t len)
{
	int i;

	for (i = 0; i < NOPTS; i++)
		if (strlen(options[i].longname) == len &&
		    memcmp(options[i].longname, name, len) == 0)
			return i;
	return -1;
}

/* Records an occurrence; getopts' OptionDuplicated for Optional options. */
static int
record(matches *m, int opt, const char *val)
{
	if (m->present[opt])
		return __dtfl_err(DTFL_EINVAL,
		    "Option '%s' given more than once", options[opt].longname);
	m->present[opt] = true;
	m->val[opt] = val;
	return 0;
}

/*
 * True when every option letter of the cluster "-abc" is known, up to
 * and including the first that takes an argument (whose argument is
 * the rest of the word, as getopts reads "-L/usr/lib").
 */
static bool
short_cluster_known(const char *arg)
{
	int opt;

	for (arg++; *arg != '\0'; arg++) {
		if ((opt = find_short(*arg)) < 0)
			return false;
		if (options[opt].hasarg)
			return true;
	}
	return true;
}

/*
 * opt_get_default for a usize: the default when absent, else
 * usize::from_str (optional '+', decimal digits, no overflow).
 */
static int
get_usize(const matches *m, int opt, size_t def, size_t *out)
{
	const char *s = m->val[opt], *p;
	size_t v = 0;

	if (!m->present[opt]) {
		*out = def;
		return 0;
	}
	p = *s == '+' ? s + 1 : s;
	if (*p == '\0')
		return __dtfl_err(DTFL_EINVAL, "invalid digit found in string: "
		    "--%s %s", options[opt].longname, s);
	for (; *p != '\0'; p++) {
		if (*p < '0' || *p > '9')
			return __dtfl_err(DTFL_EINVAL,
			    "invalid digit found in string: --%s %s",
			    options[opt].longname, s);
		if (v > (SIZE_MAX - (size_t)(*p - '0')) / 10)
			return __dtfl_err(DTFL_EINVAL,
			    "number too large to fit in target type: --%s %s",
			    options[opt].longname, s);
		v = v * 10 + (size_t)(*p - '0');
	}
	*out = v;
	return 0;
}

/*
 * Reads the first `n` lines of `path` (BufRead::lines: "\n" or "\r\n"
 * stripped; a final line without a newline counts).  Returns the
 * number read in *got.
 */
static int
read_hostfile(const char *path, size_t n, char **lines, size_t *got)
{
	FILE *f = fopen(path, "r");
	__dtfl_vec line;
	int c;

	*got = 0;
	if (f == NULL)
		return __dtfl_err(DTFL_EIO, "%s: %s", path, strerror(errno));
	__dtfl_vec_init(&line, 1);
	while (*got < n) {
		__dtfl_vec_clear(&line);
		while ((c = fgetc(f)) != EOF && c != '\n') {
			unsigned char ch = (unsigned char)c;

			__dtfl_vec_push(&line, &ch);
		}
		if (c == EOF && line.len == 0)
			break;
		if (line.len > 0 && line.data[line.len - 1] == '\r')
			line.len--;
		lines[*got] = __dtfl_alloc(line.len + 1);
		memcpy(lines[*got], line.data, line.len);
		lines[*got][line.len] = '\0';
		(*got)++;
		if (c == EOF)
			break;
	}
	__dtfl_vec_free(&line);
	if (ferror(f)) {
		fclose(f);
		return __dtfl_err(DTFL_EIO, "%s: read error", path);
	}
	fclose(f);
	return 0;
}

/*
 * communication Config::from_matches plus worker Config::from_matches.
 * Like upstream it builds the whole communication configuration, so
 * fields from an earlier configuration (cluster addresses, process) do
 * not survive; the seed, log addresses and spill hook are not
 * command-line state and are kept.  cfg changes only on success.
 */
static int
from_matches(const matches *m, dtfl_config *cfg)
{
	size_t threads, process, processes, i, got = 0;
	dtfl_progress_mode progress = DTFL_PROGRESS_DEMAND;
	dtfl_comm_mode mode;
	char **addresses = NULL;
	int rc;

	if ((rc = get_usize(m, OPT_W, 1, &threads)) != 0 ||
	    (rc = get_usize(m, OPT_P, 0, &process)) != 0 ||
	    (rc = get_usize(m, OPT_N, 1, &processes)) != 0)
		return rc;
	if (m->present[OPT_PROGRESS]) {
		if (strcmp(m->val[OPT_PROGRESS], "eager") == 0)
			progress = DTFL_PROGRESS_EAGER;
		else if (strcmp(m->val[OPT_PROGRESS], "demand") == 0)
			progress = DTFL_PROGRESS_DEMAND;
		else
			return __dtfl_err(DTFL_EINVAL,
			    "unknown progress mode: %s", m->val[OPT_PROGRESS]);
	}

	if (processes > 1) {
		/*
		 * Upstream does not check -p; an index outside the cluster
		 * panics later, in create_sockets.  Refuse it here, as
		 * dtfl_config_set_cluster does.
		 */
		if (process >= processes)
			return __dtfl_err(DTFL_EINVAL,
			    "-p %zu out of range for -n %zu", process, processes);
		/* Likewise a cluster of processes with no workers. */
		if (threads == 0)
			return __dtfl_err(DTFL_EINVAL, "-w 0 with -n %zu",
			    processes);
		addresses = __dtfl_calloc(processes, sizeof *addresses);
		if (m->present[OPT_H]) {
			rc = read_hostfile(m->val[OPT_H], processes, addresses,
			    &got);
			if (rc == 0 && got < processes)
				rc = __dtfl_err(DTFL_EINVAL, "could only read %zu "
				    "addresses from %s, but -n: %zu", got,
				    m->val[OPT_H], processes);
			if (rc != 0) {
				free_addresses(addresses, got);
				return rc;
			}
		} else {
			for (i = 0; i < processes; i++) {
				char buf[32];

				snprintf(buf, sizeof buf, "localhost:%zu",
				    (size_t)2101 + i);
				addresses[i] = __dtfl_strdup(buf);
			}
		}
		mode = DTFL_COMM_CLUSTER;
	} else if (threads > 1) {
		mode = m->present[OPT_Z] ? DTFL_COMM_PROCESS_BINARY :
		    DTFL_COMM_PROCESS;
		process = 0;
		processes = 0;
	} else {
		/* Config::Thread: one worker, even for -w 0. */
		mode = DTFL_COMM_THREAD;
		threads = 1;
		process = 0;
		processes = 0;
	}

	free_addresses(cfg->addresses, cfg->naddresses);
	cfg->addresses = addresses;
	cfg->naddresses = processes;
	cfg->mode = mode;
	cfg->threads = threads;
	cfg->process = process;
	cfg->report = m->present[OPT_R];
	cfg->zerocopy = m->present[OPT_Z];
	cfg->progress_mode = progress;
	return 0;
}

int
dtfl_config_from_args(dtfl_config *cfg, int argc, char *const *argv,
    char **rest, int *nrest)
{
	matches m;
	int *keep, nkeep = 0, i, opt, rc = 0;

	if (cfg == NULL || argc < 0 || (argc > 0 && argv == NULL) ||
	    rest == NULL || nrest == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_config_from_args: "
		    "NULL argument");
	memset(&m, 0, sizeof m);
	/* Indices of the arguments to hand back; rest may alias argv. */
	keep = __dtfl_calloc((size_t)argc + 1, sizeof *keep);
	if (argc > 0)
		keep[nkeep++] = 0;
	for (i = 1; i < argc && rc == 0; i++) {
		const char *a = argv[i];

		if (a[0] != '-' || a[1] == '\0') {
			keep[nkeep++] = i;	/* a free argument */
		} else if (strcmp(a, "--") == 0) {
			while (++i < argc)
				keep[nkeep++] = i;
		} else if (a[1] == '-') {
			const char *name = a + 2, *eq = strchr(name, '=');
			size_t len = eq != NULL ? (size_t)(eq - name) :
			    strlen(name);

			if ((opt = find_long(name, len)) < 0) {
				keep[nkeep++] = i;
			} else if (!options[opt].hasarg) {
				rc = eq != NULL ? __dtfl_err(DTFL_EINVAL,
				    "Option '%s' does not take an argument",
				    options[opt].longname) : record(&m, opt, NULL);
			} else if (eq != NULL) {
				rc = record(&m, opt, eq + 1);
			} else if (i + 1 < argc) {
				rc = record(&m, opt, argv[++i]);
			} else {
				rc = __dtfl_err(DTFL_EINVAL,
				    "Argument to option '%s' missing",
				    options[opt].longname);
			}
		} else if (!short_cluster_known(a)) {
			keep[nkeep++] = i;
		} else {
			const char *c;

			for (c = a + 1; *c != '\0' && rc == 0; c++) {
				opt = find_short(*c);
				if (!options[opt].hasarg) {
					rc = record(&m, opt, NULL);
				} else if (c[1] != '\0') {
					rc = record(&m, opt, c + 1);
					break;
				} else if (i + 1 < argc) {
					rc = record(&m, opt, argv[++i]);
				} else {
					rc = __dtfl_err(DTFL_EINVAL,
					    "Argument to option '%c' missing",
					    *c);
				}
			}
		}
	}
	if (rc == 0)
		rc = from_matches(&m, cfg);
	if (rc == 0) {
		for (i = 0; i < nkeep; i++)
			rest[i] = argv[keep[i]];
		*nrest = nkeep;
	}
	__dtfl_free(keep);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Environment (PLAN D9)                                               */
/* ------------------------------------------------------------------ */

int
dtfl_config_from_env(dtfl_config *cfg,
    const char *(*lookup)(const char *name, void *ctx), void *ctx)
{
	const char *v;

	if (cfg == NULL || lookup == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_config_from_env: "
		    "NULL argument");
	if ((v = lookup("TIMELY_WORKER_LOG_ADDR", ctx)) != NULL) {
		__dtfl_free(cfg->timely_log_addr);
		cfg->timely_log_addr = __dtfl_strdup(v);
	}
	if ((v = lookup("DIFFERENTIAL_LOG_ADDR", ctx)) != NULL) {
		__dtfl_free(cfg->differential_log_addr);
		cfg->differential_log_addr = __dtfl_strdup(v);
	}
	return 0;
}

const char *
dtfl_config_timely_log_addr(const dtfl_config *cfg)
{
	return cfg->timely_log_addr;
}

const char *
dtfl_config_differential_log_addr(const dtfl_config *cfg)
{
	return cfg->differential_log_addr;
}

/* ------------------------------------------------------------------ */
/* Structured event logging (dtfl/log.h)                               */
/* ------------------------------------------------------------------ */

int
dtfl_config_log_subscribe(dtfl_config *cfg, const char *stream,
    dtfl_log_subscriber_fn fn, void *ctx, void (*ctx_free)(void *))
{
	__dtfl_log_sub sub;
	size_t i;

	if (cfg == NULL || stream == NULL || fn == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_config_log_subscribe: "
		    "NULL argument");
	/* A second subscribe of the same stream replaces the first. */
	for (i = 0; i < cfg->log_subs.len; i++) {
		__dtfl_log_sub *s = DTFL_VEC_AT(&cfg->log_subs, __dtfl_log_sub, i);

		if (strcmp(s->stream, stream) == 0) {
			if (s->ctx_free != NULL)
				s->ctx_free(s->ctx);
			s->fn = fn;
			s->ctx = ctx;
			s->ctx_free = ctx_free;
			return DTFL_OK;
		}
	}
	sub.stream = __dtfl_strdup(stream);
	sub.fn = fn;
	sub.ctx = ctx;
	sub.ctx_free = ctx_free;
	__dtfl_vec_push(&cfg->log_subs, &sub);
	return DTFL_OK;
}

size_t
__dtfl_config_log_subs(const dtfl_config *cfg, const __dtfl_log_sub **subs)
{
	*subs = (const __dtfl_log_sub *)(void *)cfg->log_subs.data;
	return cfg->log_subs.len;
}
