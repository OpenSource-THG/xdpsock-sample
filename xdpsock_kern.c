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

/* This XDP program is only needed for the XDP_SHARED_UMEM mode.
 * If you do not use this mode, libbpf can supply an XDP program for you.
 */

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, MAX_SOCKS);
	__uint(key_size, sizeof(int));
	__uint(value_size, sizeof(int));
} xsks_map SEC(".maps");

static unsigned int rr;

SEC("xdp_sock") int xdp_sock_prog(struct xdp_md *ctx)
{
	struct ethhdr *eth = (struct ethhdr *)(unsigned long)(ctx->data);
	void *data_end = (void *)(unsigned long)(ctx->data_end);

	if ((void *)eth + sizeof(struct ethhdr) > data_end)
		return XDP_ABORTED;

	/* Pass ARP so tests don't start failing due to switch issues. Note htons(arp) == 1544. */
	if (eth->h_proto == 1544)
		return XDP_PASS;
	
#ifdef MULTI_FCQ
	/* In a multi-FCQ setup we lookup the rx channel ID in our xsk map */
	rr = ctx->rx_queue_index;
#else
	/* In a single-FCQ setup we roundrobin between sockets. */
	rr = (rr + 1) & (MAX_SOCKS - 1);
#endif

	if (bpf_map_lookup_elem(&xsks_map, &rr))
	{
		odbpf_debug("[%s][%u] Redirecting to rr=%u", QTYPE, ctx->rx_queue_index, rr);
		return bpf_redirect_map(&xsks_map, rr, 0);
	}

	odbpf_debug("[%s][%u] Lookup failed on rr=%u", QTYPE, ctx->rx_queue_index, rr);
	return XDP_DROP;
}

char _license[] SEC("license") = "Dual BSD/GPL";
