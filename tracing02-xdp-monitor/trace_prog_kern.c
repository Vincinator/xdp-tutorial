/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/bpf.h>
#include "bpf_helpers.h"

struct bpf_map_def SEC("maps") redirect_err_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(__u32),
	.value_size	= sizeof(__u64),
	.max_entries	= 2,
	/* TODO: have entries for all possible errno's */
};

#define XDP_UNKNOWN	XDP_REDIRECT + 1
struct bpf_map_def SEC("maps") exception_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(__u32),
	.value_size	= sizeof(__u64),
	.max_entries	= 6, //XDP_UNKNOWN + 1,
};

/* Tracepoint format: /sys/kernel/debug/tracing/events/xdp/xdp_redirect/format
 * Code in:                kernel/include/trace/events/xdp.h
 */
struct xdp_redirect_ctx {
	__u64 __pad;		// First 8 bytes are not accessible by bpf code
	int prog_id;		//	offset:8;  size:4; signed:1;
	__u32 act;		//	offset:12  size:4; signed:0;
	int ifindex;		//	offset:16  size:4; signed:1;
	int err;		//	offset:20  size:4; signed:1;
	int to_ifindex;		//	offset:24  size:4; signed:1;
	__u32 map_id;		//	offset:28  size:4; signed:0;
	int map_index;		//	offset:32  size:4; signed:1;
};				//	offset:36

enum {
	XDP_REDIRECT_SUCCESS = 0,
	XDP_REDIRECT_ERROR = 1
};

static __always_inline
int xdp_redirect_collect_stat(struct xdp_redirect_ctx *ctx)
{
	__u32 key = XDP_REDIRECT_ERROR;
	int err = ctx->err;
	__u64 *cnt;

	if (!err)
		key = XDP_REDIRECT_SUCCESS;

	cnt  = bpf_map_lookup_elem(&redirect_err_cnt, &key);
	if (!cnt)
		return 1;
	*cnt += 1;

	return 0; /* Indicate event was filtered (no further processing)*/
	/*
	 * Returning 1 here would allow e.g. a perf-record tracepoint
	 * to see and record these events, but it doesn't work well
	 * in-practice as stopping perf-record also unload this
	 * bpf_prog.  Plus, there is additional overhead of doing so.
	 */
}

SEC("raw_tracepoint/xdp/xdp_redirect_err")
int trace_xdp_redirect_err(struct xdp_redirect_ctx *ctx)
{
	return xdp_redirect_collect_stat(ctx);
}

SEC("raw_tracepoint/xdp/xdp_redirect_map_err")
int trace_xdp_redirect_map_err(struct xdp_redirect_ctx *ctx)
{
	return xdp_redirect_collect_stat(ctx);
}

/* Likely unloaded when prog starts */
SEC("raw_tracepoint/xdp/xdp_redirect")
int trace_xdp_redirect(struct xdp_redirect_ctx *ctx)
{
	return xdp_redirect_collect_stat(ctx);
}

/* Likely unloaded when prog starts */
SEC("raw_tracepoint/xdp/xdp_redirect_map")
int trace_xdp_redirect_map(struct xdp_redirect_ctx *ctx)
{
	return xdp_redirect_collect_stat(ctx);
}

/* Tracepoint format: /sys/kernel/debug/tracing/events/xdp/xdp_exception/format
 * Code in:                kernel/include/trace/events/xdp.h
 */
struct xdp_exception_ctx {
	__u64 __pad;	// First 8 bytes are not accessible by bpf code
	int prog_id;	//	offset:8;  size:4; signed:1;
	__u32 act;	//	offset:12; size:4; signed:0;
	int ifindex;	//	offset:16; size:4; signed:1;
};

SEC("raw_tracepoint/xdp/xdp_exception")
int trace_xdp_exception(struct xdp_exception_ctx *ctx)
{
	__u64 *cnt;
	__u32 key;

	key = ctx->act;
	if (key > XDP_REDIRECT)
		key = XDP_UNKNOWN;

	cnt = bpf_map_lookup_elem(&exception_cnt, &key);
	if (!cnt)
		return 1;
	*cnt += 1;

	return 0;
}

/* Common stats data record shared with _user.c */
struct datarec {
	__u64 processed;
	__u64 dropped;
	__u64 info;
	__u64 err;
};
#define MAX_CPUS 64

struct bpf_map_def SEC("maps") cpumap_enqueue_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(__u32),
	.value_size	= sizeof(struct datarec),
	.max_entries	= MAX_CPUS,
};

struct bpf_map_def SEC("maps") cpumap_kthread_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(__u32),
	.value_size	= sizeof(struct datarec),
	.max_entries	= 1,
};

