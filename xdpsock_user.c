/**
 * Based on linux/samples/bpf/xdpsock_user.c from kernel 5.19-rc4
 * Hacking our way to a better kernel
 */

// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2018 Intel Corporation. */

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/bpf.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/limits.h>
#include <linux/udp.h>
#include <arpa/inet.h>
#include <locale.h>
#include <net/ethernet.h>
#include <netinet/ether.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#include <bpf/libbpf.h>
#include <bpf/xsk.h>
#include <bpf/bpf.h>
#include "xdpsock.h"

/* libbpf APIs for AF_XDP are deprecated starting from v0.7 */
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

/***************** THG bits ******************/

/* Declare the bpf kernel name we will try to load. */
#ifdef XDPSOCK_KRNL
static const char *xdpsock_krnl = XDPSOCK_KRNL;
#else
static const char *xdpsock_krnl = "xdpsock_kern.bpf";
#endif

/* Some bits of this code are modified from the original in order to better
 * fit the THG changes for kernel validation. However, if you prefer to build
 * with "original bits" anyway, define this. You may get compiler warnings.
 */
#undef USE_ORIGINAL

/************** end of THG bits ***************/

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef PF_XDP
#define PF_XDP AF_XDP
#endif

#define NUM_FRAMES (4 * 1024)
#define MIN_PKT_SIZE 64

#define DEBUG_HEXDUMP 0

#define VLAN_PRIO_MASK		0xe000 /* Priority Code Point */
#define VLAN_PRIO_SHIFT		13
#define VLAN_VID_MASK		0x0fff /* VLAN Identifier */
#define VLAN_VID__DEFAULT	1
#define VLAN_PRI__DEFAULT	0

#define NSEC_PER_SEC		1000000000UL
#define NSEC_PER_USEC		1000

#define SCHED_PRI__DEFAULT	0

typedef __u64 u64;
typedef __u32 u32;
typedef __u16 u16;
typedef __u8  u8;

static unsigned long prev_time;
static long tx_cycle_diff_min;
static long tx_cycle_diff_max;
static double tx_cycle_diff_ave;
static long tx_cycle_cnt;

enum benchmark_type {
	BENCH_RXDROP = 0,
	BENCH_TXONLY = 1,
	BENCH_L2FWD = 2,
};

static enum benchmark_type opt_bench = BENCH_RXDROP;
static u32 opt_xdp_flags = XDP_FLAGS_UPDATE_IF_NOEXIST;
static const char *opt_if = "";
static int opt_ifindex;
static int opt_queue;
static unsigned long opt_duration;
static unsigned long start_time;
static bool benchmark_done;
static u32 opt_batch_size = 64;
static int opt_pkt_count;
static u16 opt_pkt_size = MIN_PKT_SIZE;
static u32 opt_pkt_fill_pattern = 0x12345678;
static bool opt_vlan_tag;
static u16 opt_pkt_vlan_id = VLAN_VID__DEFAULT;
static u16 opt_pkt_vlan_pri = VLAN_PRI__DEFAULT;
static struct ether_addr opt_txdmac = {{ 0x3c, 0xfd, 0xfe,
					 0x9e, 0x7f, 0x71 }};
static struct ether_addr opt_txsmac = {{ 0xec, 0xb1, 0xd7,
					 0x98, 0x3a, 0xc0 }};
static bool opt_extra_stats;
static bool opt_quiet;
static bool opt_app_stats;
static const char *opt_irq_str = "";
static u32 irq_no;
static int irqs_at_init = -1;
static u32 sequence;
static int opt_poll;
static int opt_interval = 1;
static int bpfstats_fd;
static int opt_retries = 3;
static u32 opt_xdp_bind_flags = XDP_USE_NEED_WAKEUP;
static u32 opt_umem_flags;
static int opt_unaligned_chunks;
static int opt_mmap_flags;
static int opt_xsk_frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;
static int opt_timeout = 1000;
static bool opt_need_wakeup = true;
static u32 opt_num_xsks = 1;
static u32 prog_id;
static bool opt_busy_poll;
static bool opt_reduced_cap;
static clockid_t opt_clock = CLOCK_MONOTONIC;
static unsigned long opt_tx_cycle_ns;
static int opt_schpolicy = SCHED_OTHER;
static int opt_schprio = SCHED_PRI__DEFAULT;
static bool opt_tstamp;
static struct stats_record prev, record = { 0 };

struct vlan_ethhdr {
	unsigned char h_dest[6];
	unsigned char h_source[6];
	__be16 h_vlan_proto;
	__be16 h_vlan_TCI;
	__be16 h_vlan_encapsulated_proto;
};

#define PKTGEN_MAGIC 0xbe9be955
struct pktgen_hdr {
	__be32 pgh_magic;
	__be32 seq_num;
	__be32 tv_sec;
	__be32 tv_usec;
};

struct xsk_ring_stats {
	unsigned long rx_npkts;
	unsigned long tx_npkts;
	unsigned long rx_dropped_npkts;
	unsigned long rx_invalid_npkts;
	unsigned long tx_invalid_npkts;
	unsigned long rx_full_npkts;
	unsigned long rx_fill_empty_npkts;
	unsigned long tx_empty_npkts;
	unsigned long prev_rx_npkts;
	unsigned long prev_tx_npkts;
	unsigned long prev_rx_dropped_npkts;
	unsigned long prev_rx_invalid_npkts;
	unsigned long prev_tx_invalid_npkts;
	unsigned long prev_rx_full_npkts;
	unsigned long prev_rx_fill_empty_npkts;
	unsigned long prev_tx_empty_npkts;
};

struct xsk_driver_stats {
	unsigned long intrs;
	unsigned long prev_intrs;
};

struct xsk_app_stats {
	unsigned long rx_empty_polls;
	unsigned long fill_fail_polls;
	unsigned long copy_tx_sendtos;
	unsigned long tx_wakeup_sendtos;
	unsigned long opt_polls;
	unsigned long prev_rx_empty_polls;
	unsigned long prev_fill_fail_polls;
	unsigned long prev_copy_tx_sendtos;
	unsigned long prev_tx_wakeup_sendtos;
	unsigned long prev_opt_polls;
};

struct xsk_umem_info {
	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	struct xsk_umem *umem;
	void *buffer;
};

struct xsk_socket_info {
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;

	u32 channel_id; /**< Channel ID of this xsk */
	u32 xsk_index; /**< Index of this xsk within xsks */
	
#ifdef MULTI_FCQ

	struct xsk_ring_prod fq; /**< Dedicated fill queue */
	struct xsk_ring_cons cq; /**< Dedicated comp queue */
	u64 umem_offset; /**< Umem offset of descriptors for this XSK */

#endif /* MULTI_FCQ */

	struct xsk_umem_info *umem;
	struct xsk_socket *xsk;
	struct xsk_ring_stats ring_stats;
	struct xsk_app_stats app_stats;
	struct xsk_driver_stats drv_stats;
	u32 outstanding_tx;
};

static const struct clockid_map {
	const char *name;
	clockid_t clockid;
} clockids_map[] = {
	{ "REALTIME", CLOCK_REALTIME },
	{ "TAI", CLOCK_TAI },
	{ "BOOTTIME", CLOCK_BOOTTIME },
	{ "MONOTONIC", CLOCK_MONOTONIC },
	{ NULL }
};

static const struct sched_map {
	const char *name;
	int policy;
} schmap[] = {
	{ "OTHER", SCHED_OTHER },
	{ "FIFO", SCHED_FIFO },
	{ NULL }
};

static int num_socks = 0;
struct xsk_socket_info *xsks[MAX_SOCKS];
int sock;

static int get_clockid(clockid_t *id, const char *name)
{
	const struct clockid_map *clk;

	for (clk = clockids_map; clk->name; clk++) {
		if (strcasecmp(clk->name, name) == 0) {
			*id = clk->clockid;
			return 0;
		}
	}

	return -1;
}

static int get_schpolicy(int *policy, const char *name)
{
	const struct sched_map *sch;

	for (sch = schmap; sch->name; sch++) {
		if (strcasecmp(sch->name, name) == 0) {
			*policy = sch->policy;
			return 0;
		}
	}

	return -1;
}

