/**
 * Based on linux/samples/bpf/xdpsock_user.c from kernel 5.19-rc4
 * Hacking our way to a better kernel
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "xdpsock.h"

#define odbpf_vdebug(fmt, args...)                                                       \
	({                                                                                   \
		char ____fmt[] = fmt;                                                            \
		bpf_trace_printk(____fmt, sizeof(____fmt), ##args);                              \
	})
#define odbpf_debug(fmt, args...) odbpf_vdebug(fmt, ##args)

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
#ifdef MULTI_FCQ

	/* In a multi-FCQ setup we lookup the rx channel ID in our xsk map */
	rr = ctx->rx_queue_index;
	if (bpf_map_lookup_elem(&xsks_map, &rr))
	{
		odbpf_debug("[MultiFCQ] Redirecting to rr=%u", rr);
		return bpf_redirect_map(&xsks_map, rr, 0);
	}

	odbpf_debug("MultiFCQ] Lookup failed on rr=%u", rr);
	return XDP_DROP;

#else

	rr = (rr + 1) & (MAX_SOCKS - 1);
	odbpf_debug("[SingleFCQ] Redirecting to rr=%u", rr);
	return bpf_redirect_map(&xsks_map, rr, XDP_DROP);

#endif
}

char _license[] SEC("license") = "Dual BSD/GPL";
