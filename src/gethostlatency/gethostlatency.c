// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "commons.h"
#include "gethostlatency.h"
#include "gethostlatency.skel.h"
#include "btf_helpers.h"
#include "trace_helpers.h"
#include "uprobe_helpers.h"

static volatile sig_atomic_t exiting;

static struct env {
	bool verbose;
	pid_t pid;
	char *libc_path;
} env = {};

const char *argp_program_version = "gethostlatency 0.1";
const char *argp_program_bug_address = "Jackie Liu <liuyun01@kylinos.cn>";
const char argp_program_doc[] =
"Show latency for getaddrinfo/gethostbyname[2] calls.\n"
"\n"
"USAGE: gethostlatency [-h] [-p PID] [-l LIBC]\n"
"\n"
"EXAMPLES:\n"
"    gethostlatency             # time getaddrinfo/gethostbyname[2] calls\n"
"    gethostlatency -p 1216     # only trace PID 1216\n";

static const struct argp_option opts[] = {
	{ "pid", 'p', "PID", 0, "Process ID to trace" },
	{ "libc", 'l', "LIBC", 0, "Specify which libc.so to use" },
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{ NULL, 'h', NULL, 0, "Show the full help" },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'v':
		env.verbose = true;
		break;
	case 'p':
		env.pid = argp_parse_pid(key, arg, state);
		break;
	case 'l':
		if (!arg)
			return ARGP_ERR_UNKNOWN;
		env.libc_path = strdup(arg);
		if (access(env.libc_path, F_OK)) {
			warning("Invalid libc: %s\n", arg);
			argp_usage(state);
		}
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}