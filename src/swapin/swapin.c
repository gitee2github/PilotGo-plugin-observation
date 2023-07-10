// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "commons.h"
#include "swapin.h"
#include "swapin.skel.h"
#include "trace_helpers.h"

static struct env {
	time_t interval;
	pid_t pid;
	int times;
	bool timestamp;
	bool verbose;
} env = {
	.interval = 1,
	.times = 99999999,
};

static volatile sig_atomic_t exiting;

const char *argp_program_version = "swapin 0.1";
const char *argp_program_bug_address = "Jackie Liu <liuyun01@kylinos.cn>";
const char argp_program_doc[] =
"Count swapins by process.\n"
"\n"
"USAGE: swapin [--help] [--timestamp] [--interval INT] [--times TIMES] "
"[--pid PID] [--verbose]\n"
"\n"
"EXAMPLES:\n"
"    swapin          # Print swapins per-process\n";

static const struct argp_option opts[] = {
	{ "timestamp", 'T', NULL, 0, "Include timestamp in output" },
	{ "interval", 'i', "INTERVAL", 0, "Output interval, in seconds (Default 1)" },
	{ "times", 't', "TIMES", 0, "The number of outputs" },
	{ "pid", 'p', "PID", 0, "Trace this PID only" },
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help" },
	{}
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'p':
		env.pid = argp_parse_pid(key, arg, state);
		break;
	case 'i':
		env.interval = argp_parse_long(key, arg, state);
		break;
	case 't':
		env.times = argp_parse_long(key, arg, state);
		break;
	case 'T':
		env.timestamp = true;
		break;
	case 'v':
		env.verbose = true;
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

int main(int argc, char *argv[])
{
	static const struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.doc = argp_program_doc,
	};
	struct swapin_bpf *obj;
	int err;

	err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (err)
		return err;
	
	if (!bpf_is_root())
		return 1;

	libbpf_set_print(libbpf_print_fn);
        
	obj = swapin_bpf__open();
	if (!obj) {
		warning("Failed to open BPF object\n");
		return 1;
	}

	if (fentry_can_attach("swap_readpage", NULL))
		bpf_program__set_autoload(obj->progs.swap_readpage_kprobe, false);
	else
		bpf_program__set_autoload(obj->progs.swap_readpage_fentry, false);

	obj->rodata->target_pid = env.pid;
        
	err = swapin_bpf__load(obj);
	if (err) {
		warning("Failed to load BPF object: %d\n", err);
		goto cleanup;
	}

	err = swapin_bpf__attach(obj);
	if (err) {
		warning("Failed to attach BPF programs: %d\n", err);
		goto cleanup;
	}
        
	signal(SIGINT, sig_handler);

	while (!exiting) {
		sleep(env.interval);
		printf("\n");

		if (env.timestamp) {
			char ts[32];

			strftime_now(ts, sizeof(ts), "%H:%M:%S");
			printf("%-8s\n", ts);
		}

		printf("%-16s %-7s %s\n", "COMM", "PID", "COUNT");

		err = print_map(bpf_map__fd(obj->maps.counts));
		if (err)
			break;

		if (--env.times == 0)
			break;
	}

cleanup:
	swapin_bpf__destroy(obj);

	return err != 0;
}