static unsigned long get_nsecs(void)
{
	struct timespec ts;

	clock_gettime(opt_clock, &ts);
	return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

static void print_benchmark(struct xsk_socket_info *xsk, bool running)
{
	const char *bench_str = "INVALID";

	if (opt_bench == BENCH_RXDROP)
		bench_str = "rxdrop";
	else if (opt_bench == BENCH_TXONLY)
		bench_str = "txonly";
	else if (opt_bench == BENCH_L2FWD)
		bench_str = "l2fwd";

	printf("%s:%u %s ", opt_if, xsk->channel_id, bench_str);
	if (opt_xdp_flags & XDP_FLAGS_SKB_MODE)
		printf("xdp-skb ");
	else if (opt_xdp_flags & XDP_FLAGS_DRV_MODE)
		printf("xdp-drv ");
	else
		printf("	");

	if (opt_poll)
		printf("poll() ");

	if (running) {
		printf("running...");
		fflush(stdout);
	}
}

static int xsk_get_xdp_stats(int fd, struct xsk_socket_info *xsk)
{
	struct xdp_statistics stats;
	socklen_t optlen;
	int err;

	optlen = sizeof(stats);
	err = getsockopt(fd, SOL_XDP, XDP_STATISTICS, &stats, &optlen);
	if (err)
		return err;

	if (optlen == sizeof(struct xdp_statistics)) {
		xsk->ring_stats.rx_dropped_npkts = stats.rx_dropped;
		xsk->ring_stats.rx_invalid_npkts = stats.rx_invalid_descs;
		xsk->ring_stats.tx_invalid_npkts = stats.tx_invalid_descs;
		xsk->ring_stats.rx_full_npkts = stats.rx_ring_full;
		xsk->ring_stats.rx_fill_empty_npkts = stats.rx_fill_ring_empty_descs;
		xsk->ring_stats.tx_empty_npkts = stats.tx_ring_empty_descs;
		return 0;
	}

	return -EINVAL;
}

static void dump_app_stats(long dt)
{
	int i;

	for (i = 0; i < num_socks && xsks[i]; i++) {
		char *fmt = "%-18s %'-14.0f %'-14lu\n";
		double rx_empty_polls_ps, fill_fail_polls_ps, copy_tx_sendtos_ps,
				tx_wakeup_sendtos_ps, opt_polls_ps;

		rx_empty_polls_ps = (xsks[i]->app_stats.rx_empty_polls -
					xsks[i]->app_stats.prev_rx_empty_polls) * 1000000000. / dt;
		fill_fail_polls_ps = (xsks[i]->app_stats.fill_fail_polls -
					xsks[i]->app_stats.prev_fill_fail_polls) * 1000000000. / dt;
		copy_tx_sendtos_ps = (xsks[i]->app_stats.copy_tx_sendtos -
					xsks[i]->app_stats.prev_copy_tx_sendtos) * 1000000000. / dt;
		tx_wakeup_sendtos_ps = (xsks[i]->app_stats.tx_wakeup_sendtos -
					xsks[i]->app_stats.prev_tx_wakeup_sendtos)
										* 1000000000. / dt;
		opt_polls_ps = (xsks[i]->app_stats.opt_polls -
					xsks[i]->app_stats.prev_opt_polls) * 1000000000. / dt;

		printf("\n%-18s %-14s %-14s\n", "", "calls/s", "count");
		printf(fmt, "rx empty polls", rx_empty_polls_ps, xsks[i]->app_stats.rx_empty_polls);
		printf(fmt, "fill fail polls", fill_fail_polls_ps,
							xsks[i]->app_stats.fill_fail_polls);
		printf(fmt, "copy tx sendtos", copy_tx_sendtos_ps,
							xsks[i]->app_stats.copy_tx_sendtos);
		printf(fmt, "tx wakeup sendtos", tx_wakeup_sendtos_ps,
							xsks[i]->app_stats.tx_wakeup_sendtos);
		printf(fmt, "opt polls", opt_polls_ps, xsks[i]->app_stats.opt_polls);

		xsks[i]->app_stats.prev_rx_empty_polls = xsks[i]->app_stats.rx_empty_polls;
		xsks[i]->app_stats.prev_fill_fail_polls = xsks[i]->app_stats.fill_fail_polls;
		xsks[i]->app_stats.prev_copy_tx_sendtos = xsks[i]->app_stats.copy_tx_sendtos;
		xsks[i]->app_stats.prev_tx_wakeup_sendtos = xsks[i]->app_stats.tx_wakeup_sendtos;
		xsks[i]->app_stats.prev_opt_polls = xsks[i]->app_stats.opt_polls;
	}

	if (opt_tx_cycle_ns) {
		printf("\n%-18s %-10s %-10s %-10s %-10s %-10s\n",
		       "", "period", "min", "ave", "max", "cycle");
		printf("%-18s %-10lu %-10lu %-10lu %-10lu %-10lu\n",
		       "Cyclic TX", opt_tx_cycle_ns, tx_cycle_diff_min,
		       (long)(tx_cycle_diff_ave / tx_cycle_cnt),
		       tx_cycle_diff_max, tx_cycle_cnt);
	}
}

static bool get_interrupt_number(void)
{
	FILE *f_int_proc;
	char line[4096];
	bool found = false;

	f_int_proc = fopen("/proc/interrupts", "r");
	if (f_int_proc == NULL) {
		printf("Failed to open /proc/interrupts.\n");
		return found;
	}

	while (!feof(f_int_proc) && !found) {
		/* Make sure to read a full line at a time */
		if (fgets(line, sizeof(line), f_int_proc) == NULL ||
				line[strlen(line) - 1] != '\n') {
			printf("Error reading from interrupts file\n");
			break;
		}

		/* Extract interrupt number from line */
		if (strstr(line, opt_irq_str) != NULL) {
			irq_no = atoi(line);
			found = true;
			break;
		}
	}

	fclose(f_int_proc);

	return found;
}

static int get_irqs(void)
{
	char count_path[PATH_MAX];
	int total_intrs = -1;
	FILE *f_count_proc;
	char line[4096];

	snprintf(count_path, sizeof(count_path),
		"/sys/kernel/irq/%i/per_cpu_count", irq_no);
	f_count_proc = fopen(count_path, "r");
	if (f_count_proc == NULL) {
		printf("Failed to open %s\n", count_path);
		return total_intrs;
	}

	if (fgets(line, sizeof(line), f_count_proc) == NULL ||
			line[strlen(line) - 1] != '\n') {
		printf("Error reading from %s\n", count_path);
	} else {
		static const char com[2] = ",";
		char *token;

		total_intrs = 0;
		token = strtok(line, com);
		while (token != NULL) {
			/* sum up interrupts across all cores */
			total_intrs += atoi(token);
			token = strtok(NULL, com);
		}
	}

	fclose(f_count_proc);

	return total_intrs;
}

static void dump_driver_stats(long dt)
{
	int i;

	for (i = 0; i < num_socks && xsks[i]; i++) {
		char *fmt = "%-18s %'-14.0f %'-14lu\n";
		double intrs_ps;
		int n_ints = get_irqs();

		if (n_ints < 0) {
			printf("error getting intr info for intr %i\n", irq_no);
			return;
		}
		xsks[i]->drv_stats.intrs = n_ints - irqs_at_init;

		intrs_ps = (xsks[i]->drv_stats.intrs - xsks[i]->drv_stats.prev_intrs) *
			 1000000000. / dt;

		printf("\n%-18s %-14s %-14s\n", "", "intrs/s", "count");
		printf(fmt, "irqs", intrs_ps, xsks[i]->drv_stats.intrs);

		xsks[i]->drv_stats.prev_intrs = xsks[i]->drv_stats.intrs;
	}
}

#define NANOSEC_PER_SEC 1000000000 /* 10^9 */
static __u64 gettime(void)
{
	struct timespec t;
	int res;

	res = clock_gettime(CLOCK_MONOTONIC, &t);
	if (res < 0) {
		fprintf(stderr, "Error with gettimeofday! (%i)\n", res);
		exit(1);
	}
	return (__u64) t.tv_sec * NANOSEC_PER_SEC + t.tv_nsec;
}

static const char *xdp_action_names[XDP_ACTION_MAX] = {
	[XDP_ABORTED]   = "XDP_ABORTED",
	[XDP_DROP]      = "XDP_DROP",
	[XDP_PASS]      = "XDP_PASS",
	[XDP_TX]        = "XDP_TX",
	[XDP_REDIRECT]  = "XDP_REDIRECT",
};

const char *action2str(__u32 action)
{
        if (action < XDP_ACTION_MAX)
                return xdp_action_names[action];
        return NULL;
}

static void stats_print(struct stats_record *stats_rec,
			struct stats_record *stats_prev)
{
	struct record *rec, *prev;
	__u64 packets, bytes;
	double pps; /* packets per sec */
	double bps; /* bits per sec */
	int i;

	/* Print stats "header" */
	printf("%-12s\n", "XDP-action");

	/* Print for each XDP actions stats */
	for (i = 0; i < XDP_ACTION_MAX; i++)
	{
		char *fmt = "%-12s %'11lld pkts (%'10.0f pps)"
			" %'11lld Kbytes (%'6.0f Mbits/s)\n";
		const char *action = action2str(i);

		rec  = &stats_rec->stats[i];
		prev = &stats_prev->stats[i];

		packets = rec->total.rx_packets - prev->total.rx_packets;
		pps     = packets;

		bytes   = rec->total.rx_bytes   - prev->total.rx_bytes;
		bps     = (bytes * 8) / 1000000;

		printf(fmt, action, rec->total.rx_packets, pps,
		       rec->total.rx_bytes / 1000 , bps);
	}
	printf("\n");
}

/* BPF_MAP_TYPE_PERCPU_ARRAY */
void map_get_value_percpu_array(int fd, __u32 key, struct datarec *value)
{
	/* For percpu maps, userspace gets a value per possible CPU */
	unsigned int nr_cpus = libbpf_num_possible_cpus();
	struct datarec values[nr_cpus];
	__u64 sum_bytes = 0;
	__u64 sum_pkts = 0;
	int i;

	if ((bpf_map_lookup_elem(fd, &key, values)) != 0) {
		fprintf(stderr,
			"ERR: bpf_map_lookup_elem failed key:0x%X\n", key);
		return;
	}

	/* Sum values from each CPU */
	for (i = 0; i < nr_cpus; i++) {
		sum_pkts  += values[i].rx_packets;
		sum_bytes += values[i].rx_bytes;
	}
	value->rx_packets = sum_pkts;
	value->rx_bytes   = sum_bytes;
}

static bool map_collect(int fd, __u32 map_type, __u32 key, struct record *rec)
{
	struct datarec value;

	/* Get time as close as possible to reading map contents */
	rec->timestamp = gettime();

	switch (map_type) {
	case BPF_MAP_TYPE_PERCPU_ARRAY:
		map_get_value_percpu_array(fd, key, &value);
		break;
	default:
		fprintf(stderr, "ERR: Unknown map_type(%u) cannot handle\n",
			map_type);
		return false;
		break;
	}

	rec->total.rx_packets = value.rx_packets;
	rec->total.rx_bytes   = value.rx_bytes;
	return true;
}

static void dump_stats(void)
{
	unsigned long now = get_nsecs();
	long dt = now - prev_time;
	int i;

	prev_time = now;

	fprintf(stdout, "----------------------------------------------------------------------\n");
#ifdef USE_DEBUGMODE
	fprintf(stdout, " *** WARNING - Debugmode is enabled - performance may not be good ***\n");
#endif

	prev = record;
	for (unsigned int key = 0; key < XDP_ACTION_MAX; key++)
		map_collect(bpfstats_fd, BPF_MAP_TYPE_PERCPU_ARRAY, key, &record.stats[key]);
	stats_print(&record, &prev);

	for (i = 0; i < num_socks && xsks[i]; i++) {
		char *fmt = "%-18s %'-14.0f %'-14lu\n";
		double rx_pps, tx_pps, dropped_pps, rx_invalid_pps, full_pps, fill_empty_pps,
			tx_invalid_pps, tx_empty_pps;

		rx_pps = (xsks[i]->ring_stats.rx_npkts - xsks[i]->ring_stats.prev_rx_npkts) *
			 1000000000. / dt;
		tx_pps = (xsks[i]->ring_stats.tx_npkts - xsks[i]->ring_stats.prev_tx_npkts) *
			 1000000000. / dt;

		printf("\n sock%d@", i);
		print_benchmark(xsks[i], false);
		printf("\n");

		printf("%-18s %-14s %-14s %-14.2f\n", "", "pps", "pkts",
		       dt / 1000000000.);
		printf(fmt, "rx", rx_pps, xsks[i]->ring_stats.rx_npkts);
		printf(fmt, "tx", tx_pps, xsks[i]->ring_stats.tx_npkts);

		xsks[i]->ring_stats.prev_rx_npkts = xsks[i]->ring_stats.rx_npkts;
		xsks[i]->ring_stats.prev_tx_npkts = xsks[i]->ring_stats.tx_npkts;

		if (opt_extra_stats) {
			if (!xsk_get_xdp_stats(xsk_socket__fd(xsks[i]->xsk), xsks[i])) {
				dropped_pps = (xsks[i]->ring_stats.rx_dropped_npkts -
						xsks[i]->ring_stats.prev_rx_dropped_npkts) *
							1000000000. / dt;
				rx_invalid_pps = (xsks[i]->ring_stats.rx_invalid_npkts -
						xsks[i]->ring_stats.prev_rx_invalid_npkts) *
							1000000000. / dt;
				tx_invalid_pps = (xsks[i]->ring_stats.tx_invalid_npkts -
						xsks[i]->ring_stats.prev_tx_invalid_npkts) *
							1000000000. / dt;
				full_pps = (xsks[i]->ring_stats.rx_full_npkts -
						xsks[i]->ring_stats.prev_rx_full_npkts) *
							1000000000. / dt;
				fill_empty_pps = (xsks[i]->ring_stats.rx_fill_empty_npkts -
						xsks[i]->ring_stats.prev_rx_fill_empty_npkts) *
							1000000000. / dt;
				tx_empty_pps = (xsks[i]->ring_stats.tx_empty_npkts -
						xsks[i]->ring_stats.prev_tx_empty_npkts) *
							1000000000. / dt;

				printf(fmt, "rx dropped", dropped_pps,
				       xsks[i]->ring_stats.rx_dropped_npkts);
				printf(fmt, "rx invalid", rx_invalid_pps,
				       xsks[i]->ring_stats.rx_invalid_npkts);
				printf(fmt, "tx invalid", tx_invalid_pps,
				       xsks[i]->ring_stats.tx_invalid_npkts);
				printf(fmt, "rx queue full", full_pps,
				       xsks[i]->ring_stats.rx_full_npkts);
				printf(fmt, "fill ring empty", fill_empty_pps,
				       xsks[i]->ring_stats.rx_fill_empty_npkts);
				printf(fmt, "tx ring empty", tx_empty_pps,
				       xsks[i]->ring_stats.tx_empty_npkts);

				xsks[i]->ring_stats.prev_rx_dropped_npkts =
					xsks[i]->ring_stats.rx_dropped_npkts;
				xsks[i]->ring_stats.prev_rx_invalid_npkts =
					xsks[i]->ring_stats.rx_invalid_npkts;
				xsks[i]->ring_stats.prev_tx_invalid_npkts =
					xsks[i]->ring_stats.tx_invalid_npkts;
				xsks[i]->ring_stats.prev_rx_full_npkts =
					xsks[i]->ring_stats.rx_full_npkts;
				xsks[i]->ring_stats.prev_rx_fill_empty_npkts =
					xsks[i]->ring_stats.rx_fill_empty_npkts;
				xsks[i]->ring_stats.prev_tx_empty_npkts =
					xsks[i]->ring_stats.tx_empty_npkts;
			} else {
				printf("%-15s\n", "Error retrieving extra stats");
			}
		}
	}

	if (opt_app_stats)
		dump_app_stats(dt);
	if (irq_no)
		dump_driver_stats(dt);
}

static bool is_benchmark_done(void)
{
	if (opt_duration > 0) {
		unsigned long dt = (get_nsecs() - start_time);

		if (dt >= opt_duration)
			benchmark_done = true;
	}
	return benchmark_done;
}

static void *poller(void *arg)
{
	(void)arg;
	while (!is_benchmark_done()) {
		sleep(opt_interval);
		dump_stats();
	}

	return NULL;
}

static void remove_xdp_program(void)
{
	u32 curr_prog_id = 0;

	if (bpf_xdp_query_id(opt_ifindex, opt_xdp_flags, &curr_prog_id)) {
		printf("bpf_xdp_query_id failed\n");
		exit(EXIT_FAILURE);
	}

	if (prog_id == curr_prog_id)
		bpf_xdp_detach(opt_ifindex, opt_xdp_flags, NULL);
	else if (!curr_prog_id)
		printf("couldn't find a prog id on a given interface\n");
	else
		printf("program on interface changed, not removing\n");
}

static void int_exit(int sig)
{
	benchmark_done = true;
}

static void __exit_with_error(int error, const char *file, const char *func,
			      int line)
{
	fprintf(stderr, "%s:%s:%i: errno: %d/\"%s\"\n", file, func,
		line, error, strerror(error));

#ifndef MULTI_FCQ
	/* In single FCQ mode, we only have an XDP program laoded if num_xsks > 1. */
	if (opt_num_xsks > 1)
#endif
		remove_xdp_program();
	exit(EXIT_FAILURE);
}

#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, __LINE__)

static void xdpsock_cleanup(void)
{
	struct xsk_umem *umem = xsks[0]->umem->umem;
	int i, cmd = CLOSE_CONN;

	dump_stats();
	for (i = 0; i < num_socks; i++)
		xsk_socket__delete(xsks[i]->xsk);
	(void)xsk_umem__delete(umem);

	if (opt_reduced_cap) {
		if (write(sock, &cmd, sizeof(int)) < 0)
			exit_with_error(errno);
	}

#ifndef MULTI_FCQ
	/* In single FCQ mode, we only have an XDP program laoded if num_xsks > 1. */
	if (opt_num_xsks > 1)
#endif
		remove_xdp_program();
}

static void swap_mac_addresses(void *data)
{
	struct ether_header *eth = (struct ether_header *)data;
	struct ether_addr *src_addr = (struct ether_addr *)&eth->ether_shost;
	struct ether_addr *dst_addr = (struct ether_addr *)&eth->ether_dhost;
	struct ether_addr tmp;

	tmp = *src_addr;
	*src_addr = *dst_addr;
	*dst_addr = tmp;
}

static void hex_dump(void *pkt, size_t length, u64 addr)
{
	const unsigned char *address = (unsigned char *)pkt;
	const unsigned char *line = address;
	size_t line_size = 32;
	unsigned char c;
	char buf[32];
	int i = 0;

	if (!DEBUG_HEXDUMP)
		return;

	sprintf(buf, "addr=%llu", addr);
	printf("length = %zu\n", length);
	printf("%s | ", buf);
	while (length-- > 0) {
		printf("%02X ", *address++);
		if (!(++i % line_size) || (length == 0 && i % line_size)) {
			if (length == 0) {
				while (i++ % line_size)
					printf("__ ");
			}
			printf(" | ");	/* right close */
			while (line < address) {
				c = *line++;
				printf("%c", (c < 33 || c == 255) ? 0x2E : c);
			}
			printf("\n");
			if (length > 0)
				printf("%s | ", buf);
		}
	}
	printf("\n");
}

static void *memset32_htonl(void *dest, u32 val, u32 size)
{
	u32 *ptr = (u32 *)dest;
	int i;

	val = htonl(val);

	for (i = 0; i < (size & (~0x3)); i += 4)
		ptr[i >> 2] = val;

	for (; i < size; i++)
		((char *)dest)[i] = ((char *)&val)[i & 3];

	return dest;
}

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
static inline unsigned short from32to16(unsigned int x)
{
	/* add up 16-bit and 16-bit for 16+c bit */
	x = (x & 0xffff) + (x >> 16);
	/* add up carry.. */
	x = (x & 0xffff) + (x >> 16);
	return x;
}

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
static unsigned int do_csum(const unsigned char *buff, int len)
{
	unsigned int result = 0;
	int odd;

	if (len <= 0)
		goto out;
	odd = 1 & (unsigned long)buff;
	if (odd) {
#ifdef __LITTLE_ENDIAN
		result += (*buff << 8);
#else
		result = *buff;
#endif
		len--;
		buff++;
	}
	if (len >= 2) {
		if (2 & (unsigned long)buff) {
			result += *(unsigned short *)buff;
			len -= 2;
			buff += 2;
		}
		if (len >= 4) {
			const unsigned char *end = buff +
						   ((unsigned int)len & ~3);
			unsigned int carry = 0;

			do {
				unsigned int w = *(unsigned int *)buff;

				buff += 4;
				result += carry;
				result += w;
				carry = (w > result);
			} while (buff < end);
			result += carry;
			result = (result & 0xffff) + (result >> 16);
		}
		if (len & 2) {
			result += *(unsigned short *)buff;
			buff += 2;
		}
	}
	if (len & 1)
#ifdef __LITTLE_ENDIAN
		result += *buff;
#else
		result += (*buff << 8);
#endif
	result = from32to16(result);
	if (odd)
		result = ((result >> 8) & 0xff) | ((result & 0xff) << 8);
out:
	return result;
}

/*
 *	This is a version of ip_compute_csum() optimized for IP headers,
 *	which always checksum on 4 octet boundaries.
 *	This function code has been taken from
 *	Linux kernel lib/checksum.c
 */
static inline __sum16 ip_fast_csum(const void *iph, unsigned int ihl)
{
	return (__sum16)~do_csum(iph, ihl * 4);
}

/*
 * Fold a partial checksum
 * This function code has been taken from
 * Linux kernel include/asm-generic/checksum.h
 */
static inline __sum16 csum_fold(__wsum csum)
{
	u32 sum = (u32)csum;

	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return (__sum16)~sum;
}

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
static inline u32 from64to32(u64 x)
{
	/* add up 32-bit and 32-bit for 32+c bit */
	x = (x & 0xffffffff) + (x >> 32);
	/* add up carry.. */
	x = (x & 0xffffffff) + (x >> 32);
	return (u32)x;
}

__wsum csum_tcpudp_nofold(__be32 saddr, __be32 daddr,
			  __u32 len, __u8 proto, __wsum sum);

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
__wsum csum_tcpudp_nofold(__be32 saddr, __be32 daddr,
			  __u32 len, __u8 proto, __wsum sum)
{
	unsigned long long s = (u32)sum;

	s += (u32)saddr;
	s += (u32)daddr;
#ifdef __BIG_ENDIAN__
	s += proto + len;
#else
	s += (proto + len) << 8;
#endif
	return (__wsum)from64to32(s);
}

/*
 * This function has been taken from
 * Linux kernel include/asm-generic/checksum.h
 */
static inline __sum16
csum_tcpudp_magic(__be32 saddr, __be32 daddr, __u32 len,
		  __u8 proto, __wsum sum)
{
	return csum_fold(csum_tcpudp_nofold(saddr, daddr, len, proto, sum));
}

static inline u16 udp_csum(u32 saddr, u32 daddr, u32 len,
			   u8 proto, u16 *udp_pkt)
{
	u32 csum = 0;
	u32 cnt = 0;

	/* udp hdr and data */
	for (; cnt < len; cnt += 2)
		csum += udp_pkt[cnt >> 1];

	return csum_tcpudp_magic(saddr, daddr, len, proto, csum);
}

#define ETH_FCS_SIZE 4

#define ETH_HDR_SIZE (opt_vlan_tag ? sizeof(struct vlan_ethhdr) : \
		      sizeof(struct ethhdr))
