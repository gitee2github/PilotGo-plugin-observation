// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include "commons.h"
#include "klockstat.h"
#include "klockstat.skel.h"
#include "trace_helpers.h"
#include "compat.h"
#include <sys/param.h>

static struct prog_env {
    pid_t pid;
	pid_t tid;
	char *caller;
    char *lock_name;
	unsigned int nr_locks;
	unsigned int nr_stack_entries;
	unsigned int duration;
    unsigned int interval;
    unsigned int iterations;
	bool reset;
	bool timestamp;
	bool verbose;
	bool per_thread;
}

static const char args_doc[] = "FUNCTION";
static const char argp_program_doc[] =
"Trace mutex/sem lock acquisition and hold times, in nsec\n"
"\n"
"Usage: klockstat [-hPRTv] [-p PID] [-t TID] [-c FUNC] [-L LOCK] [-n NR_LOCKS]\n"
"                 [-s NR_STACKS] [-S SORT] [-d DURATION] [-i INTERVAL]\n"
"\v"
"Examples:\n"
"  klockstat                     # trace system wide until ctrl-c\n"
"  klockstat -d 5                # trace for 5 seconds\n"
"  klockstat -i 5                # print stats every 5 seconds\n"
"  klockstat -p 181              # trace process 181 only\n"
"  klockstat -t 181              # trace thread 181 only\n"
"  klockstat -c pipe_            # print only for lock callers with 'pipe_'\n"
"                                # prefix\n"
"  klockstat -L cgroup_mutex     # trace the cgroup_mutex lock only (accepts addr too)\n"
"  klockstat -S acq_count        # sort lock acquired results by acquire count\n"
"  klockstat -S hld_total        # sort lock held results by total held time\n"
"  klockstat -S acq_count,hld_total  # combination of above\n"
"  klockstat -n 3                # display top 3 locks/threads\n"
"  klockstat -s 6                # display 6 stack entries per lock\n"
"  klockstat -P                  # print stats per thread\n";

static const struct argp_option opts[] = {
	{ "pid", 'p', "PID", 0, "Filter by process ID" },
	{ "tid", 't', "TID", 0, "Filter by thread ID" },
	{ 0, 0, 0, 0, "" },
	{ "caller", 'c', "FUNC", 0, "Filter by caller string prefix " },
	{ "lock", 'L', "LOCK", 0, "Filter by specific ksym lock name" },
	{ 0, 0, 0, 0, "" },
	{ "locks", 'n', "NR_LOCKS", 0, "Number of locks or threads to print" },
	{ "stacks", 's', "NR_STACKS", 0, "Number of stack entries to print per lock" },
	{ "sort", 'S', "SORT", 0, "Sort by field:\n  acq_[max|total|count]\n  hld_[max|total|count]" },
	{ 0, 0, 0, 0, "" },
	{ "duration", 'd', "SECONDS", 0, "Duration to trace" },
	{ "interval", 'i', "SECONDS", 0, "Print interval" },
	{ "reset", 'R', NULL, 0, "Reset stats each interval" },
	{ "timestamp", 'T', NULL, 0, "Print timestamp" },
	{ "verbose", 'v', NULL, 0, "Verbose debug output" },
	{ "per-thread", 'P', NULL, 0, "Print per-thread stats" },
	{ NULL, 'h', NULL, OPTION_HIDDEN, "Show the full help" },
	{}
};

static void *parse_lock_addr(const char *lock_name)
{
	unsigned long lock_addr;

	return sscanf(lock_name, "0x%lx", &lock_addr) ? (void *)lock_addr : NULL;
}

