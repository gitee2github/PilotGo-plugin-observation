// SPDX-License-Identifier: GPL-2.0

#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "core_fixes.bpf.h"
#include "runqslower.h"

#define TASK_RUNNING 0
#define BPF_F_CURRENT_CPU 0xffffffffULL

const volatile __u64 min_us = 0;
const volatile pid_t target_pid = 0;
const volatile pid_t target_tgid = 0;

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, u32);
	__type(value, u64);
} start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u32));
} events SEC(".maps");

/* record enqueue timestamp */
__always_inline
static int trace_enqueue(struct task_struct *p)
{
	u32 pid = BPF_CORE_READ(p, pid);
	u32 tgid = BPF_CORE_READ(p, tgid);
	u64 ts;

	if (!pid)
		return 0;
	if (target_pid && target_pid != pid)
		return 0;
	if (target_tgid && target_tgid != tgid)
		return 0;

	ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&start, &pid, &ts, 0);
	return 0;
}


__always_inline
static int handle_switch(void *ctx, struct task_struct *prev, struct task_struct *next)
{
	struct runq_event event = {};

	u64 *tsp, delta_us;
	u32 pid;

	/* treat like an enqueue event and store timestamp */
	if (get_task_state(prev) == TASK_RUNNING)
		trace_enqueue(prev);

	pid = BPF_CORE_READ(next, pid);

	/* fetch timestamp and calculate delta */
	tsp = bpf_map_lookup_elem(&start, &pid);
	if (!tsp)
		return 0;

	delta_us = (bpf_ktime_get_ns() - *tsp) / 1000;
	/* not slow? return */
	if (min_us && delta_us <= min_us)
		return 0;

	event.pid = pid;
	event.prev_pid = BPF_CORE_READ(prev, pid);
	event.delta_us = delta_us;
	BPF_CORE_READ_STR_INTO(&event.task, next, comm);
	BPF_CORE_READ_STR_INTO(&event.prev_task, prev, comm);

	/* output */
	bpf_perf_event_output(ctx, &events, BPF_F_CURRENT_CPU,
			      &event, sizeof(event));

	bpf_map_delete_elem(&start, &pid);
	return 0;
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(sched_wakeup, struct task_struct *p)
{
	/* TP_PROTO(struct task_struct *p) */
	return trace_enqueue(p);
}