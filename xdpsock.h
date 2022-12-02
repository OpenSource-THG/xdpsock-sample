/**
 * Based on linux/samples/bpf/xdpsock_user.c from kernel 5.19-rc4
 * Hacking our way to a better kernel
 */

/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright(c) 2019 Intel Corporation.
 */

#ifndef XDPSOCK_H_
#define XDPSOCK_H_

#ifndef MAX_SOCKS
#warning MAX_SOCKS is undefined - defaulting to 2
#define MAX_SOCKS 2
#endif

#define XDP_ACTION_MAX XDP_REDIRECT + 1

#define SOCKET_NAME "sock_cal_bpf_fd"
#define MAX_NUM_OF_CLIENTS 10

#define CLOSE_CONN  1

typedef __u64 u64;
typedef __u32 u32;

/* This is the data record stored in the map */
struct datarec {
	__u64 rx_packets;
	__u64 rx_bytes;
};

struct record {
	__u64 timestamp;
	struct datarec total; /* defined in common_kern_user.h */
};

struct stats_record {
	struct record stats[XDP_ACTION_MAX];
};

#endif /* XDPSOCK_H */