#define PKTGEN_HDR_SIZE (opt_tstamp ? sizeof(struct pktgen_hdr) : 0)
#define PKT_HDR_SIZE (ETH_HDR_SIZE + sizeof(struct iphdr) + \
		      sizeof(struct udphdr) + PKTGEN_HDR_SIZE)
#define PKTGEN_HDR_OFFSET (ETH_HDR_SIZE + sizeof(struct iphdr) + \
			   sizeof(struct udphdr))
#define PKTGEN_SIZE_MIN (PKTGEN_HDR_OFFSET + sizeof(struct pktgen_hdr) + \
			 ETH_FCS_SIZE)

#define PKT_SIZE		(opt_pkt_size - ETH_FCS_SIZE)
#define IP_PKT_SIZE		(PKT_SIZE - ETH_HDR_SIZE)
#define UDP_PKT_SIZE		(IP_PKT_SIZE - sizeof(struct iphdr))
#define UDP_PKT_DATA_SIZE	(UDP_PKT_SIZE - \
				 (sizeof(struct udphdr) + PKTGEN_HDR_SIZE))

static u8 pkt_data[XSK_UMEM__DEFAULT_FRAME_SIZE];

static void gen_eth_hdr_data(void)
{
	struct pktgen_hdr *pktgen_hdr;
	struct udphdr *udp_hdr;
	struct iphdr *ip_hdr;

	if (opt_vlan_tag) {
		struct vlan_ethhdr *veth_hdr = (struct vlan_ethhdr *)pkt_data;
		u16 vlan_tci = 0;

		udp_hdr = (struct udphdr *)(pkt_data +
					    sizeof(struct vlan_ethhdr) +
					    sizeof(struct iphdr));
		ip_hdr = (struct iphdr *)(pkt_data +
					  sizeof(struct vlan_ethhdr));
		pktgen_hdr = (struct pktgen_hdr *)(pkt_data +
						   sizeof(struct vlan_ethhdr) +
						   sizeof(struct iphdr) +
						   sizeof(struct udphdr));
		/* ethernet & VLAN header */
		memcpy(veth_hdr->h_dest, &opt_txdmac, ETH_ALEN);
		memcpy(veth_hdr->h_source, &opt_txsmac, ETH_ALEN);
		veth_hdr->h_vlan_proto = htons(ETH_P_8021Q);
		vlan_tci = opt_pkt_vlan_id & VLAN_VID_MASK;
		vlan_tci |= (opt_pkt_vlan_pri << VLAN_PRIO_SHIFT) & VLAN_PRIO_MASK;
		veth_hdr->h_vlan_TCI = htons(vlan_tci);
		veth_hdr->h_vlan_encapsulated_proto = htons(ETH_P_IP);
	} else {
		struct ethhdr *eth_hdr = (struct ethhdr *)pkt_data;

		udp_hdr = (struct udphdr *)(pkt_data +
					    sizeof(struct ethhdr) +
					    sizeof(struct iphdr));
		ip_hdr = (struct iphdr *)(pkt_data +
					  sizeof(struct ethhdr));
		pktgen_hdr = (struct pktgen_hdr *)(pkt_data +
						   sizeof(struct ethhdr) +
						   sizeof(struct iphdr) +
						   sizeof(struct udphdr));
		/* ethernet header */
		memcpy(eth_hdr->h_dest, &opt_txdmac, ETH_ALEN);
		memcpy(eth_hdr->h_source, &opt_txsmac, ETH_ALEN);
		eth_hdr->h_proto = htons(ETH_P_IP);
	}


	/* IP header */
	ip_hdr->version = IPVERSION;
	ip_hdr->ihl = 0x5; /* 20 byte header */
	ip_hdr->tos = 0x0;
	ip_hdr->tot_len = htons(IP_PKT_SIZE);
	ip_hdr->id = 0;
	ip_hdr->frag_off = 0;
	ip_hdr->ttl = IPDEFTTL;
	ip_hdr->protocol = IPPROTO_UDP;
	ip_hdr->saddr = htonl(0x0a0a0a10);
	ip_hdr->daddr = htonl(0x0a0a0a20);

	/* IP header checksum */
	ip_hdr->check = 0;
	ip_hdr->check = ip_fast_csum((const void *)ip_hdr, ip_hdr->ihl);

	/* UDP header */
	udp_hdr->source = htons(0x1000);
	udp_hdr->dest = htons(0x1000);
	udp_hdr->len = htons(UDP_PKT_SIZE);

	if (opt_tstamp)
		pktgen_hdr->pgh_magic = htonl(PKTGEN_MAGIC);

	/* UDP data */
	memset32_htonl(pkt_data + PKT_HDR_SIZE, opt_pkt_fill_pattern,
		       UDP_PKT_DATA_SIZE);

	/* UDP header checksum */
	udp_hdr->check = 0;
	udp_hdr->check = udp_csum(ip_hdr->saddr, ip_hdr->daddr, UDP_PKT_SIZE,
				  IPPROTO_UDP, (u16 *)udp_hdr);
}