static void *get_lock_addr(struct ksyms *ksyms, const char *lock_name)
{
	const struct ksym *ksym = ksyms__get_symbol(ksyms, lock_name);

	return ksym ? (void *)ksym->addr : parse_lock_addr(lock_name);
}

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	struct prog_env *env = state->input;

	switch (key) {
	case 'p':
		env->pid = argp_parse_pid(key, arg, state);
		break;
	case 't':
		errno = 0;
		env->tid = strtol(arg, NULL, 10);
		if (errno || env->tid <= 0) {
			warning("Invalid TID: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'c':
		env->caller = arg;
		break;
	case 'L':
		env->lock_name = arg;
		break;
	case 'n':
		errno = 0;
		env->nr_locks = strtol(arg, NULL, 10);
		if (errno || env->nr_locks <= 0) {
			warning("Invalid NR_LOCKS: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 's':
		errno = 0;
		env->nr_stack_entries = strtol(arg, NULL, 10);
		if (errno || env->nr_stack_entries <= 0) {
			warning("Invalid NR_STACKS: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'S':
		if (!parse_sorts(env, arg)) {
			warning("Bad sort string: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'd':
		errno = 0;
		env->duration = strtol(arg, NULL, 10);
		if (errno || env->duration <= 0) {
			warning("Invalid duration: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'i':
		errno = 0;
		env->interval = strtol(arg, NULL, 10);
		if (errno || env->interval <= 0) {
			warning("Invalid duration: %s\n", arg);
			argp_usage(state);
		}
		break;
	case 'R':
		env->reset = true;
		break;
	case 'T':
		env->timestamp = true;
		break;
	case 'P':
		env->per_thread = true;
		break;
	case 'h':
		argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
		break;
	case 'v':
		env->verbose = true;
		break;
	case ARGP_KEY_END:
		if (env->duration) {
			env->interval = min(env->interval, env->duration);
			env->iterations = env->duration / env->interval;
		}
		if (env->per_thread && env->nr_stack_entries != 1) {
			warning("--per-thread and --stacks cannot be used together\n");
			argp_usage(state);
		}
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static volatile sig_atomic_t exiting;

static void sig_handler(int sig)
{
	exiting = 1;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			  va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void enable_fentry(struct klockstat_bpf *obj)
{
	bool debug_lock;

	bpf_program__set_autoload(obj->progs.kprobe_mutex_lock, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_lock_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_trylock, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_trylock_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_lock_interruptible, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_lock_interruptible_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_lock_killable, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_lock_killable_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_mutex_unlock, false);

	bpf_program__set_autoload(obj->progs.kprobe_down_read, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_read_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_read_trylock, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_read_trylock_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_read_interruptible, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_read_interruptible_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_read_killable, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_read_killable_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_up_read, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_write, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_write_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_write_trylock, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_write_trylock_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_write_killable, false);
	bpf_program__set_autoload(obj->progs.kprobe_down_write_killable_exit, false);
	bpf_program__set_autoload(obj->progs.kprobe_up_write, false);

	/**
	 * commit 31784cff7ee0 ("rwsem: Implement down_read_interruptible")
	 */
	if (!fentry_can_attach("down_read_interruptible", NULL)) {
		bpf_program__set_autoload(obj->progs.down_read_interruptible, false);
		bpf_program__set_autoload(obj->progs.down_read_interruptible_exit, false);
	}

	/* CONFIG_DEBUG_LOCK_ALLOC is on */
	debug_lock = fentry_can_attach("mutex_lock_nested", NULL);
	if (!debug_lock)
		return;

	bpf_program__set_attach_target(obj->progs.mutex_lock, 0,
				       "mutex_lock_nested");
	bpf_program__set_attach_target(obj->progs.mutex_lock_exit, 0,
				       "mutex_lock_nested");
	bpf_program__set_attach_target(obj->progs.mutex_lock_interruptible, 0,
				       "mutex_lock_interruptible_nested");
	bpf_program__set_attach_target(obj->progs.mutex_lock_interruptible_exit, 0,
				       "mutex_lock_interruptible_nested");
	bpf_program__set_attach_target(obj->progs.mutex_lock_killable, 0,
				       "mutex_lock_killable_nested");
	bpf_program__set_attach_target(obj->progs.mutex_lock_killable_exit, 0,
				       "mutex_lock_killable_nested");

	bpf_program__set_attach_target(obj->progs.down_read, 0,
				       "down_read_nested");
	bpf_program__set_attach_target(obj->progs.down_read_exit, 0,
				       "down_read_nested");
	bpf_program__set_attach_target(obj->progs.down_read_killable, 0,
				       "down_read_killable_nested");
	bpf_program__set_attach_target(obj->progs.down_read_killable_exit, 0,
				       "down_read_killable_nested");
	bpf_program__set_attach_target(obj->progs.down_write, 0,
				       "down_write_nested");
	bpf_program__set_attach_target(obj->progs.down_write_exit, 0,
				       "down_write_nested");
	bpf_program__set_attach_target(obj->progs.down_write_killable, 0,
				       "down_write_killable_nested");
	bpf_program__set_attach_target(obj->progs.down_write_killable_exit, 0,
				       "down_write_killable_nested");
}

static void enable_kprobes(struct klockstat_bpf *obj)
{
	bpf_program__set_autoload(obj->progs.mutex_lock, false);
	bpf_program__set_autoload(obj->progs.mutex_lock_exit, false);
	bpf_program__set_autoload(obj->progs.mutex_trylock_exit, false);
	bpf_program__set_autoload(obj->progs.mutex_lock_interruptible, false);
	bpf_program__set_autoload(obj->progs.mutex_lock_interruptible_exit, false);
	bpf_program__set_autoload(obj->progs.mutex_lock_killable, false);
	bpf_program__set_autoload(obj->progs.mutex_lock_killable_exit, false);
	bpf_program__set_autoload(obj->progs.mutex_unlock, false);

	bpf_program__set_autoload(obj->progs.down_read, false);
	bpf_program__set_autoload(obj->progs.down_read_exit, false);
	bpf_program__set_autoload(obj->progs.down_read_trylock_exit, false);
	bpf_program__set_autoload(obj->progs.down_read_interruptible, false);
	bpf_program__set_autoload(obj->progs.down_read_interruptible_exit, false);
	bpf_program__set_autoload(obj->progs.down_read_killable, false);
	bpf_program__set_autoload(obj->progs.down_read_killable_exit, false);
	bpf_program__set_autoload(obj->progs.up_read, false);
	bpf_program__set_autoload(obj->progs.down_write, false);
	bpf_program__set_autoload(obj->progs.down_write_exit, false);
	bpf_program__set_autoload(obj->progs.down_write_trylock_exit, false);
	bpf_program__set_autoload(obj->progs.down_write_killable, false);
	bpf_program__set_autoload(obj->progs.down_write_killable_exit, false);
	bpf_program__set_autoload(obj->progs.up_write, false);

	/**
	 * commit 31784cff7ee0 ("rwsem: Implement down_read_interruptible")
	 */
	if (!kprobe_exists("down_read_interruptible")) {
		bpf_program__set_autoload(obj->progs.kprobe_down_read_interruptible, false);
		bpf_program__set_autoload(obj->progs.kprobe_down_read_interruptible_exit, false);
	}
}

int main(int argc, char *argv[])
{
    static struct argp argp = {
		.options = opts,
		.parser = parse_arg,
		.args_doc = args_doc,
		.doc = argp_program_doc,
	};
    struct ksyms *ksyms = NULL;
    int err;
    void *lock_addr = NULL;

    err = argp_parse(&argp, argc, argv, 0, NULL, &env);
	if (err)
		return err;

    if (!bpf_is_root())
		return 1;

    signal(SIGINT, sig_handler);
	libbpf_set_print(libbpf_print_fn);

    ksyms = ksyms__load();
	if (!ksyms) {
		warning("failed to load kallsyms\n");
		err = 1;
		goto cleanup;
	}
	if (env.lock_name) {
		lock_addr = get_lock_addr(ksyms, env.lock_name);
		if (!lock_addr) {
			warning("Failed to find lock %s\n", env.lock_name);
			err = 1;
			goto cleanup;
		}
	}

    obj = klockstat_bpf__open();
	if (!obj) {
		warning("Failed to open BPF object\n");
		err = 1;
		goto cleanup;
	}

	obj->rodata->target_tgid = env.pid;
	obj->rodata->target_pid = env.tid;
	obj->rodata->target_lock = lock_addr;
	obj->rodata->per_thread = env.per_thread;

	if (fentry_can_attach("mutex_locK", NULL) ||
	    fentry_can_attach("mutex_lock_nested", NULL))
		enable_fentry(obj);
	else
		enable_kprobes(obj);
    
    err = klockstat_bpf__load(obj);
	if (err) {
		warning("Failed to load BPF object\n");
		goto cleanup;
	}

	err = klockstat_bpf__attach(obj);
	if (err) {
		warning("Failed to attach BPF programs\n");
		goto cleanup;
	}

    printf("Tracing mutex/sem lock events... Hit Ctrl-C to end\n");

	for (int i = 0; i < env.iterations && !exiting; i++) {
		sleep(env.interval);

		printf("\n");
		if (env.timestamp) {
			char ts[32];

			strftime_now(ts, sizeof(ts), "%H:%M:%S");
			printf("%-8s\n", ts);
		}

		if (print_stats(ksyms, bpf_map__fd(obj->maps.stack_map),
				bpf_map__fd(obj->maps.stat_map))) {
			warning("print_stats error, aborting.\n");
			break;
		}
		fflush(stdout);
	}

    printf("Exiting trace of mutex/sem locks\n");

cleanup:
    if (obj)
        klockstat_bpf__destroy(obj);
    ksyms__free(ksyms);

    return err != 0;
}