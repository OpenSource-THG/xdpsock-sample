/**
 * Based on linux/samples/bpf/xdpsock_user.c from kernel 5.19-rc4
 * Hacking our way to a better kernel
 */

// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include "xdpsock.h"

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
#else
	rr = (rr + 1) & (MAX_SOCKS - 1);
#endif

	return bpf_redirect_map(&xsks_map, rr, XDP_DROP);
}