static void gen_eth_frame(struct xsk_umem_info *umem, u64 addr)
{
	memcpy(xsk_umem__get_data(umem->buffer, addr), pkt_data,
	       PKT_SIZE);
}

static struct xsk_umem_info *xsk_configure_umem(void *buffer, u64 size)
{
	struct xsk_umem_info *umem;
	struct xsk_umem_config cfg = {
		/* We recommend that you set the fill ring size >= HW RX ring size +
		 * AF_XDP RX ring size. Make sure you fill up the fill ring
		 * with buffers at regular intervals, and you will with this setting
		 * avoid allocation failures in the driver. These are usually quite
		 * expensive since drivers have not been written to assume that
		 * allocation failures are common. For regular sockets, kernel
		 * allocated memory is used that only runs out in OOM situations
		 * that should be rare.
		 */
		.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS * 2,
		.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
		.frame_size = opt_xsk_frame_size,
		.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
		.flags = opt_umem_flags
	};
	int ret;

	umem = calloc(1, sizeof(*umem));
	if (!umem)
		exit_with_error(errno);

	ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq,
			       &cfg);
	if (ret)
		exit_with_error(-ret);

	umem->buffer = buffer;
	return umem;
}

static void xsk_populate_fill_ring(struct xsk_umem_info *umem, struct xsk_socket_info *xsk)
{
	int ret, i;
	u32 idx;

#ifdef MULTI_FCQ
	if (umem == NULL || xsk == NULL)
		exit_with_error(-EINVAL);

	fprintf(stdout, "Filling multi-FCQ XSK[%u] from umem_offset:%llu\n",
		xsk->xsk_index, xsk->umem_offset);

	/* Multi FCQ mode, we fill the xsk->fq. */
	struct xsk_ring_prod *fq_ptr = &xsk->fq;

	/* In a multi-FCQ setup, umem size is multiplied by the number of XSK sockets we have. That
	 * means our umem offset for each descriptor is not uniform - and is different on a per-FQ/CQ
	 * basis. */
	int offset = xsk->umem_offset;

#else
	if (umem == NULL || xsk != NULL)
		exit_with_error(-EINVAL);

	fprintf(stdout, "Filling single-FCQ XSK\n");
	
	/* Single MCQ mode, so we fill the umem->fq. */
	struct xsk_ring_prod *fq_ptr = &umem->fq;

	/* Single MCQ mode, no per-xsk offset needed. */
	int offset = 0;

#endif /* MULTI_FCQ */

	ret = xsk_ring_prod__reserve(fq_ptr,
				     XSK_RING_PROD__DEFAULT_NUM_DESCS * 2, &idx);
	if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS * 2)
		exit_with_error(-ret);
	for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS * 2; i++)
		*xsk_ring_prod__fill_addr(fq_ptr, idx++) =
			offset + (i * opt_xsk_frame_size);
	xsk_ring_prod__submit(fq_ptr, XSK_RING_PROD__DEFAULT_NUM_DESCS * 2);
}

/* Original xsk_configure_socket() always binds to the same Channel ID, which is not multi-core.
 *
 * Revised API includes an xsk_index, so we can apply this as an offset from Channel ID,
 * allowing us to bind one XSK socket to each Channel. This also means we can support zero
 * copy queue offset via --queue=n, n being the offset.
 */
