#ifndef __VMLINUX_H__
#define __VMLINUX_H__

#ifndef BPF_NO_PRESERVE_ACCESS_INDEX
#pragma clang attribute push (__attribute__((preserve_access_index)), apply_to = record)
#endif

typedef unsigned char __u8;

typedef short unsigned int __u16;

typedef int __s32;

typedef unsigned int __u32;

typedef long long int __s64;

typedef long long unsigned int __u64;

typedef __u8 u8;

typedef __u16 u16;

typedef __s32 s32;

typedef __u32 u32;

typedef __s64 s64;

typedef __u64 u64;

enum {
	false = 0,
	true = 1,
};

typedef long int __kernel_long_t;

typedef long unsigned int __kernel_ulong_t;

typedef int __kernel_pid_t;

typedef unsigned int __kernel_uid32_t;

typedef __kernel_ulong_t __kernel_size_t;

typedef long long int __kernel_time64_t;

typedef __kernel_long_t __kernel_clock_t;

typedef int __kernel_timer_t;

typedef int __kernel_clockid_t;

typedef __kernel_pid_t pid_t;

typedef __kernel_clockid_t clockid_t;

typedef _Bool bool;

typedef __kernel_uid32_t uid_t;

typedef __kernel_size_t size_t;

typedef unsigned int gfp_t;

typedef u64 phys_addr_t;

typedef struct {
	int counter;
} atomic_t;

typedef struct {
	s64 counter;
} atomic64_t;

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

struct hlist_node;

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next;
	struct hlist_node **pprev;
};

struct callback_head {
	struct callback_head *next;
	void (*func)(struct callback_head *);
};

struct jump_entry {
	s32 code;
	s32 target;
	long int key;
};

struct static_key_mod;

struct static_key {
	atomic_t enabled;
	union {
		long unsigned int type;
		struct jump_entry *entries;
		struct static_key_mod *next;
	};
};

struct static_key_false {
	struct static_key key;
};

struct qspinlock {
	union {
		atomic_t val;
		struct {
			u8 locked;
			u8 pending;
		};
		struct {
			u16 locked_pending;
			u16 tail;
		};
	};
};

typedef struct qspinlock arch_spinlock_t;

struct qrwlock {
	union {
		atomic_t cnts;
		struct {
			u8 wlocked;
			u8 __lstate[3];
		};
	};
	arch_spinlock_t wait_lock;
};

typedef struct qrwlock arch_rwlock_t;

struct lockdep_subclass_key {
	char __one_byte;
};

struct lock_class_key {
	union {
		struct hlist_node hash_entry;
		struct lockdep_subclass_key subkeys[8];
	};
};

struct lock_trace;

struct lock_class {
	struct hlist_node hash_entry;
	struct list_head lock_entry;
	struct list_head locks_after;
	struct list_head locks_before;
	const struct lockdep_subclass_key *key;
	unsigned int subclass;
	unsigned int dep_gen_id;
	long unsigned int usage_mask;
	const struct lock_trace *usage_traces[10];
	int name_version;
	const char *name;
	u8 wait_type_inner;
	u8 wait_type_outer;
	u8 lock_type;
};

struct lock_trace {
	struct hlist_node hash_entry;
	u32 hash;
	u32 nr_entries;
	long unsigned int entries[0];
};

struct lockdep_map {
	struct lock_class_key *key;
	struct lock_class *class_cache[2];
	const char *name;
	u8 wait_type_outer;
	u8 wait_type_inner;
	u8 lock_type;
};