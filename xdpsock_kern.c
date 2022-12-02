/**
 * Based on linux/samples/bpf/xdpsock_user.c from kernel 5.19-rc4
 * Hacking our way to a better kernel
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "xdpsock.h"

#include <netinet/if_ether.h>

#ifdef MULTI_FCQ
#define QTYPE "MULTI"
#else
#define QTYPE "SINGLE"
#endif

#ifdef USE_DEBUGMODE
#define odbpf_vdebug(fmt, args...)                                                       \
	({                                                                                   \
		char ____fmt[] = fmt;                                                            \
		bpf_trace_printk(____fmt, sizeof(____fmt), ##args);                              \
	})
#define odbpf_debug(fmt, args...) odbpf_vdebug(fmt, ##args)
#else
#define odbpf_debug(fmt, args...)
#endif /* USE_DEBUGMODE */

/* LLVM maps __sync_fetch_and_add() as a built-in function to the BPF atomic add
 * instruction (that is BPF_STX | BPF_XADD | BPF_W for word sizes)
 */
#ifndef lock_xadd
#define lock_xadd(ptr, val)	((void) __sync_fetch_and_add(ptr, val))
#endif

/*
 * Swaps destination and source MAC addresses inside an Ethernet header
 */
static __always_inline void
xdp_sock_swap_src_dst_mac(struct ethhdr *eth)
{
        __u8 h_tmp[6];

        __builtin_memcpy(h_tmp, eth->h_source, 6);
        __builtin_memcpy(eth->h_source, eth->h_dest, 6);
        __builtin_memcpy(eth->h_dest, h_tmp, 6);
}

/* This XDP program is only needed for the XDP_SHARED_UMEM mode.
 * If you do not use this mode, libbpf can supply an XDP program for you.
 */

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, MAX_SOCKS);
	__uint(key_size, sizeof(int));
	__uint(value_size, sizeof(int));
} xsks_map SEC(".maps");

/*
 * Declare our wss_bpfstats_map so that we can see what packets we are parsing. This
 * uses a shared structure type!
 */
struct
{
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, __u32);
	__type(value, struct datarec);
	__uint(max_entries, XDP_ACTION_MAX);
} bpfstats SEC(".maps");

static __always_inline
__u32 xdp_stats_record_action(struct xdp_md *ctx, __u32 action)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data     = (void *)(long)ctx->data;

	if (action >= XDP_ACTION_MAX)
		return XDP_ABORTED;

	/* Lookup in kernel BPF-side return pointer to actual data record */
	struct datarec *rec = bpf_map_lookup_elem(&bpfstats, &action);
	if (!rec)
		return XDP_ABORTED;

	/* Calculate packet length */
	__u64 bytes = data_end - data;

	/* BPF_MAP_TYPE_PERCPU_ARRAY returns a data record specific to current
	 * CPU and XDP hooks runs under Softirq, which makes it safe to update
	 * without atomic operations.
	 */
	rec->rx_packets++;
	rec->rx_bytes += bytes;

	return action;
}

SEC("xdp_sock") int xdp_sock_prog(struct xdp_md *ctx)
{
	struct ethhdr *eth = (struct ethhdr *)(unsigned long)(ctx->data);
	void *data_end = (void *)(unsigned long)(ctx->data_end);

	if ((void *)eth + sizeof(struct ethhdr) > data_end)
		return xdp_stats_record_action(ctx, XDP_ABORTED);

	/* Pass ARP so tests don't start failing due to switch issues. Note htons(arp) == 1544. */
	if (eth->h_proto == 1544)
		return xdp_stats_record_action(ctx, XDP_PASS);
	
	odbpf_debug("DDOS: Transmitting, we do not want to send this to userspace.");
	xdp_sock_swap_src_dst_mac(eth);
	return xdp_stats_record_action(ctx, XDP_TX);

}

char _license[] SEC("license") = "Dual BSD/GPL";