static struct xsk_socket_info *xsk_configure_socket(struct xsk_umem_info *umem,
						    bool rx, bool tx, int xsk_index)
{
	struct xsk_socket_config cfg;
	struct xsk_socket_info *xsk;
	struct xsk_ring_cons *rxr;
	struct xsk_ring_prod *txr;
	int ret;

	xsk = calloc(1, sizeof(*xsk));
	if (!xsk)
		exit_with_error(errno);

	xsk->umem = umem;
	cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;

#ifdef MULTI_FCQ

	/* We don't want to use dispatcher - we always want to load our kernel. */
	cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;

#else

	/* Original code */
	if (opt_num_xsks > 1 || opt_reduced_cap)
		cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
	else
		cfg.libbpf_flags = 0;

#endif /* MULTI_FCQ */

	cfg.xdp_flags = opt_xdp_flags;
	cfg.bind_flags = opt_xdp_bind_flags;

	rxr = rx ? &xsk->rx : NULL;
	txr = tx ? &xsk->tx : NULL;

	/* Save our position in xsks array and map. */
	xsk->xsk_index = xsk_index;

#ifdef MULTI_FCQ

	/* In a multi-FCQ setup we need to store a umem offset, telling us where the umem descriptors
	 * for this XSK are. This is so each channel does not hit the same memory space.
	 *
	 * Logic here is xsk[0] gets the first batch of descriptors, xsk[1] gets the next batch,
	 * and so on. */

	xsk->umem_offset = xsk_index * (NUM_FRAMES * opt_xsk_frame_size);

	/* In a multi-FCQ setup, we bind to multiple channel IDs, so we calculate this via the
	 * queue number + the xsk index. Mellanox cards will need to have --queue=n for zero copy. */

	xsk->channel_id = opt_queue + xsk_index;

	/* In a multi-FCQ setup we use the xsk_socket__create_shared() API which lets us pass
	 * in pointers to dedicated Fill/Completion queue per XSK. */

	fprintf(stdout, "Opening multi-FCQ XSK[%u] to %s channel %u...\n",
		xsk->xsk_index, opt_if, xsk->channel_id);
	ret = xsk_socket__create_shared(&xsk->xsk, opt_if, xsk->channel_id, umem->umem,
	 			rxr, txr, &xsk->fq, &xsk->cq, &cfg);

#else /* MULTI_FCQ */

	/* In a single-FCQ setup we stick to the original design of xdpsock_user.c, and so our channel
	 * ID will only ever be a single queue. */

	xsk->channel_id = opt_queue;
	
	fprintf(stdout, "Opening single-FCQ XSK[%u] to %s channel %u...\n",
		xsk->xsk_index, opt_if, opt_queue);
	ret = xsk_socket__create(&xsk->xsk, opt_if, opt_queue, umem->umem,
				 rxr, txr, &cfg);

#endif /* MULTI_FCQ */

	if (ret)
		exit_with_error(-ret);

	ret = bpf_xdp_query_id(opt_ifindex, opt_xdp_flags, &prog_id);
	if (ret)
		exit_with_error(-ret);

	xsk->app_stats.rx_empty_polls = 0;
	xsk->app_stats.fill_fail_polls = 0;
	xsk->app_stats.copy_tx_sendtos = 0;
	xsk->app_stats.tx_wakeup_sendtos = 0;
	xsk->app_stats.opt_polls = 0;
	xsk->app_stats.prev_rx_empty_polls = 0;
	xsk->app_stats.prev_fill_fail_polls = 0;
	xsk->app_stats.prev_copy_tx_sendtos = 0;
	xsk->app_stats.prev_tx_wakeup_sendtos = 0;
	xsk->app_stats.prev_opt_polls = 0;

	return xsk;
}

static struct option long_options[] = {
	{"rxdrop", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"l2fwd", no_argument, 0, 'l'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"poll", no_argument, 0, 'p'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
	{"interval", required_argument, 0, 'n'},
	{"retries", required_argument, 0, 'O'},
	{"zero-copy", no_argument, 0, 'z'},
	{"copy", no_argument, 0, 'c'},
	{"frame-size", required_argument, 0, 'f'},
	{"no-need-wakeup", no_argument, 0, 'm'},
	{"unaligned", no_argument, 0, 'u'},
#ifdef MULTI_FCQ
	{"channels", required_argument, 0, 'M'},
#else
	{"shared-umem", no_argument, 0, 'M'},
#endif
	{"force", no_argument, 0, 'F'},
	{"duration", required_argument, 0, 'd'},
	{"clock", required_argument, 0, 'w'},
	{"batch-size", required_argument, 0, 'b'},
	{"tx-pkt-count", required_argument, 0, 'C'},
	{"tx-pkt-size", required_argument, 0, 's'},
	{"tx-pkt-pattern", required_argument, 0, 'P'},
	{"tx-vlan", no_argument, 0, 'V'},
	{"tx-vlan-id", required_argument, 0, 'J'},
	{"tx-vlan-pri", required_argument, 0, 'K'},
	{"tx-dmac", required_argument, 0, 'G'},
	{"tx-smac", required_argument, 0, 'H'},
	{"tx-cycle", required_argument, 0, 'T'},
	{"tstamp", no_argument, 0, 'y'},
	{"policy", required_argument, 0, 'W'},
	{"schpri", required_argument, 0, 'U'},
	{"extra-stats", no_argument, 0, 'x'},
	{"quiet", no_argument, 0, 'Q'},
	{"app-stats", no_argument, 0, 'a'},
	{"irq-string", no_argument, 0, 'I'},
	{"busy-poll", no_argument, 0, 'B'},
	{"reduce-cap", no_argument, 0, 'R'},
	{0, 0, 0, 0}
};

static void usage(const char *prog)
{
	const char *str =
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -r, --rxdrop		Discard all incoming packets (default)\n"
		"  -t, --txonly		Only send packets\n"
		"  -l, --l2fwd		MAC swap L2 forwarding\n"
		"  -i, --interface=n	Run on interface n\n"
#ifdef MULTI_FCQ
		"  -q, --queue=n	Start at queue n (default 0), this can be used for ZC queue offsets (looking at you mlx...)\n"
#else
		"  -q, --queue=n	Use queue n (default 0)\n"
#endif
		"  -p, --poll		Use poll syscall\n"
		"  -S, --xdp-skb=n	Use XDP skb-mod\n"
		"  -N, --xdp-native=n	Enforce XDP native mode\n"
		"  -n, --interval=n	Specify statistics update interval (default 1 sec).\n"
		"  -O, --retries=n	Specify time-out retries (1s interval) attempt (default 3).\n"
		"  -z, --zero-copy      Force zero-copy mode.\n"
		"  -c, --copy           Force copy mode.\n"
		"  -m, --no-need-wakeup Turn off use of driver need wakeup flag.\n"
		"  -f, --frame-size=n   Set the frame size (must be a power of two in aligned mode, default is %d).\n"
		"  -u, --unaligned	Enable unaligned chunk placement\n"
#ifdef MULTI_FCQ
		"  -M, --channels=n		Open n number of channels. Also enables XDP_SHARED_UMEM (cannot be used with -R).\n"
#else
		"  -M, --shared-umem	Enable XDP_SHARED_UMEM (cannot be used with -R)\n"
#endif
		"  -F, --force		Force loading the XDP prog\n"
		"  -d, --duration=n	Duration in secs to run command.\n"
		"			Default: forever.\n"
		"  -w, --clock=CLOCK	Clock NAME (default MONOTONIC).\n"
		"  -b, --batch-size=n	Batch size for sending or receiving\n"
		"			packets. Default: %d\n"
		"  -C, --tx-pkt-count=n	Number of packets to send.\n"
		"			Default: Continuous packets.\n"
		"  -s, --tx-pkt-size=n	Transmit packet size.\n"
		"			(Default: %d bytes)\n"
		"			Min size: %d, Max size %d.\n"
		"  -P, --tx-pkt-pattern=nPacket fill pattern. Default: 0x%x\n"
		"  -V, --tx-vlan        Send VLAN tagged  packets (For -t|--txonly)\n"
		"  -J, --tx-vlan-id=n   Tx VLAN ID [1-4095]. Default: %d (For -V|--tx-vlan)\n"
		"  -K, --tx-vlan-pri=n  Tx VLAN Priority [0-7]. Default: %d (For -V|--tx-vlan)\n"
		"  -G, --tx-dmac=<MAC>  Dest MAC addr of TX frame in aa:bb:cc:dd:ee:ff format (For -V|--tx-vlan)\n"
		"  -H, --tx-smac=<MAC>  Src MAC addr of TX frame in aa:bb:cc:dd:ee:ff format (For -V|--tx-vlan)\n"
		"  -T, --tx-cycle=n     Tx cycle time in micro-seconds (For -t|--txonly).\n"
		"  -y, --tstamp         Add time-stamp to packet (For -t|--txonly).\n"
		"  -W, --policy=POLICY  Schedule policy. Default: SCHED_OTHER\n"
		"  -U, --schpri=n       Schedule priority. Default: %d\n"
		"  -x, --extra-stats	Display extra statistics.\n"
		"  -Q, --quiet          Do not display any stats.\n"
		"  -a, --app-stats	Display application (syscall) statistics.\n"
		"  -I, --irq-string	Display driver interrupt statistics for interface associated with irq-string.\n"
		"  -B, --busy-poll      Busy poll.\n"
		"  -R, --reduce-cap	Use reduced capabilities (cannot be used with -M)\n"
		"\nMAX_SOCKS:%d MULTI_FCQ:%s KRNL:%s DEBUGMODE:%s\n"
		"\n";
	fprintf(stderr, str, prog, XSK_UMEM__DEFAULT_FRAME_SIZE,
		opt_batch_size, MIN_PKT_SIZE, MIN_PKT_SIZE,
		XSK_UMEM__DEFAULT_FRAME_SIZE, opt_pkt_fill_pattern,
		VLAN_VID__DEFAULT, VLAN_PRI__DEFAULT,
		SCHED_PRI__DEFAULT,
		MAX_SOCKS,
#ifdef MULTI_FCQ
		"Yes",
#else
		"No",
#endif
		xdpsock_krnl,
#ifdef USE_DEBUGMODE
		"Yes"
#else
		"No"
#endif
		);

	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv,
#ifdef MULTI_FCQ
				"Frtli:q:pSNn:w:O:czf:muM:d:b:C:s:P:VJ:K:G:H:T:yW:U:xQaI:BR",
#else
				"Frtli:q:pSNn:w:O:czf:muMd:b:C:s:P:VJ:K:G:H:T:yW:U:xQaI:BR",
#endif
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'r':
			opt_bench = BENCH_RXDROP;
			break;
		case 't':
			opt_bench = BENCH_TXONLY;
			break;
		case 'l':
			opt_bench = BENCH_L2FWD;
			break;
		case 'i':
			opt_if = optarg;
			break;
		case 'q':
			opt_queue = atoi(optarg);
			break;
		case 'p':
			opt_poll = 1;
			break;
		case 'S':
			opt_xdp_flags |= XDP_FLAGS_SKB_MODE;
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'N':
			/* default, set below */
			break;
		case 'n':
			opt_interval = atoi(optarg);
			break;
		case 'w':
			if (get_clockid(&opt_clock, optarg)) {
				fprintf(stderr,
					"ERROR: Invalid clock %s. Default to CLOCK_MONOTONIC.\n",
					optarg);
				opt_clock = CLOCK_MONOTONIC;
			}
			break;
		case 'O':
			opt_retries = atoi(optarg);
			break;
		case 'z':
			opt_xdp_bind_flags |= XDP_ZEROCOPY;
			break;
		case 'c':
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'u':
			opt_umem_flags |= XDP_UMEM_UNALIGNED_CHUNK_FLAG;
			opt_unaligned_chunks = 1;
			opt_mmap_flags = MAP_HUGETLB;
			break;
		case 'F':
			opt_xdp_flags &= ~XDP_FLAGS_UPDATE_IF_NOEXIST;
			break;
		case 'f':
			opt_xsk_frame_size = atoi(optarg);
			break;
		case 'm':
			opt_need_wakeup = false;
			opt_xdp_bind_flags &= ~XDP_USE_NEED_WAKEUP;
			break;
		case 'M':
#ifdef MULTI_FCQ
			opt_num_xsks = atoi(optarg);
			if (opt_num_xsks == 0 || opt_num_xsks > MAX_SOCKS)
			{
				fprintf(stderr, "ERROR: Invalid number of XSK sockets: %d (Min: 1 Max: %d)\n",
					opt_num_xsks, MAX_SOCKS);
				usage(basename(argv[0]));
			}
#else
			opt_num_xsks = MAX_SOCKS;
#endif
			break;
		case 'd':
			opt_duration = atoi(optarg);
			opt_duration *= 1000000000;
			break;
		case 'b':
			opt_batch_size = atoi(optarg);
			break;
		case 'C':
			opt_pkt_count = atoi(optarg);
			break;
		case 's':
			opt_pkt_size = atoi(optarg);
			if (opt_pkt_size > (XSK_UMEM__DEFAULT_FRAME_SIZE) ||
			    opt_pkt_size < MIN_PKT_SIZE) {
				fprintf(stderr,
					"ERROR: Invalid frame size %d\n",
					opt_pkt_size);
				usage(basename(argv[0]));
			}
			break;
		case 'P':
			opt_pkt_fill_pattern = strtol(optarg, NULL, 16);
			break;
		case 'V':
			opt_vlan_tag = true;
			break;
		case 'J':
			opt_pkt_vlan_id = atoi(optarg);
			break;
		case 'K':
			opt_pkt_vlan_pri = atoi(optarg);
			break;
		case 'G':
			if (!ether_aton_r(optarg,
					  (struct ether_addr *)&opt_txdmac)) {
				fprintf(stderr, "Invalid dmac address:%s\n",
					optarg);
				usage(basename(argv[0]));
			}
			break;
		case 'H':
			if (!ether_aton_r(optarg,
					  (struct ether_addr *)&opt_txsmac)) {
				fprintf(stderr, "Invalid smac address:%s\n",
					optarg);
				usage(basename(argv[0]));
			}
			break;
		case 'T':
			opt_tx_cycle_ns = atoi(optarg);
			opt_tx_cycle_ns *= NSEC_PER_USEC;
			break;
		case 'y':
			opt_tstamp = 1;
			break;
		case 'W':
			if (get_schpolicy(&opt_schpolicy, optarg)) {
				fprintf(stderr,
					"ERROR: Invalid policy %s. Default to SCHED_OTHER.\n",
					optarg);
				opt_schpolicy = SCHED_OTHER;
			}
			break;
		case 'U':
			opt_schprio = atoi(optarg);
			break;
		case 'x':
			opt_extra_stats = 1;
			break;
		case 'Q':
			opt_quiet = 1;
			break;
		case 'a':
			opt_app_stats = 1;
			break;
		case 'I':
			opt_irq_str = optarg;
			if (get_interrupt_number())
				irqs_at_init = get_irqs();
			if (irqs_at_init < 0) {
				fprintf(stderr, "ERROR: Failed to get irqs for %s\n", opt_irq_str);
				usage(basename(argv[0]));
			}
			break;
		case 'B':
			opt_busy_poll = 1;
			break;
		case 'R':
			opt_reduced_cap = true;
			break;
		default:
			usage(basename(argv[0]));
		}
	}

	if (!(opt_xdp_flags & XDP_FLAGS_SKB_MODE))
		opt_xdp_flags |= XDP_FLAGS_DRV_MODE;

	opt_ifindex = if_nametoindex(opt_if);
	if (!opt_ifindex) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
			opt_if);
		usage(basename(argv[0]));
	}

	if ((opt_xsk_frame_size & (opt_xsk_frame_size - 1)) &&
	    !opt_unaligned_chunks) {
		fprintf(stderr, "--frame-size=%d is not a power of two\n",
			opt_xsk_frame_size);
		usage(basename(argv[0]));
	}

	if (opt_reduced_cap && opt_num_xsks > 1) {
		fprintf(stderr, "ERROR: -M and -R cannot be used together\n");
		usage(basename(argv[0]));
	}
}

