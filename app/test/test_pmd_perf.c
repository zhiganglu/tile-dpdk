/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdio.h>
#include <inttypes.h>
#include <signal.h>
#include <unistd.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_byteorder.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include "packet_burst_generator.h"
#include "test.h"

#define NB_ETHPORTS_USED                (1)
#define NB_SOCKETS                      (2)
#define MEMPOOL_CACHE_SIZE 250
#define MBUF_SIZE (2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MAX_PKT_BURST                   (32)
#define RTE_TEST_RX_DESC_DEFAULT        (128)
#define RTE_TEST_TX_DESC_DEFAULT        (512)
#define RTE_PORT_ALL            (~(uint8_t)0x0)

/* how long test would take at full line rate */
#define RTE_TEST_DURATION                (2)

/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 0 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 32 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH 0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH 0  /**< Default values of TX write-back threshold reg. */

#define MAX_TRAFFIC_BURST              2048

#define NB_MBUF RTE_MAX(						\
		(unsigned)(nb_ports*nb_rx_queue*nb_rxd +		\
			   nb_ports*nb_lcores*MAX_PKT_BURST +		\
			   nb_ports*nb_tx_queue*nb_txd +		\
			   nb_lcores*MEMPOOL_CACHE_SIZE +		\
			   nb_ports*MAX_TRAFFIC_BURST),			\
			(unsigned)8192)


static struct rte_mempool *mbufpool[NB_SOCKETS];
/* ethernet addresses of ports */
static struct ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_NONE,
		.max_rx_pkt_len = ETHER_MAX_LEN,
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload enabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.hw_vlan_strip  = 0, /**< VLAN strip enabled. */
		.hw_vlan_extend = 0, /**< Extended VLAN disabled. */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
		.enable_scatter = 0, /**< scatter rx disabled */
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
	.lpbk_mode = 1,  /* enable loopback */
};

static struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = RX_PTHRESH,
		.hthresh = RX_HTHRESH,
		.wthresh = RX_WTHRESH,
	},
	.rx_free_thresh = 32,
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = TX_PTHRESH,
		.hthresh = TX_HTHRESH,
		.wthresh = TX_WTHRESH,
	},
	.tx_free_thresh = 32, /* Use PMD default values */
	.tx_rs_thresh = 32, /* Use PMD default values */
	.txq_flags = (ETH_TXQ_FLAGS_NOMULTSEGS |
		      ETH_TXQ_FLAGS_NOVLANOFFL |
		      ETH_TXQ_FLAGS_NOXSUMSCTP |
		      ETH_TXQ_FLAGS_NOXSUMUDP |
		      ETH_TXQ_FLAGS_NOXSUMTCP)
};

enum {
	LCORE_INVALID = 0,
	LCORE_AVAIL,
	LCORE_USED,
};

struct lcore_conf {
	uint8_t status;
	uint8_t socketid;
	uint16_t nb_ports;
	uint8_t portlist[RTE_MAX_ETHPORTS];
} __rte_cache_aligned;

struct lcore_conf lcore_conf[RTE_MAX_LCORE];

static uint64_t link_mbps;

enum {
	SC_CONTINUOUS = 0,
	SC_BURST_POLL_FIRST,
	SC_BURST_XMIT_FIRST,
};

static uint32_t sc_flag;

