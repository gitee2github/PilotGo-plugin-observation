// SPDX-License-Identifier: GPL-2.0
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include "runqueue-latency.h"
#include "bits.bpf.h"
#include "maps.bpf.h"
#include "core_fixes.bpf.h"

#define MAX_ENTRIES	10240
#define TASK_RUNNING	0

const volatile bool filter_memcg = false;
const volatile bool target_per_process = false;
const volatile bool target_per_thread = false;
const volatile bool target_per_pidns = false;
const volatile bool target_ms = false;
const volatile pid_t target_tgid = 0;

struct {
	__uint(type, BPF_MAP_TYPE_CGROUP_ARRAY);
	__type(key, u32);
	__type(value, u32);
	__uint(max_entries, 1);
} cgroup_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_ENTRIES);
	__type(key, u32);
	__type(value, u64);
} start SEC(".maps");

static struct hist zero;

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_ENTRIES);
	__type(key, u32);
	__type(value, struct hist);
} hists SEC(".maps");


static bool filter_memcg_fn(void)
{
	return filter_memcg && !bpf_current_task_under_cgroup(&cgroup_map, 0);
}

static int trace_enqueue(u32 tgid, u32 pid)
{
	u64 ts;

	if (filter_memcg_fn())
		return 0;

	if (!pid)
		return 0;
	if (target_tgid && target_tgid != tgid)
		return 0;

	ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&start, &pid, &ts, BPF_ANY);

	return 0;
}

static unsigned int pid_namespace(struct task_struct *task)
{
	struct pid *pid;
	unsigned int level;
	struct upid upid;
	unsigned int inum;

	/* get the pid namespace by following task_active_pid_ns(),
	 * pid->numbers[pid->level].ns
	 */
	pid = BPF_CORE_READ(task, thread_pid);
	level = BPF_CORE_READ(pid, level);
	bpf_core_read(&upid, sizeof(upid), &pid->numbers[level]);
	inum = BPF_CORE_READ(upid.ns, ns.inum);

	return inum;
}

static int handle_switch(bool preempt, struct task_struct *prev, struct task_struct *next)
{
	struct hist *histp;
	u64 *tsp, slot;
	u32 pid, hkey;
	s64 delta;

	if (filter_memcg_fn())
		return 0;

	if (get_task_state(prev) == TASK_RUNNING)
		trace_enqueue(BPF_CORE_READ(prev, tgid), BPF_CORE_READ(prev, pid));

	pid = BPF_CORE_READ(next, pid);
	tsp = bpf_map_lookup_elem(&start, &pid);
	if (!tsp)
		return 0;
	delta = bpf_ktime_get_ns() - *tsp;
	if (delta < 0)
		goto cleanup;

	if (target_per_process)
		hkey = BPF_CORE_READ(next, tgid);
	else if (target_per_thread)
		hkey = pid;
	else if (target_per_pidns)
		hkey = pid_namespace(next);
	else
		hkey = -1;

	histp = bpf_map_lookup_or_try_init(&hists, &hkey, &zero);
	if (!histp)
		goto cleanup;

	if (!histp->comm[0])
		BPF_CORE_READ_STR_INTO(&histp->comm, next, comm);
	if (target_ms)
		delta /= 1000000U;
	else
		delta /= 1000U;

	slot = log2l(delta);
	if (slot >= MAX_SLOTS)
		slot = MAX_SLOTS - 1;
	__sync_fetch_and_add(&histp->slots[slot], 1);

cleanup:
	bpf_map_delete_elem(&start, &pid);
	return 0;
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(sched_wakeup_btf, struct task_struct *p)
{
	return trace_enqueue(p->tgid, p->pid);
}

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(sched_wakeup_new_btf, struct task_struct *p)
{
	return trace_enqueue(p->tgid, p->pid);
}

SEC("tp_btf/sched_switch")
int BPF_PROG(sched_switch_btf, bool preempt, struct task_struct *prev, struct task_struct *next)
{
	return handle_switch(preempt, prev, next);
}

SEC("raw_tp/sched_wakeup")
int BPF_PROG(sched_wakeup_raw, struct task_struct *p)
{
	return trace_enqueue(BPF_CORE_READ(p, tgid), BPF_CORE_READ(p, pid));
}

SEC("raw_tp/sched_wakeup_new")
int BPF_PROG(sched_wakeup_new_raw, struct task_struct *p)
{
	return trace_enqueue(BPF_CORE_READ(p, tgid), BPF_CORE_READ(p, pid));
}

SEC("raw_tp/sched_switch")
int BPF_PROG(sched_switch_raw, bool preempt, struct task_struct *prev, struct task_struct *next)
{
	return handle_switch(preempt, prev, next);
}

char LICENSE[] SEC("license") = "GPL";