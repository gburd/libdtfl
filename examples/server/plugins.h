/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/server/plugins.h: the example dd_server plugins.
 *
 * Each plugin is a statically registered dtfl_dataflow_plugin (dd_server.h):
 * a name and a build callback.  The dd_server tool and the P14 tests
 * register these by taking their address, the static-registration
 * stand-in for dlopen (the divergence from server/src/bin/dd_server.rs,
 * which opens shared objects).  A plugin uses only the public dtfl_* API.
 *
 * A build callback returns 0 on success or a DTFL_E* code (the Rust
 * Result<(), String>); on an argument error it prints a message to stderr
 * and returns DTFL_EINVAL, since a plugin cannot set dtfl_last_error_detail
 * through the public API.
 */
#ifndef DTFL_EXAMPLES_SERVER_PLUGINS_H
#define DTFL_EXAMPLES_SERVER_PLUGINS_H

#include "dtfl/dd_server.h"

#include <stdio.h>

extern const dtfl_dataflow_plugin dtfl_plugin_degr_dist;
extern const dtfl_dataflow_plugin dtfl_plugin_reachability;
extern const dtfl_dataflow_plugin dtfl_plugin_neighborhood;
extern const dtfl_dataflow_plugin dtfl_plugin_random_graph;

/* Report a wrong argument count and return DTFL_EINVAL, as the upstream
 * plugins return Err("expected N arguments ...").  `want` is the count the
 * plugin needs; the message names the plugin. */
static inline int
dtfl_plugin_argc_error(const char *plugin, size_t want, size_t got)
{
	fprintf(stderr, "%s: expected %zu argument%s, got %zu\n", plugin, want,
	    want == 1 ? "" : "s", got);
	return 1; /* DTFL_EINVAL */
}

#endif /* DTFL_EXAMPLES_SERVER_PLUGINS_H */
