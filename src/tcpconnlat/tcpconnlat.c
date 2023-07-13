// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "commons.h"
#include "tcpconnlat.h"
#include "tcpconnlat.skel.h"
#include "trace_helpers.h"
#include "compat.h"

#include <arpa/inet.h>

static volatile sig_atomic_t exiting;

static struct env {
	__u64 min_us;
	pid_t pid;
	bool timestamp;
	bool lport;
	bool verbose;
} env;

const char *argp_program_version = "tcpconnlat 0.1";
const char *argp_program_bug_address = "Jackie Liu <liuyun01@kylinos.cn>";
const char argp_program_doc[] =
"\nTrace TCP connects and show connection latency.\n"
"\n"
"USAGE: tcpconnlat [--help] [-t] [-p PID] [-L]\n"
"\n"
"EXAMPLES:\n"
"    tcpconnlat              # summarize on-CPU time as a histogram\n"
"    tcpconnlat 1            # trace connection latency slower than 1 ms\n"
"    tcpconnlat 0.1          # trace connection latency slower than 100 us\n"
"    tcpconnlat -t           # 1s summaries, milliseconds, and timestamps\n"
"    tcpconnlat -p 185       # trace PID 185 only\n"
"    tcpconnlat -L           # include LPORT while printing outputs\n";

static const struct argp_option opts[] = {
	{ "timestamp", 't', NULL, 0, "Include timestamp on output" },
	{ "pid", 'p', "PID", 0, "Trace this PID only" },
	{ "lport", 'L', NULL, 0, "Include LPORT on output" },
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help" },
	{}
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	static int pos_args;

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
	case 't':
		env.timestamp = true;
		break;
	case 'L':
		env.lport = true;
		break;
	case ARGP_KEY_ARG:
		if (pos_args++) {
			warning("Unrecognized positional argument: %s\n", arg);
			argp_usage(state);
		}
		errno = 0;
		env.min_us = strtod(arg, NULL) * 1000;
		if (errno || env.min_us <= 0) {
			warning("Invalid delay (in us) %s\n", arg);
			argp_usage(state);
		}
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sig_handler(int sig)
{
	exiting = 1;
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	const struct event *e = data;
	char src[INET6_ADDRSTRLEN];
	char dst[INET6_ADDRSTRLEN];
	union {
		struct in_addr  x4;
		struct in6_addr x6;
	} s, d;

	if (env.timestamp)
		printf("%-9.3f ", time_since_start());

	if (e->af == AF_INET) {
		s.x4.s_addr = e->saddr_v4;
		d.x4.s_addr = e->daddr_v4;
	} else if (e->af == AF_INET6) {
		memcpy(&s.x6.s6_addr, e->saddr_v6, sizeof(s.x6.s6_addr));
		memcpy(&d.x6.s6_addr, e->daddr_v6, sizeof(d.x6.s6_addr));
	} else {
		warning("broken event: event->af=%d", e->af);
		return 1;
	}

	if (env.lport) {
		printf("%-6d %-16.16s v%d %-16s %-6d %-16s %-5d %.2f\n", e->tgid,
		       e->comm, e->af == AF_INET ? 4 : 6,
		       inet_ntop(e->af, &s, src, sizeof(src)), e->lport,
		       inet_ntop(e->af, &d, dst, sizeof(dst)), ntohs(e->dport),
		       e->delta_us / 1000.0);
	} else {
		printf("%-6d %-16.16s v%d %-16s %-16s %-5d %.2f\n", e->tgid, e->comm,
		       e->af == AF_INET ? 4 : 6,
		       inet_ntop(e->af, &s, src, sizeof(src)),
		       inet_ntop(e->af, &d, dst, sizeof(dst)), ntohs(e->dport),
		       e->delta_us / 1000.0);
	}

	return 0;
}