/* Check the link status of all ports in up to 3s, and print them finally */
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 30 /* 3s (30 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("Checking link statuses...\n");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status) {
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
					if (link_mbps == 0)
						link_mbps = link.link_speed;
				} else
					printf("Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
			print_flag = 1;
	}
}

static void
print_ethaddr(const char *name, const struct ether_addr *eth_addr)
{
	char buf[ETHER_ADDR_FMT_SIZE];
	ether_format_addr(buf, ETHER_ADDR_FMT_SIZE, eth_addr);
	printf("%s%s", name, buf);
}

static int
init_traffic(struct rte_mempool *mp,
	     struct rte_mbuf **pkts_burst, uint32_t burst_size)
{
	struct ether_hdr pkt_eth_hdr;
	struct ipv4_hdr pkt_ipv4_hdr;
	struct udp_hdr pkt_udp_hdr;
	uint32_t pktlen;
	static uint8_t src_mac[] = { 0x00, 0xFF, 0xAA, 0xFF, 0xAA, 0xFF };
	static uint8_t dst_mac[] = { 0x00, 0xAA, 0xFF, 0xAA, 0xFF, 0xAA };


	initialize_eth_header(&pkt_eth_hdr,
		(struct ether_addr *)src_mac,
		(struct ether_addr *)dst_mac, 0, 0);
	pkt_eth_hdr.ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

	pktlen = initialize_ipv4_header(&pkt_ipv4_hdr,
					IPV4_ADDR(10, 0, 0, 1),
					IPV4_ADDR(10, 0, 0, 2), 26);
	printf("IPv4 pktlen %u\n", pktlen);

	pktlen = initialize_udp_header(&pkt_udp_hdr, 0, 0, 18);

	printf("UDP pktlen %u\n", pktlen);

	return generate_packet_burst(mp, pkts_burst, &pkt_eth_hdr,
				     0, &pkt_ipv4_hdr, 1,
				     &pkt_udp_hdr, burst_size,
				     PACKET_BURST_GEN_PKT_LEN, 1);
}

static int
init_lcores(void)
{
	unsigned lcore_id;

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		lcore_conf[lcore_id].socketid =
			rte_lcore_to_socket_id(lcore_id);
		if (rte_lcore_is_enabled(lcore_id) == 0) {
			lcore_conf[lcore_id].status = LCORE_INVALID;
			continue;
		} else
			lcore_conf[lcore_id].status = LCORE_AVAIL;
	}
	return 0;
}

static int
init_mbufpool(unsigned nb_mbuf)
{
	int socketid;
	unsigned lcore_id;
	char s[64];

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (rte_lcore_is_enabled(lcore_id) == 0)
			continue;

		socketid = rte_lcore_to_socket_id(lcore_id);
		if (socketid >= NB_SOCKETS) {
			rte_exit(EXIT_FAILURE,
				"Socket %d of lcore %u is out of range %d\n",
				socketid, lcore_id, NB_SOCKETS);
		}
		if (mbufpool[socketid] == NULL) {
			snprintf(s, sizeof(s), "mbuf_pool_%d", socketid);
			mbufpool[socketid] =
				rte_mempool_create(s, nb_mbuf, MBUF_SIZE,
					MEMPOOL_CACHE_SIZE,
					sizeof(struct rte_pktmbuf_pool_private),
					rte_pktmbuf_pool_init, NULL,
					rte_pktmbuf_init, NULL,
					socketid, 0);
			if (mbufpool[socketid] == NULL)
				rte_exit(EXIT_FAILURE,
					"Cannot init mbuf pool on socket %d\n",
					socketid);
			else
				printf("Allocated mbuf pool on socket %d\n",
					socketid);
		}
	}
	return 0;
}

static uint16_t
alloc_lcore(uint16_t socketid)
{
	unsigned lcore_id;

	for (lcore_id = 0; lcore_id < RTE_MAX_LCORE; lcore_id++) {
		if (LCORE_AVAIL != lcore_conf[lcore_id].status ||
		    lcore_conf[lcore_id].socketid != socketid ||
		    lcore_id == rte_get_master_lcore())
			continue;
		lcore_conf[lcore_id].status = LCORE_USED;
		lcore_conf[lcore_id].nb_ports = 0;
		return lcore_id;
	}

	return (uint16_t)-1;
}

volatile uint64_t stop;
uint64_t count;
uint64_t drop;
uint64_t idle;

static void
reset_count(void)
{
	count = 0;
	drop = 0;
	idle = 0;
}

static void
stats_display(uint8_t port_id)
{
	struct rte_eth_stats stats;
	rte_eth_stats_get(port_id, &stats);

	printf("  RX-packets: %-10"PRIu64" RX-missed: %-10"PRIu64" RX-bytes:  "
	       "%-"PRIu64"\n",
	       stats.ipackets, stats.imissed, stats.ibytes);
	printf("  RX-badcrc:  %-10"PRIu64" RX-badlen: %-10"PRIu64" RX-errors: "
	       "%-"PRIu64"\n",
	       stats.ibadcrc, stats.ibadlen, stats.ierrors);
	printf("  RX-nombuf:  %-10"PRIu64"\n",
	       stats.rx_nombuf);
	printf("  TX-packets: %-10"PRIu64" TX-errors: %-10"PRIu64" TX-bytes:  "
	       "%-"PRIu64"\n",
	       stats.opackets, stats.oerrors, stats.obytes);
}

static void
signal_handler(int signum)
{
	/*  USR1 signal, stop testing */
	if (signum == SIGUSR1) {
		printf("Force Stop!\n");
		stop = 1;
	}

	/*  USR2 signal, print stats */
	if (signum == SIGUSR2)
		stats_display(0);
}

