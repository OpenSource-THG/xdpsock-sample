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

SEC("xdp_sock") int xdp_sock_prog(struct xdp_md *ctx)
{
	struct ethhdr *eth = (struct ethhdr *)(unsigned long)(ctx->data);
	void *data_end = (void *)(unsigned long)(ctx->data_end);

	if ((void *)eth + sizeof(struct ethhdr) > data_end)
		return XDP_ABORTED;

	/* Pass ARP so tests don't start failing due to switch issues. Note htons(arp) == 1544. */
	if (eth->h_proto == 1544)
		return XDP_PASS;
	
	odbpf_debug("DDOS: Transmitting, we do not want to send this to userspace.");
	xdp_sock_swap_src_dst_mac(eth);
	return XDP_TX;

}

char _license[] SEC("license") = "Dual BSD/GPL";