/* Tracepoint: /sys/kernel/debug/tracing/events/xdp/xdp_cpumap_enqueue/format
 * Code in:         kernel/include/trace/events/xdp.h
 */
struct cpumap_enqueue_ctx {
	__u64 __pad;		// First 8 bytes are not accessible by bpf code
	int map_id;		//	offset:8;  size:4; signed:1;
	__u32 act;		//	offset:12; size:4; signed:0;
	int cpu;		//	offset:16; size:4; signed:1;
	unsigned int drops;	//	offset:20; size:4; signed:0;
	unsigned int processed;	//	offset:24; size:4; signed:0;
	int to_cpu;		//	offset:28; size:4; signed:1;
};

SEC("raw_tracepoint/xdp/xdp_cpumap_enqueue")
int trace_xdp_cpumap_enqueue(struct cpumap_enqueue_ctx *ctx)
{
	__u32 to_cpu = ctx->to_cpu;
	struct datarec *rec;

	if (to_cpu >= MAX_CPUS)
		return 1;

	rec = bpf_map_lookup_elem(&cpumap_enqueue_cnt, &to_cpu);
	if (!rec)
		return 0;
	rec->processed += ctx->processed;
	rec->dropped   += ctx->drops;

	/* Record bulk events, then userspace can calc average bulk size */
	if (ctx->processed > 0)
		rec->info += 1;

	return 0;
}

/* Tracepoint: /sys/kernel/debug/tracing/events/xdp/xdp_cpumap_kthread/format
 * Code in:         kernel/include/trace/events/xdp.h
 */
struct cpumap_kthread_ctx {
	__u64 __pad;		// First 8 bytes are not accessible by bpf code
	int map_id;		//	offset:8;  size:4; signed:1;
	__u32 act;		//	offset:12; size:4; signed:0;
	int cpu;		//	offset:16; size:4; signed:1;
	unsigned int drops;	//	offset:20; size:4; signed:0;
	unsigned int processed;	//	offset:24; size:4; signed:0;
	int sched;		//	offset:28; size:4; signed:1;
};

SEC("raw_tracepoint/xdp/xdp_cpumap_kthread")
int trace_xdp_cpumap_kthread(struct cpumap_kthread_ctx *ctx)
{
	struct datarec *rec;
	__u32 key = 0;

	rec = bpf_map_lookup_elem(&cpumap_kthread_cnt, &key);
	if (!rec)
		return 0;
	rec->processed += ctx->processed;
	rec->dropped   += ctx->drops;

	/* Count times kthread yielded CPU via schedule call */
	if (ctx->sched)
		rec->info++;

	return 0;
}

struct bpf_map_def SEC("maps") devmap_xmit_cnt = {
	.type		= BPF_MAP_TYPE_PERCPU_ARRAY,
	.key_size	= sizeof(__u32),
	.value_size	= sizeof(struct datarec),
	.max_entries	= 1,
};
BPF_ANNOTATE_KV_PAIR(devmap_xmit_cnt, int, struct datarec);

/* Tracepoint: /sys/kernel/debug/tracing/events/xdp/xdp_devmap_xmit/format
 * Code in:         kernel/include/trace/events/xdp.h
 */
struct devmap_xmit_ctx {
	__u64 __pad;		// First 8 bytes are not accessible by bpf code
	int map_id;		//	offset:8;  size:4; signed:1;
	__u32 act;		//	offset:12; size:4; signed:0;
	__u32 map_index;		//	offset:16; size:4; signed:0;
	int drops;		//	offset:20; size:4; signed:1;
	int sent;		//	offset:24; size:4; signed:1;
	int from_ifindex;	//	offset:28; size:4; signed:1;
	int to_ifindex;		//	offset:32; size:4; signed:1;
	int err;		//	offset:36; size:4; signed:1;
};

SEC("raw_tracepoint/xdp/xdp_devmap_xmit")
int trace_xdp_devmap_xmit(struct devmap_xmit_ctx *ctx)
{
	struct datarec *rec;
	__u32 key = 0;

	rec = bpf_map_lookup_elem(&devmap_xmit_cnt, &key);
	if (!rec)
		return 0;
	rec->processed += ctx->sent;
	rec->dropped   += ctx->drops;

	/* Record bulk events, then userspace can calc average bulk size */
	rec->info += 1;

	/* Record error cases, where no frame were sent */
	if (ctx->err)
		rec->err++;

	/* Catch API error of drv ndo_xdp_xmit sent more than count */
	if (ctx->drops < 0)
		rec->err++;

	return 1;
}