static void kick_tx(struct xsk_socket_info *xsk)
{
	int ret;

	ret = sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
	if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN ||
	    errno == EBUSY || errno == ENETDOWN)
		return;
	exit_with_error(errno);
}

static inline void complete_tx_l2fwd(struct xsk_socket_info *xsk)
{
	u32 idx_cq = 0, idx_fq = 0;
	unsigned int rcvd;
	size_t ndescs;

	if (!xsk->outstanding_tx)
		return;

	/* In copy mode, Tx is driven by a syscall so we need to use e.g. sendto() to
	 * really send the packets. In zero-copy mode we do not have to do this, since Tx
	 * is driven by the NAPI loop. So as an optimization, we do not have to call
	 * sendto() all the time in zero-copy mode for l2fwd.
	 */
	if (opt_xdp_bind_flags & XDP_COPY) {
		xsk->app_stats.copy_tx_sendtos++;
		kick_tx(xsk);
	}

	ndescs = (xsk->outstanding_tx > opt_batch_size) ? opt_batch_size :
		xsk->outstanding_tx;

#ifdef MULTI_FCQ
	struct xsk_ring_prod *fq_ptr = &xsk->fq;
	struct xsk_ring_cons *cq_ptr = &xsk->cq;
#else
	struct xsk_umem_info *umem = xsk->umem;
	struct xsk_ring_prod *fq_ptr = &umem->fq;
	struct xsk_ring_cons *cq_ptr = &umem->cq;
#endif

	/* re-add completed Tx buffers */
	rcvd = xsk_ring_cons__peek(cq_ptr, ndescs, &idx_cq);
	if (rcvd > 0) {
		unsigned int i;
		int ret;

		ret = xsk_ring_prod__reserve(fq_ptr, rcvd, &idx_fq);
		while (ret != rcvd) {
			if (ret < 0)
				exit_with_error(-ret);
			if (opt_busy_poll || xsk_ring_prod__needs_wakeup(fq_ptr)) {
				xsk->app_stats.fill_fail_polls++;
				recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL,
					 NULL);
			}
			ret = xsk_ring_prod__reserve(fq_ptr, rcvd, &idx_fq);
		}

		for (i = 0; i < rcvd; i++)
			*xsk_ring_prod__fill_addr(fq_ptr, idx_fq++) =
				*xsk_ring_cons__comp_addr(cq_ptr, idx_cq++);

		xsk_ring_prod__submit(fq_ptr, rcvd);
		xsk_ring_cons__release(cq_ptr, rcvd);
		xsk->outstanding_tx -= rcvd;
	}
}

static inline void complete_tx_only(struct xsk_socket_info *xsk,
				    int batch_size)
{
	unsigned int rcvd;
	u32 idx;

	if (!xsk->outstanding_tx)
		return;

	if (!opt_need_wakeup || xsk_ring_prod__needs_wakeup(&xsk->tx)) {
		xsk->app_stats.tx_wakeup_sendtos++;
		kick_tx(xsk);
	}

#ifdef MULTI_FCQ
	struct xsk_ring_cons *cq_ptr = &xsk->cq;
#else
	struct xsk_ring_cons *cq_ptr = &xsk->umem->cq;
#endif

	rcvd = xsk_ring_cons__peek(cq_ptr, batch_size, &idx);
	if (rcvd > 0) {
		xsk_ring_cons__release(cq_ptr, rcvd);
		xsk->outstanding_tx -= rcvd;
	}
}

static void rx_drop(struct xsk_socket_info *xsk)
{
	unsigned int rcvd, i;
	u32 idx_rx = 0, idx_fq = 0;
	int ret;

#ifdef MULTI_FCQ
	/* In a multi-FCQ setup, we take the XSK's dedicated fill queue */
	struct xsk_ring_prod *fq_ptr = &xsk->fq;
#else
	/* Otherwise we take the single umem fill queue */
	struct xsk_ring_prod *fq_ptr = &xsk->umem->fq;
#endif

	rcvd = xsk_ring_cons__peek(&xsk->rx, opt_batch_size, &idx_rx);
	if (!rcvd) {
		if (opt_busy_poll || xsk_ring_prod__needs_wakeup(fq_ptr)) {
			xsk->app_stats.rx_empty_polls++;
			recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
		}
		return;
	}

	ret = xsk_ring_prod__reserve(fq_ptr, rcvd, &idx_fq);
	while (ret != rcvd) {
		if (ret < 0)
			exit_with_error(-ret);
		if (opt_busy_poll || xsk_ring_prod__needs_wakeup(fq_ptr)) {
			xsk->app_stats.fill_fail_polls++;
			recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
		}
		ret = xsk_ring_prod__reserve(fq_ptr, rcvd, &idx_fq);
	}

	for (i = 0; i < rcvd; i++) {
		u64 addr = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx)->addr;
		u32 len = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++)->len;
		u64 orig = xsk_umem__extract_addr(addr);

		addr = xsk_umem__add_offset_to_addr(addr);
		char *pkt = xsk_umem__get_data(xsk->umem->buffer, addr);

		hex_dump(pkt, len, addr);
		*xsk_ring_prod__fill_addr(fq_ptr, idx_fq++) = orig;
	}

	xsk_ring_prod__submit(fq_ptr, rcvd);
	xsk_ring_cons__release(&xsk->rx, rcvd);
	xsk->ring_stats.rx_npkts += rcvd;
}

static void rx_drop_all(void)
{
	struct pollfd fds[MAX_SOCKS] = {};
	int i, ret;

	for (i = 0; i < num_socks; i++) {
		fds[i].fd = xsk_socket__fd(xsks[i]->xsk);
		fds[i].events = POLLIN;
	}

	for (;;) {
		if (opt_poll) {
			for (i = 0; i < num_socks; i++)
				xsks[i]->app_stats.opt_polls++;

			ret = poll(fds, num_socks, opt_timeout);
			if (ret <= 0)
#ifdef USE_ORIGINAL
				continue;
#else
			{
				if (benchmark_done)
					break;
				continue;
			}
#endif /* USE_ORIGINAL */
		}

		for (i = 0; i < num_socks; i++)
			rx_drop(xsks[i]);

		if (benchmark_done)
			break;
	}
}