struct rte_mbuf **tx_burst;

uint64_t (*do_measure)(struct lcore_conf *conf,
		       struct rte_mbuf *pkts_burst[],
		       uint64_t total_pkts);

static uint64_t
measure_rxtx(struct lcore_conf *conf,
	     struct rte_mbuf *pkts_burst[],
	     uint64_t total_pkts)
{
	unsigned i, portid, nb_rx, nb_tx;
	uint64_t prev_tsc, cur_tsc;

	prev_tsc = rte_rdtsc();

	while (likely(!stop)) {
		for (i = 0; i < conf->nb_ports; i++) {
			portid = conf->portlist[i];
			nb_rx = rte_eth_rx_burst((uint8_t) portid, 0,
						 pkts_burst, MAX_PKT_BURST);
			if (unlikely(nb_rx == 0)) {
				idle++;
				continue;
			}

			count += nb_rx;
			nb_tx = rte_eth_tx_burst(portid, 0, pkts_burst, nb_rx);
			if (unlikely(nb_tx < nb_rx)) {
				drop += (nb_rx - nb_tx);
				do {
					rte_pktmbuf_free(pkts_burst[nb_tx]);
				} while (++nb_tx < nb_rx);
			}
		}
		if (unlikely(count >= total_pkts))
			break;
	}

	cur_tsc = rte_rdtsc();

	return cur_tsc - prev_tsc;
}

static uint64_t
measure_rxonly(struct lcore_conf *conf,
	       struct rte_mbuf *pkts_burst[],
	       uint64_t total_pkts)
{
	unsigned i, portid, nb_rx, nb_tx;
	uint64_t diff_tsc, cur_tsc;

	diff_tsc = 0;
	while (likely(!stop)) {
		for (i = 0; i < conf->nb_ports; i++) {
			portid = conf->portlist[i];

			cur_tsc = rte_rdtsc();
			nb_rx = rte_eth_rx_burst((uint8_t) portid, 0,
						 pkts_burst, MAX_PKT_BURST);
			if (unlikely(nb_rx == 0)) {
				idle++;
				continue;
			}
			diff_tsc += rte_rdtsc() - cur_tsc;

			count += nb_rx;
			nb_tx = rte_eth_tx_burst(portid, 0, pkts_burst, nb_rx);
			if (unlikely(nb_tx < nb_rx)) {
				drop += (nb_rx - nb_tx);
				do {
					rte_pktmbuf_free(pkts_burst[nb_tx]);
				} while (++nb_tx < nb_rx);
			}
		}
		if (unlikely(count >= total_pkts))
			break;
	}

	return diff_tsc;
}

static uint64_t
measure_txonly(struct lcore_conf *conf,
	       struct rte_mbuf *pkts_burst[],
	       uint64_t total_pkts)
{
	unsigned i, portid, nb_rx, nb_tx;
	uint64_t diff_tsc, cur_tsc;

	printf("do tx measure\n");
	diff_tsc = 0;
	while (likely(!stop)) {
		for (i = 0; i < conf->nb_ports; i++) {
			portid = conf->portlist[i];
			nb_rx = rte_eth_rx_burst((uint8_t) portid, 0,
						 pkts_burst, MAX_PKT_BURST);
			if (unlikely(nb_rx == 0)) {
				idle++;
				continue;
			}

			count += nb_rx;

			cur_tsc = rte_rdtsc();
			nb_tx = rte_eth_tx_burst(portid, 0, pkts_burst, nb_rx);
			if (unlikely(nb_tx < nb_rx)) {
				drop += (nb_rx - nb_tx);
				do {
					rte_pktmbuf_free(pkts_burst[nb_tx]);
				} while (++nb_tx < nb_rx);
			}
			diff_tsc += rte_rdtsc() - cur_tsc;
		}
		if (unlikely(count >= total_pkts))
			break;
	}

	return diff_tsc;
}

