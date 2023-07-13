// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "commons.h"
#include "runqueue-latency.h"
#include "runqueue-latency.skel.h"
#include "trace_helpers.h"

struct env {
	time_t interval;
	pid_t pid;
	int times;
	bool milliseconds;
	bool per_process;
	bool per_thread;
	bool per_pidns;
	bool timestamp;
	bool verbose;
	char *cgroupspath;
	bool cg;
} env = {
	.interval = 99999999,
	.times = 99999999,
};

static volatile sig_atomic_t exiting;

const char *argp_program_version = "runqueue-latency 0.1";
const char *argp_program_bug_address = "Jackie Liu <liuyun01@kylinos.cn>";
const char argp_program_doc[] =
"Summarize run queue (scheduler) latency as a histogram.\n"
"\n"
"USAGE: runqlat [--help] [-T] [-m] [--pidnss] [-L] [-P] [-p PID] [interval] [count] [-c CG]\n"
"\n"
"EXAMPLES:\n"
"    runqlat         # summarize run queue latency as a histogram\n"
"    runqlat 1 10    # print 1 second summaries, 10 times\n"
"    runqlat -mT 1   # 1s summaries, milliseconds, and timestamps\n"
"    runqlat -P      # show each PID separately\n"
"    runqlat -p 185  # trace PID 185 only\n"
"    runqlat -c CG   # Trace process under cgroupsPath CG\n";

#define OPT_PIDNSS	1 /* --pidnss */

static const struct argp_option opts[] = {
	{ "timestamp", 'T', NULL, 0, "Include timestamp on output" },
	{ "milliseconds", 'm', NULL, 0, "Millisecond histogram" },
	{ "pidnss", OPT_PIDNSS, NULL, 0, "Print a histogram per PID namespace" },
	{ "pids", 'P', NULL, 0, "Print a histogram per process ID" },
	{ "tids", 'L', NULL, 0, "Print a histogram per thread ID" },
	{ "pid", 'p', "PID", 0, "Trace this PID only"},
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{ "cgroup", 'c', "/sys/fs/cgroup/unified", 0, "Trace process in cgroup path" },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help" },
	{},
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
	case 'm':
		env.milliseconds = true;
		break;
	case 'p':
		env.pid = argp_parse_pid(key, arg, state);
		break;
	case 'L':
		env.per_thread = true;
		break;
	case 'P':
		env.per_process = true;
		break;
	case OPT_PIDNSS:
		env.per_pidns = true;
		break;
	case 'T':
		env.timestamp = true;
		break;
	case 'c':
		env.cgroupspath = arg;
		env.cg = true;
		break;
	case ARGP_KEY_ARG:
		errno = 0;
		if (pos_args == 0) {
			env.interval = strtol(arg, NULL, 10);
			if (errno) {
				warning("Invalid interval\n");
				argp_usage(state);
			}
		} else if (pos_args == 1) {
			env.times = strtol(arg, NULL, 10);
			if (errno) {
				warning("Invalid times\n");
				argp_usage(state);
			}
		} else {
			warning("Unrecongnized positional argument: %s\n", arg);
			argp_usage(state);
		}
		pos_args++;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}