static int tx_only(struct xsk_socket_info *xsk, u32 *frame_nb,
		   int batch_size, unsigned long tx_ns)
{
	u32 idx, tv_sec, tv_usec;
	unsigned int i;

	while (xsk_ring_prod__reserve(&xsk->tx, batch_size, &idx) <
				      batch_size) {
		complete_tx_only(xsk, batch_size);
		if (benchmark_done)
			return 0;
	}

	if (opt_tstamp) {
		tv_sec = (u32)(tx_ns / NSEC_PER_SEC);
		tv_usec = (u32)((tx_ns % NSEC_PER_SEC) / 1000);
	}

	for (i = 0; i < batch_size; i++) {
		struct xdp_desc *tx_desc = xsk_ring_prod__tx_desc(&xsk->tx,
								  idx + i);
		tx_desc->addr = (*frame_nb + i) * opt_xsk_frame_size;
		tx_desc->len = PKT_SIZE;

		if (opt_tstamp) {
			struct pktgen_hdr *pktgen_hdr;
			u64 addr = tx_desc->addr;
			char *pkt;

			pkt = xsk_umem__get_data(xsk->umem->buffer, addr);
			pktgen_hdr = (struct pktgen_hdr *)(pkt + PKTGEN_HDR_OFFSET);

			pktgen_hdr->seq_num = htonl(sequence++);
			pktgen_hdr->tv_sec = htonl(tv_sec);
			pktgen_hdr->tv_usec = htonl(tv_usec);

			hex_dump(pkt, PKT_SIZE, addr);
		}
	}

	xsk_ring_prod__submit(&xsk->tx, batch_size);
	xsk->ring_stats.tx_npkts += batch_size;
	xsk->outstanding_tx += batch_size;
	*frame_nb += batch_size;
	*frame_nb %= NUM_FRAMES;
	complete_tx_only(xsk, batch_size);

	return batch_size;
}

static inline int get_batch_size(int pkt_cnt)
{
	if (!opt_pkt_count)
		return opt_batch_size;

	if (pkt_cnt + opt_batch_size <= opt_pkt_count)
		return opt_batch_size;

	return opt_pkt_count - pkt_cnt;
}

static void complete_tx_only_all(void)
{
	bool pending;
	int i;

	do {
		pending = false;
		for (i = 0; i < num_socks; i++) {
			if (xsks[i]->outstanding_tx) {
				complete_tx_only(xsks[i], opt_batch_size);
				pending = !!xsks[i]->outstanding_tx;
			}
		}
		sleep(1);
	} while (pending && opt_retries-- > 0);
}

static void tx_only_all(void)
{
	struct pollfd fds[MAX_SOCKS] = {};
	u32 frame_nb[MAX_SOCKS] = {};
	unsigned long next_tx_ns = 0;
	int pkt_cnt = 0;
	int i, ret;

	if (opt_poll && opt_tx_cycle_ns) {
		fprintf(stderr,
			"Error: --poll and --tx-cycles are both set\n");
		return;
	}

	for (i = 0; i < num_socks; i++) {
		fds[0].fd = xsk_socket__fd(xsks[i]->xsk);
		fds[0].events = POLLOUT;
	}

	if (opt_tx_cycle_ns) {
		/* Align Tx time to micro-second boundary */
		next_tx_ns = (get_nsecs() / NSEC_PER_USEC + 1) *
			     NSEC_PER_USEC;
		next_tx_ns += opt_tx_cycle_ns;

		/* Initialize periodic Tx scheduling variance */
		tx_cycle_diff_min = 1000000000;
		tx_cycle_diff_max = 0;
		tx_cycle_diff_ave = 0.0;
	}

	while ((opt_pkt_count && pkt_cnt < opt_pkt_count) || !opt_pkt_count) {
		int batch_size = get_batch_size(pkt_cnt);
		unsigned long tx_ns = 0;
		struct timespec next;
		int tx_cnt = 0;
		long diff;
		int err;

		if (opt_poll) {
			for (i = 0; i < num_socks; i++)
				xsks[i]->app_stats.opt_polls++;
			ret = poll(fds, num_socks, opt_timeout);
			if (ret <= 0)
#ifdef USE_ORIGINAL
				continue;
#else
			{
				if (benchmark_done)
					break;
				continue;
			}
#endif

			if (!(fds[0].revents & POLLOUT))
				continue;
		}

		if (opt_tx_cycle_ns) {
			next.tv_sec = next_tx_ns / NSEC_PER_SEC;
			next.tv_nsec = next_tx_ns % NSEC_PER_SEC;
			err = clock_nanosleep(opt_clock, TIMER_ABSTIME, &next, NULL);
			if (err) {
				if (err != EINTR)
					fprintf(stderr,
						"clock_nanosleep failed. Err:%d errno:%d\n",
						err, errno);
				break;
			}

			/* Measure periodic Tx scheduling variance */
			tx_ns = get_nsecs();
			diff = tx_ns - next_tx_ns;
			if (diff < tx_cycle_diff_min)
				tx_cycle_diff_min = diff;

			if (diff > tx_cycle_diff_max)
				tx_cycle_diff_max = diff;

			tx_cycle_diff_ave += (double)diff;
			tx_cycle_cnt++;
		} else if (opt_tstamp) {
			tx_ns = get_nsecs();
		}

		for (i = 0; i < num_socks; i++)
			tx_cnt += tx_only(xsks[i], &frame_nb[i], batch_size, tx_ns);

		pkt_cnt += tx_cnt;

		if (benchmark_done)
			break;

		if (opt_tx_cycle_ns)
			next_tx_ns += opt_tx_cycle_ns;
	}

	if (opt_pkt_count)
		complete_tx_only_all();
}

static void l2fwd(struct xsk_socket_info *xsk)
{
	unsigned int rcvd, i;
	u32 idx_rx = 0, idx_tx = 0;
	int ret;

	complete_tx_l2fwd(xsk);

	rcvd = xsk_ring_cons__peek(&xsk->rx, opt_batch_size, &idx_rx);
	if (!rcvd) {
#ifdef MULTI_FCQ
		struct xsk_ring_prod *fq_ptr = &xsk->fq;
#else
		struct xsk_ring_prod *fq_ptr = &xsk->umem->fq;
#endif
		if (opt_busy_poll || xsk_ring_prod__needs_wakeup(fq_ptr)) {
			xsk->app_stats.rx_empty_polls++;
			recvfrom(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
		}
		return;
	}
	xsk->ring_stats.rx_npkts += rcvd;

	ret = xsk_ring_prod__reserve(&xsk->tx, rcvd, &idx_tx);
	while (ret != rcvd) {
		if (ret < 0)
			exit_with_error(-ret);
		complete_tx_l2fwd(xsk);
		if (opt_busy_poll || xsk_ring_prod__needs_wakeup(&xsk->tx)) {
			xsk->app_stats.tx_wakeup_sendtos++;
			kick_tx(xsk);
		}
		ret = xsk_ring_prod__reserve(&xsk->tx, rcvd, &idx_tx);
	}

	for (i = 0; i < rcvd; i++) {
		u64 addr = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx)->addr;
		u32 len = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++)->len;
		u64 orig = addr;

		addr = xsk_umem__add_offset_to_addr(addr);
		char *pkt = xsk_umem__get_data(xsk->umem->buffer, addr);

		swap_mac_addresses(pkt);

		hex_dump(pkt, len, addr);
		xsk_ring_prod__tx_desc(&xsk->tx, idx_tx)->addr = orig;
		xsk_ring_prod__tx_desc(&xsk->tx, idx_tx++)->len = len;
	}

	xsk_ring_prod__submit(&xsk->tx, rcvd);
	xsk_ring_cons__release(&xsk->rx, rcvd);

	xsk->ring_stats.tx_npkts += rcvd;
	xsk->outstanding_tx += rcvd;
}

static void l2fwd_all(void)
{
	struct pollfd fds[MAX_SOCKS] = {};
	int i, ret;

	for (;;) {
		if (opt_poll) {
			for (i = 0; i < num_socks; i++) {
				fds[i].fd = xsk_socket__fd(xsks[i]->xsk);
				fds[i].events = POLLOUT | POLLIN;
				xsks[i]->app_stats.opt_polls++;
			}
			ret = poll(fds, num_socks, opt_timeout);
			if (ret <= 0)
#ifdef USE_ORIGINAL
				continue;
#else
			{
				if (benchmark_done)
					break;
				continue;
			}
#endif /* USE_ORIGINAL */
		}

		for (i = 0; i < num_socks; i++)
			l2fwd(xsks[i]);

		if (benchmark_done)
			break;
	}
}

static struct bpf_object *load_xdp_program(char **argv)
{
	struct bpf_object *bpf_obj = NULL;
	struct bpf_program *bpf_prog = NULL;
	int prog_fd;
	int err;

	fprintf(stdout, "Our XDP kernel is: %s\n", xdpsock_krnl);

	bpf_obj = bpf_object__open_file(xdpsock_krnl, NULL);
	if (bpf_obj == NULL)
	{
		fprintf(
			stdout, "Error opening the BPF object: %s, %s", xdpsock_krnl, strerror(errno));
		exit(EXIT_FAILURE);
	}

	bpf_prog = bpf_object__find_program_by_name(bpf_obj, "xdp_sock_prog");
	if (bpf_prog == NULL)
	{
		/* Don't take a risk with this. If our section name can't be found, close the
		 * object. This will also close the object FD. */
		bpf_object__close(bpf_obj);
		bpf_obj = NULL;
		fprintf(stdout,
		               "Error loading BPF program: %s: section_name %s not found",
		               xdpsock_krnl,
		               "xdp_sock_prog");
		exit(EXIT_FAILURE);
	}

	/* Set the type of the program after we have found it from the section name. */
	bpf_program__set_type(bpf_prog, BPF_PROG_TYPE_XDP);

	/* Now we have found the program and set up the program type, we can load
	 * the BPF object into the kernel. */
	err = bpf_object__load(bpf_obj);
	if (err != 0)
	{
		bpf_object__close(bpf_obj);
		char errbuf[256];
		libbpf_strerror(err, errbuf, sizeof(errbuf));
		fprintf(stdout, "Error loading BPF object: %s: %s", xdpsock_krnl, errbuf);
		exit(EXIT_FAILURE);
	}

	prog_fd = bpf_program__fd(bpf_prog);
	if (prog_fd < 0) {
		printf("program not found: %s\n", strerror(prog_fd));
		exit(EXIT_FAILURE);
	}