/* main processing loop */
static int
main_loop(__rte_unused void *args)
{
#define PACKET_SIZE 64
#define FRAME_GAP 12
#define MAC_PREAMBLE 8
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id;
	unsigned i, portid, nb_rx = 0, nb_tx = 0;
	struct lcore_conf *conf;
	int pkt_per_port;
	uint64_t diff_tsc;
	uint64_t packets_per_second, total_packets;

	lcore_id = rte_lcore_id();
	conf = &lcore_conf[lcore_id];
	if (conf->status != LCORE_USED)
		return 0;

	pkt_per_port = MAX_TRAFFIC_BURST;

	int idx = 0;
	for (i = 0; i < conf->nb_ports; i++) {
		int num = pkt_per_port;
		portid = conf->portlist[i];
		printf("inject %d packet to port %d\n", num, portid);
		while (num) {
			nb_tx = RTE_MIN(MAX_PKT_BURST, num);
			nb_tx = rte_eth_tx_burst(portid, 0,
						&tx_burst[idx], nb_tx);
			num -= nb_tx;
			idx += nb_tx;
		}
	}
	printf("Total packets inject to prime ports = %u\n", idx);

	packets_per_second = (link_mbps * 1000 * 1000) /
		((PACKET_SIZE + FRAME_GAP + MAC_PREAMBLE) * CHAR_BIT);
	printf("Each port will do %"PRIu64" packets per second\n",
	       packets_per_second);

	total_packets = RTE_TEST_DURATION * conf->nb_ports * packets_per_second;
	printf("Test will stop after at least %"PRIu64" packets received\n",
		+ total_packets);

	diff_tsc = do_measure(conf, pkts_burst, total_packets);

	for (i = 0; i < conf->nb_ports; i++) {
		portid = conf->portlist[i];
		int nb_free = pkt_per_port;
		do { /* dry out */
			nb_rx = rte_eth_rx_burst((uint8_t) portid, 0,
						 pkts_burst, MAX_PKT_BURST);
			nb_tx = 0;
			while (nb_tx < nb_rx)
				rte_pktmbuf_free(pkts_burst[nb_tx++]);
			nb_free -= nb_rx;
		} while (nb_free != 0);
		printf("free %d mbuf left in port %u\n", pkt_per_port, portid);
	}

	if (count == 0)
		return -1;

	printf("%"PRIu64" packet, %"PRIu64" drop, %"PRIu64" idle\n",
	       count, drop, idle);
	printf("Result: %"PRIu64" cycles per packet\n", diff_tsc / count);

	return 0;
}

rte_atomic64_t start;

static inline int
poll_burst(void *args)
{
#define MAX_IDLE           (10000)
	unsigned lcore_id;
	struct rte_mbuf **pkts_burst;
	uint64_t diff_tsc, cur_tsc;
	uint16_t next[RTE_MAX_ETHPORTS];
	struct lcore_conf *conf;
	uint32_t pkt_per_port = *((uint32_t *)args);
	unsigned i, portid, nb_rx = 0;
	uint64_t total;
	uint64_t timeout = MAX_IDLE;

	lcore_id = rte_lcore_id();
	conf = &lcore_conf[lcore_id];
	if (conf->status != LCORE_USED)
		return 0;

	total = pkt_per_port * conf->nb_ports;
	printf("start to receive total expect %"PRIu64"\n", total);

	pkts_burst = (struct rte_mbuf **)
		rte_calloc_socket("poll_burst",
				  total, sizeof(void *),
				  CACHE_LINE_SIZE, conf->socketid);
	if (!pkts_burst)
		return -1;

	for (i = 0; i < conf->nb_ports; i++) {
		portid = conf->portlist[i];
		next[portid] = i * pkt_per_port;
	}

	while (!rte_atomic64_read(&start))
		;

	cur_tsc = rte_rdtsc();
	while (total) {
		for (i = 0; i < conf->nb_ports; i++) {
			portid = conf->portlist[i];
			nb_rx = rte_eth_rx_burst((uint8_t) portid, 0,
						 &pkts_burst[next[portid]],
						 MAX_PKT_BURST);
			if (unlikely(nb_rx == 0)) {
				timeout--;
				if (unlikely(timeout == 0))
					goto timeout;
				continue;
			}
			next[portid] += nb_rx;
			total -= nb_rx;
		}
	}
timeout:
	diff_tsc = rte_rdtsc() - cur_tsc;

	printf("%"PRIu64" packets lost, IDLE %"PRIu64" times\n",
	       total, MAX_IDLE - timeout);

	/* clean up */
	total = pkt_per_port * conf->nb_ports - total;
	for (i = 0; i < total; i++)
		rte_pktmbuf_free(pkts_burst[i]);

	rte_free(pkts_burst);

	return diff_tsc / total;
}

