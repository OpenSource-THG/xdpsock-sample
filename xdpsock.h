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

#define SOCKET_NAME "sock_cal_bpf_fd"
#define MAX_NUM_OF_CLIENTS 10

#define CLOSE_CONN  1

typedef __u64 u64;
typedef __u32 u32;

#endif /* XDPSOCK_H */