	if (bpf_xdp_attach(opt_ifindex, prog_fd, opt_xdp_flags, NULL) < 0) {
		fprintf(stderr, "ERROR: link set xdp fd failed\n");
		exit(EXIT_FAILURE);
	}

	return bpf_obj;
	fprintf(stdout, "XDP Program loaded: %s\n", xdpsock_krnl);
}

static void enter_xsks_into_map(struct bpf_object *obj)
{
	struct bpf_map *map;
	int i, xsks_map;

	map = bpf_object__find_map_by_name(obj, "xsks_map");
	xsks_map = bpf_map__fd(map);
	if (xsks_map < 0) {
		fprintf(stderr, "ERROR: no xsks map found: %s\n",
			strerror(xsks_map));
			exit(EXIT_FAILURE);
	}

	for (i = 0; i < num_socks; i++) {
#ifdef MUTLI_FCQ
		if (i != xsks[i]->xsk_index)
		{
			fprintf(stderr, "ERROR: xsk with invalid xsk_index at index (xsk_index:%u, i:%d)\n",
				xsks[i]->xsk_index, i);
			exit(EXIT_FAILURE);
		}
#endif

		int fd = xsk_socket__fd(xsks[i]->xsk);
		int key, ret;

#ifdef MULTI_FCQ
		/* In a multi-FCQ setup, we need to insert with key=channel */
		key = xsks[i]->channel_id;
#else
		/* In a single-FCQ setup, we need to insert with key=xsk_index */
		key = xsks[i]->xsk_index;
#endif
		ret = bpf_map_update_elem(xsks_map, &key, &fd, 0);
		if (ret) {
			fprintf(stderr, "ERROR: bpf_map_update_elem %d, ret: %d\n", i, ret);
			exit(EXIT_FAILURE);
		}

		fprintf(stdout, "Inserted XSK[%u] fd:%d into xsks_map[key=%u]\n", xsks[i]->xsk_index, fd, key);
	}
}

static void apply_setsockopt(struct xsk_socket_info *xsk)
{
	int sock_opt;

	if (!opt_busy_poll)
		return;

	sock_opt = 1;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_PREFER_BUSY_POLL,
		       (void *)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);

	sock_opt = 20;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_BUSY_POLL,
		       (void *)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);

	sock_opt = opt_batch_size;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_BUSY_POLL_BUDGET,
		       (void *)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);
}

static int recv_xsks_map_fd_from_ctrl_node(int sock, int *_fd)
{
	char cms[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec iov;
	int value;
	int len;

	iov.iov_base = &value;
	iov.iov_len = sizeof(int);

	msg.msg_name = 0;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_flags = 0;
	msg.msg_control = (caddr_t)cms;
	msg.msg_controllen = sizeof(cms);

	len = recvmsg(sock, &msg, 0);

	if (len < 0) {
		fprintf(stderr, "Recvmsg failed length incorrect.\n");
		return -EINVAL;
	}

	if (len == 0) {
		fprintf(stderr, "Recvmsg failed no data\n");
		return -EINVAL;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	*_fd = *(int *)CMSG_DATA(cmsg);

	return 0;
}

static int
recv_xsks_map_fd(int *xsks_map_fd)
{
	struct sockaddr_un server;
	int err;

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		fprintf(stderr, "Error opening socket stream: %s", strerror(errno));
		return errno;
	}

	server.sun_family = AF_UNIX;
	strcpy(server.sun_path, SOCKET_NAME);

	if (connect(sock, (struct sockaddr *)&server, sizeof(struct sockaddr_un)) < 0) {
		close(sock);
		fprintf(stderr, "Error connecting stream socket: %s", strerror(errno));
		return errno;
	}

	err = recv_xsks_map_fd_from_ctrl_node(sock, xsks_map_fd);
	if (err) {
		fprintf(stderr, "Error %d receiving fd\n", err);
		return err;
	}
	return 0;
}

int xdpsock_open_bpfstats_map(struct bpf_object *obj)
{
	struct bpf_map *map_ptr;
	map_ptr = bpf_object__find_map_by_name(obj, "bpfstats");

	int bpfstats_fd = bpf_map__fd(map_ptr);
	if (bpfstats_fd < 0) {
		return -1;
	}
	return bpfstats_fd;
}

int main(int argc, char **argv)
{
	struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
	struct __user_cap_data_struct data[2] = { { 0 } };
	bool rx = false, tx = false;
	struct sched_param schparam;
	struct xsk_umem_info *umem;
	struct bpf_object *obj;
	int xsks_map_fd = 0;
	pthread_t pt;
	int i, ret;
	void *bufs;

	parse_command_line(argc, argv);

	if (opt_reduced_cap) {
		if (capget(&hdr, data)  < 0)
			fprintf(stderr, "Error getting capabilities\n");

		data->effective &= CAP_TO_MASK(CAP_NET_RAW);
		data->permitted &= CAP_TO_MASK(CAP_NET_RAW);

		if (capset(&hdr, data) < 0)
			fprintf(stderr, "Setting capabilities failed\n");

		if (capget(&hdr, data)  < 0) {
			fprintf(stderr, "Error getting capabilities\n");
		} else {
			fprintf(stderr, "Capabilities EFF %x Caps INH %x Caps Per %x\n",
				data[0].effective, data[0].inheritable, data[0].permitted);
			fprintf(stderr, "Capabilities EFF %x Caps INH %x Caps Per %x\n",
				data[1].effective, data[1].inheritable, data[1].permitted);
		}
	} else {
		/* Use libbpf 1.0 API mode */
		libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

#ifndef MULTI_FCQ
		/* In a single-FCQ setup we only load a program if num_xsks > 1. */
		if (opt_num_xsks > 1)
#endif
			obj = load_xdp_program(argv);
	}

	fprintf(stdout, "Bringing up %u AF_XDP sockets in %s mode...\n", opt_num_xsks,
#ifdef MULTI_FCQ
		"Multi-FCQ"
#else
		"Single-FCQ"
#endif
	);

	/* Reserve memory for the umem. Use hugepages if unaligned chunk mode */
#ifdef MULTI_FCQ
	bufs = mmap(NULL, (NUM_FRAMES * opt_xsk_frame_size) * opt_num_xsks,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | opt_mmap_flags, -1, 0);
#else
	bufs = mmap(NULL, NUM_FRAMES * opt_xsk_frame_size,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | opt_mmap_flags, -1, 0);
#endif /* MULTI_FCQ */
	if (bufs == MAP_FAILED) {
		printf("ERROR: mmap failed\n");
		exit(EXIT_FAILURE);
	}

	/* Create sockets... */
#ifdef MULTI_FCQ
	umem = xsk_configure_umem(bufs, (NUM_FRAMES * opt_xsk_frame_size) * opt_num_xsks);
#else
	umem = xsk_configure_umem(bufs, NUM_FRAMES * opt_xsk_frame_size);
#endif
	if (opt_bench == BENCH_RXDROP || opt_bench == BENCH_L2FWD) {
		rx = true;
#ifdef MULTI_FCQ
		/* In multi-fcq setup we don't fill here, we need XSK's to be setup. */
#else
		/* In a single-fcq setup we fill here before XSKs are setup */
		xsk_populate_fill_ring(umem, NULL);
#endif
	}
	if (opt_bench == BENCH_L2FWD || opt_bench == BENCH_TXONLY)
		tx = true;
	for (i = 0; i < opt_num_xsks; i++)
		xsks[num_socks++] = xsk_configure_socket(umem, rx, tx, i);

#ifdef MULTI_FCQ
	/* In a multi-fcq setup we fill via each XSK FQ. */
	for (i = 0; i < opt_num_xsks; i++)
		xsk_populate_fill_ring(umem, xsks[i]);
#endif

	for (i = 0; i < opt_num_xsks; i++)
		apply_setsockopt(xsks[i]);

	if (opt_bench == BENCH_TXONLY) {
		if (opt_tstamp && opt_pkt_size < PKTGEN_SIZE_MIN)
			opt_pkt_size = PKTGEN_SIZE_MIN;

		gen_eth_hdr_data();

		for (i = 0; i < NUM_FRAMES; i++)
			gen_eth_frame(umem, i * opt_xsk_frame_size);
	}

	if (obj == NULL)
	{
		fprintf(stderr, "BPF Object is still NULL\n");
		exit(EXIT_FAILURE);
	}

#ifdef MULTI_FCQ
	/* We need to insert our XSK irrespective of whether we have 1 channel or not */
	if (opt_bench != BENCH_TXONLY)
#else
	/* In single FCQ mode we default to the original logic */
	if (opt_num_xsks > 1 && opt_bench != BENCH_TXONLY)
#endif
		enter_xsks_into_map(obj);

	if (opt_reduced_cap) {
		ret = recv_xsks_map_fd(&xsks_map_fd);
		if (ret) {
			fprintf(stderr, "Error %d receiving xsks_map_fd\n", ret);
			exit_with_error(ret);
		}
		if (xsks[0]->xsk) {
			ret = xsk_socket__update_xskmap(xsks[0]->xsk, xsks_map_fd);
			if (ret) {
				fprintf(stderr, "Update of BPF map failed(%d)\n", ret);
				exit_with_error(ret);
			}
		}
	}

	bpfstats_fd = xdpsock_open_bpfstats_map(obj);

	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);
	signal(SIGABRT, int_exit);

	setlocale(LC_ALL, "");

	prev_time = get_nsecs();
	start_time = prev_time;

	if (!opt_quiet) {
		ret = pthread_create(&pt, NULL, poller, NULL);
		if (ret)
			exit_with_error(ret);
	}

	/* Configure sched priority for better wake-up accuracy */
	memset(&schparam, 0, sizeof(schparam));
	schparam.sched_priority = opt_schprio;
	ret = sched_setscheduler(0, opt_schpolicy, &schparam);
	if (ret) {
		fprintf(stderr, "Error(%d) in setting priority(%d): %s\n",
			errno, opt_schprio, strerror(errno));
		goto out;
	}

	if (opt_bench == BENCH_RXDROP)
		rx_drop_all();
	else if (opt_bench == BENCH_TXONLY)
		tx_only_all();
	else
		l2fwd_all();

out:
	benchmark_done = true;

	if (!opt_quiet)
		pthread_join(pt, NULL);

	xdpsock_cleanup();

#ifdef MULTI_FCQ
	munmap(bufs, (NUM_FRAMES * opt_xsk_frame_size) * opt_num_xsks);
#else
	munmap(bufs, NUM_FRAMES * opt_xsk_frame_size);
#endif

	return 0;
}