static int
exec_burst(uint32_t flags, int lcore)
{
	unsigned i, portid, nb_tx = 0;
	struct lcore_conf *conf;
	uint32_t pkt_per_port;
	int num, idx = 0;
	int diff_tsc;

	conf = &lcore_conf[lcore];

	pkt_per_port = MAX_TRAFFIC_BURST;
	num = pkt_per_port;

	rte_atomic64_init(&start);

	/* start polling thread, but not actually poll yet */
	rte_eal_remote_launch(poll_burst,
			      (void *)&pkt_per_port, lcore);

	/* Only when polling first */
	if (flags == SC_BURST_POLL_FIRST)
		rte_atomic64_set(&start, 1);

	/* start xmit */
	while (num) {
		nb_tx = RTE_MIN(MAX_PKT_BURST, num);
		for (i = 0; i < conf->nb_ports; i++) {
			portid = conf->portlist[i];
			rte_eth_tx_burst(portid, 0,
					 &tx_burst[idx], nb_tx);
			idx += nb_tx;
		}
		num -= nb_tx;
	}

	sleep(5);

	/* only when polling second  */
	if (flags == SC_BURST_XMIT_FIRST)
		rte_atomic64_set(&start, 1);

	/* wait for polling finished */
	diff_tsc = rte_eal_wait_lcore(lcore);
	if (diff_tsc < 0)
		return -1;

	printf("Result: %d cycles per packet\n", diff_tsc);

	return 0;
}

static int
test_pmd_perf(void)
{
	uint16_t nb_ports, num, nb_lcores, slave_id = (uint16_t)-1;
	uint16_t nb_rxd = MAX_TRAFFIC_BURST;
	uint16_t nb_txd = MAX_TRAFFIC_BURST;
	uint16_t portid;
	uint16_t nb_rx_queue = 1, nb_tx_queue = 1;
	int socketid = -1;
	int ret;

	printf("Start PMD RXTX cycles cost test.\n");

	signal(SIGUSR1, signal_handler);
	signal(SIGUSR2, signal_handler);

	nb_ports = rte_eth_dev_count();
	if (nb_ports < NB_ETHPORTS_USED) {
		printf("At least %u port(s) used for perf. test\n",
		       NB_ETHPORTS_USED);
		return -1;
	}

	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;

	nb_lcores = rte_lcore_count();

	memset(lcore_conf, 0, sizeof(lcore_conf));
	init_lcores();

	init_mbufpool(NB_MBUF);

	if (sc_flag == SC_CONTINUOUS) {
		nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
		nb_txd = RTE_TEST_TX_DESC_DEFAULT;
	}
	printf("CONFIG RXD=%d TXD=%d\n", nb_rxd, nb_txd);

	reset_count();
	num = 0;
	for (portid = 0; portid < nb_ports; portid++) {
		if (socketid == -1) {
			socketid = rte_eth_dev_socket_id(portid);
			slave_id = alloc_lcore(socketid);
			if (slave_id == (uint16_t)-1) {
				printf("No avail lcore to run test\n");
				return -1;
			}
			printf("Performance test runs on lcore %u socket %u\n",
			       slave_id, socketid);
		}

		if (socketid != rte_eth_dev_socket_id(portid)) {
			printf("Skip port %d\n", portid);
			continue;
		}

		/* port configure */
		ret = rte_eth_dev_configure(portid, nb_rx_queue,
					    nb_tx_queue, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"Cannot configure device: err=%d, port=%d\n",
				 ret, portid);

		rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
		printf("Port %u ", portid);
		print_ethaddr("Address:", &ports_eth_addr[portid]);
		printf("\n");

		/* tx queue setup */
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
					     socketid, &tx_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"rte_eth_tx_queue_setup: err=%d, "
				"port=%d\n", ret, portid);

		/* rx queue steup */
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
						socketid, &rx_conf,
						mbufpool[socketid]);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: err=%d,"
				 "port=%d\n", ret, portid);

		/* Start device */
		stop = 0;
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"rte_eth_dev_start: err=%d, port=%d\n",
				ret, portid);

		/* always eanble promiscuous */
		rte_eth_promiscuous_enable(portid);

		lcore_conf[slave_id].portlist[num++] = portid;
		lcore_conf[slave_id].nb_ports++;
	}
	check_all_ports_link_status(nb_ports, RTE_PORT_ALL);

	if (tx_burst == NULL) {
		tx_burst = (struct rte_mbuf **)
			rte_calloc_socket("tx_buff",
					  MAX_TRAFFIC_BURST * nb_ports,
					  sizeof(void *),
					  CACHE_LINE_SIZE, socketid);
		if (!tx_burst)
			return -1;
	}

	init_traffic(mbufpool[socketid],
		     tx_burst, MAX_TRAFFIC_BURST * nb_ports);

	printf("Generate %d packets @socket %d\n",
	       MAX_TRAFFIC_BURST * nb_ports, socketid);

	if (sc_flag == SC_CONTINUOUS) {
		/* do both rxtx by default */
		if (NULL == do_measure)
			do_measure = measure_rxtx;

		rte_eal_remote_launch(main_loop, NULL, slave_id);

		if (rte_eal_wait_lcore(slave_id) < 0)
			return -1;
	} else if (sc_flag == SC_BURST_POLL_FIRST ||
		   sc_flag == SC_BURST_XMIT_FIRST)
		exec_burst(sc_flag, slave_id);

	/* port tear down */
	for (portid = 0; portid < nb_ports; portid++) {
		if (socketid != rte_eth_dev_socket_id(portid))
			continue;

		rte_eth_dev_stop(portid);
	}

	return 0;
}

