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

static void handle_event(void *ctx, int cpu, void *data, __u32 data_sz)
{
	const struct event *e = data;
	char ts[16];

	strftime_now(ts, sizeof(ts), "%H:%M:%S");
	printf("%-8s %-7d %-16s %-10.3f %-s\n",
	       ts, e->pid, e->comm, (double)e->time/1000000, e->host);
}

static void handle_lost_events(void *ctx, int cpu, __u64 lost_cnt)
{
	warning("lost %llu events on CPU #%d!\n", lost_cnt, cpu);
}

static int get_libc_path(char *path)
{
	FILE *fp;
	char buf[PATH_MAX] = {};
	char *filename;
	float version;

	if (env.libc_path) {
		memcpy(path, env.libc_path, strlen(env.libc_path));
		return 0;
	}

	fp = fopen("/proc/self/maps", "r");
	if (!fp)
		return -errno;

	while (fscanf(fp, "%*x-%*x %*s %*s %*s %*s %[^\n]\n", buf) != EOF) {
		if (strchr(buf, '/') != buf)
			continue;
		filename = strrchr(buf, '/') + 1;
		if (sscanf(filename, "libc-%f.so", &version) == 1 ||
		    sscanf(filename, "libc.so.%f", &version) == 1) {
			memcpy(path, buf, strlen(buf));
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);
	return -1;
}

static int attach_uprobes(struct gethostlatency_bpf *obj, struct bpf_link *links[])
{
	char libc_path[PATH_MAX] = {};
	off_t func_off;
	int err;

	err = get_libc_path(libc_path);
	if (err) {
		warning("Could not find libc.so\n");
		return -1;
	}

	func_off = get_elf_func_offset(libc_path, "getaddrinfo");
	if (func_off < 0) {
		warning("Could not find getaddrinfo in %s\n", libc_path);
		return -1;
	}

	links[0] = bpf_program__attach_uprobe(obj->progs.handle_entry_gethost, false,
					      env.pid ?: -1, libc_path, func_off);
	if (!links[0]) {
		warning("Failed to attach getaddrinfo: %d\n", -errno);
		return -1;
	}

	links[1] = bpf_program__attach_uprobe(obj->progs.handle_return_gethost, true,
					      env.pid ?: -1, libc_path, func_off);
	if (!links[1]) {
		warning("Failed to attach getaddrinfo: %d\n", -errno);
		return -1;
	}

	func_off = get_elf_func_offset(libc_path, "gethostbyname");
	if (func_off < 0) {
		warning("Could not find gethostbyname in %s\n", libc_path);
		return -1;
	}

	links[2] = bpf_program__attach_uprobe(obj->progs.handle_entry_gethost, false,
					      env.pid ?: -1, libc_path, func_off);
	if (!links[2]) {
		warning("Failed to attach gethostbyname: %d\n", -errno);
		return -1;
	}

	links[3] = bpf_program__attach_uprobe(obj->progs.handle_return_gethost, true,
					      env.pid ?: -1, libc_path, func_off);
	if (!links[3]) {
		warning("Failed to attach gethostbyname: %d\n", -errno);
		return -1;
	}

	func_off = get_elf_func_offset(libc_path, "gethostbyname2");
	if (func_off < 0) {
		warning("Could not find gethostbyname2 in %s\n", libc_path);
		return -1;
	}

	links[4] = bpf_program__attach_uprobe(obj->progs.handle_entry_gethost, false,
					      env.pid ?: -1, libc_path, func_off);
	if (!links[4]) {
		warning("Failed to attach gethostbyname2: %d\n", -errno);
		return -1;
	}

	links[5] = bpf_program__attach_uprobe(obj->progs.handle_return_gethost, true,
					      env.pid ?: -1, libc_path, func_off);
	if (!links[5]) {
		warning("Failed to attach gethostbyname2: %d\n", -errno);
		return -1;
	}

	return 0;
}