int
test_set_rxtx_conf(cmdline_fixed_string_t mode)
{
	printf("mode switch to %s\n", mode);

	if (!strcmp(mode, "vector")) {
		/* vector rx, tx */
		tx_conf.txq_flags = 0xf01;
		tx_conf.tx_rs_thresh = 32;
		tx_conf.tx_free_thresh = 32;
		port_conf.rxmode.hw_ip_checksum = 0;
		port_conf.rxmode.enable_scatter = 0;
		return 0;
	} else if (!strcmp(mode, "scalar")) {
		/* bulk alloc rx, simple tx */
		tx_conf.txq_flags = 0xf01;
		tx_conf.tx_rs_thresh = 128;
		tx_conf.tx_free_thresh = 128;
		port_conf.rxmode.hw_ip_checksum = 1;
		port_conf.rxmode.enable_scatter = 0;
		return 0;
	} else if (!strcmp(mode, "hybrid")) {
		/* bulk alloc rx, vector tx
		 * when vec macro not define,
		 * using the same rx/tx as scalar
		 */
		tx_conf.txq_flags = 0xf01;
		tx_conf.tx_rs_thresh = 32;
		tx_conf.tx_free_thresh = 32;
		port_conf.rxmode.hw_ip_checksum = 1;
		port_conf.rxmode.enable_scatter = 0;
		return 0;
	} else if (!strcmp(mode, "full")) {
		/* full feature rx,tx pair */
		tx_conf.txq_flags = 0x0;   /* must condition */
		tx_conf.tx_rs_thresh = 32;
		tx_conf.tx_free_thresh = 32;
		port_conf.rxmode.hw_ip_checksum = 0;
		port_conf.rxmode.enable_scatter = 1; /* must condition */
		return 0;
	}

	return -1;
}

int
test_set_rxtx_anchor(cmdline_fixed_string_t type)
{
	printf("type switch to %s\n", type);

	if (!strcmp(type, "rxtx")) {
		do_measure = measure_rxtx;
		return 0;
	} else if (!strcmp(type, "rxonly")) {
		do_measure = measure_rxonly;
		return 0;
	} else if (!strcmp(type, "txonly")) {
		do_measure = measure_txonly;
		return 0;
	}

	return -1;
}

int
test_set_rxtx_sc(cmdline_fixed_string_t type)
{
	printf("stream control switch to %s\n", type);

	if (!strcmp(type, "continuous")) {
		sc_flag = SC_CONTINUOUS;
		return 0;
	} else if (!strcmp(type, "poll_before_xmit")) {
		sc_flag = SC_BURST_POLL_FIRST;
		return 0;
	} else if (!strcmp(type, "poll_after_xmit")) {
		sc_flag = SC_BURST_XMIT_FIRST;
		return 0;
	}

	return -1;
}

static struct test_command pmd_perf_cmd = {
	.command = "pmd_perf_autotest",
	.callback = test_pmd_perf,
};
REGISTER_TEST_COMMAND(pmd_perf_cmd);
