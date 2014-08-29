/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2014 Tilera Corporation. All rights reserved.
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
 *     * Neither the name of Tilera Corporation nor the names of its
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

#include <unistd.h>

#include <rte_eal.h>
#include <rte_dev.h>
#include <rte_eal_memconfig.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

#include <arch/mpipe_xaui_def.h>
#include <arch/mpipe_gbe_def.h>

#include <gxio/mpipe.h>

#ifdef RTE_LIBRTE_MPIPE_DEBUG_INIT
#define MPIPE_DEBUG_INIT
#endif
#ifdef RTE_LIBRTE_MPIPE_DEBUG_RX
#define MPIPE_DEBUG_RX
#endif
#ifdef RTE_LIBRTE_MPIPE_DEBUG_TX
#define MPIPE_DEBUG_TX
#endif

#ifdef MPIPE_DEBUG_INIT
#define PMD_INIT_LOG(level, fmt, args...) \
	RTE_LOG(level, PMD, "%s(): " fmt, __func__, ## args)
#define PMD_INIT_FUNC_TRACE() PMD_INIT_LOG(DEBUG, " >>")
#else
#define PMD_INIT_LOG(level, fmt, args...) do { } while(0)
#define PMD_INIT_FUNC_TRACE() do { } while(0)
#endif

#ifdef MPIPE_DEBUG_RX
#define PMD_RX_LOG(level, fmt, args...) \
	RTE_LOG(level, PMD, "%s() rx: " fmt, __func__, ## args)
#else
#define PMD_RX_LOG(level, fmt, args...) do { } while(0)
#endif

#ifdef MPIPE_DEBUG_TX
#define PMD_TX_LOG(level, fmt, args...) \
	RTE_LOG(level, PMD, "%s() tx: " fmt, __func__, ## args)
#else
#define PMD_TX_LOG(level, fmt, args...) do { } while(0)
#endif

#define MPIPE_TX_MAX_QUEUES		128
#define MPIPE_TX_DESCS			512
#define MPIPE_RX_MAX_QUEUES		16
#define MPIPE_RX_BUCKETS		256

#define MPIPE_LINK_UPDATE_TIMEOUT	10	/*  s */
#define MPIPE_LINK_UPDATE_INTERVAL	100000	/* us */
#define MPIPE_BSM_MAX_OFFSET		128

#define MPIPE_XGBE_ENA_HASH_MULTI	\
	(1UL << MPIPE_XAUI_RECEIVE_CONFIGURATION__ENA_HASH_MULTI_SHIFT)
#define MPIPE_XGBE_ENA_HASH_UNI		\
	(1UL << MPIPE_XAUI_RECEIVE_CONFIGURATION__ENA_HASH_UNI_SHIFT)
#define MPIPE_XGBE_COPY_ALL		\
	(1UL << MPIPE_XAUI_RECEIVE_CONFIGURATION__COPY_ALL_SHIFT)
#define MPIPE_GBE_ENA_MULTI_HASH	\
	(1UL << MPIPE_GBE_NETWORK_CONFIGURATION__MULTI_HASH_ENA_SHIFT)
#define MPIPE_GBE_ENA_UNI_HASH		\
	(1UL << MPIPE_GBE_NETWORK_CONFIGURATION__UNI_HASH_ENA_SHIFT)
#define MPIPE_GBE_COPY_ALL		\
	(1UL << MPIPE_GBE_NETWORK_CONFIGURATION__COPY_ALL_SHIFT)

/* Per queue statistics. */
struct mpipe_queue_stats {
	uint64_t packets, bytes, errors;
};

/* Common tx/rx queue fields. */
struct mpipe_queue {
	struct mpipe_dev_priv *priv;	/* "priv" data of its device. */
	uint16_t nb_desc;		/* Number of tx descriptors. */
	uint16_t port_id;		/* Device index. */
	uint16_t stat_idx;		/* Queue stats index. */
	uint8_t queue_idx;		/* Queue index. */
	uint8_t link_status;		/* 0 = link down. */
	struct mpipe_queue_stats stats;	/* Stat data for the queue. */
};

/* Transmit queue description. */
struct mpipe_tx_queue {
	struct mpipe_queue q;		/* Common stuff. */
};

/* Receive queue description. */
struct mpipe_rx_queue {
	struct mpipe_queue q;		/* Common stuff. */
	gxio_mpipe_iqueue_t iqueue;	/* mPIPE iqueue. */
	gxio_mpipe_idesc_t *next_desc;	/* Next idesc to process. */
	int avail_descs;		/* Number of available descs. */
	int meta_size;			/* Size of mbuf structure. */
	int max_rx_batch;		/* Limit on RX batch size. */
	struct rte_mempool *mpool;	/* mpool used by the rx queue. */
	void *rx_ring_mem;		/* DMA ring memory. */
};

struct mpipe_dev_priv {
	gxio_mpipe_context_t *context;	/* mPIPE context. */
	gxio_mpipe_link_t link;		/* mPIPE link for the device. */
	gxio_mpipe_equeue_t equeue;	/* mPIPE equeue. */
	gxio_mpipe_rules_stacks_t stacks; /* mPIPE buffer stacks. */
	unsigned equeue_size;		/* mPIPE equeue desc count. */
	int instance;			/* mPIPE instance. */
	int ering;			/* mPIPE eDMA ring. */
	int channel;			/* Device channel. */
	int port_id;			/* DPDK port index. */
	struct rte_eth_dev *eth_dev;	/* DPDK device. */
	struct rte_pci_device pci_dev;	/* PCI device data. */
	struct rte_mbuf **tx_comps;	/* TX completion array. */
	int is_xaui:1,			/* Is this an xgbe or gbe? */
	    initialized:1,		/* Initialized port? */
	    running:1;			/* Running port? */
	struct ether_addr mac_addr;	/* MAC address. */
	unsigned nb_rx_queues;		/* Configured tx queues. */
	unsigned nb_tx_queues;		/* Configured rx queues. */
	int first_bucket;		/* mPIPE bucket start index. */
	int first_ring;			/* mPIPE notif ring start index. */
	int notif_group;		/* mPIPE notif group. */
	int dp_count;			/* Active datapath thread count. */
	int tx_stat_mapping[RTE_ETHDEV_QUEUE_STAT_CNTRS];
	int rx_stat_mapping[RTE_ETHDEV_QUEUE_STAT_CNTRS];
};

#define mpipe_priv(dev)			\
	((struct mpipe_dev_priv*)(dev)->data->dev_private)

#define mpipe_name(priv)		\
	((priv)->eth_dev->data->name)

#define mpipe_rx_queue(priv, n)		\
	((struct mpipe_rx_queue *)(priv)->eth_dev->data->rx_queues[n])

#define mpipe_tx_queue(priv, n)		\
	((struct mpipe_tx_queue *)(priv)->eth_dev->data->tx_queues[n])

static void
mpipe_xmit_flush(struct mpipe_dev_priv *priv);

static void
mpipe_recv_flush(struct mpipe_rx_queue *rx_queue);

static unsigned mpipe_equeue_sizes[] = {
	GXIO_MPIPE_EQUEUE_ENTRY_512,
	GXIO_MPIPE_EQUEUE_ENTRY_2K,
	GXIO_MPIPE_EQUEUE_ENTRY_8K,
	GXIO_MPIPE_EQUEUE_ENTRY_64K,
	-1
};

static unsigned mpipe_iqueue_sizes[] = {
	GXIO_MPIPE_IQUEUE_ENTRY_128,
	GXIO_MPIPE_IQUEUE_ENTRY_512,
	GXIO_MPIPE_IQUEUE_ENTRY_2K,
	GXIO_MPIPE_IQUEUE_ENTRY_64K,
	-1
};

static int
mpipe_ring_size(unsigned *sizes, unsigned nb_desc)
{
	while (*sizes < nb_desc)
		sizes++;
	return *sizes;
}

static int mpipe_equeue_size(int nb_desc)
{
	return mpipe_ring_size(mpipe_equeue_sizes, nb_desc);
}

static int mpipe_iqueue_size(int nb_desc)
{
	return mpipe_ring_size(mpipe_iqueue_sizes, nb_desc);
}

static inline int
mpipe_dev_atomic_read_link_status(struct rte_eth_dev *dev,
				  struct rte_eth_link *link)
{
	struct rte_eth_link *dst = link;
	struct rte_eth_link *src = &(dev->data->dev_link);;

	if (rte_atomic64_cmpset((uint64_t *)dst, *(uint64_t *)dst,
				*(uint64_t *)src) == 0)
		return -1;

	return 0;
}

static inline int
mpipe_dev_atomic_write_link_status(struct rte_eth_dev *dev,
				   struct rte_eth_link *link)
{
	struct rte_eth_link *dst = &(dev->data->dev_link);
	struct rte_eth_link *src = link;

	if (rte_atomic64_cmpset((uint64_t *)dst, *(uint64_t *)dst,
				*(uint64_t *)src) == 0)
		return -1;

	return 0;
}

static void
mpipe_infos_get(struct rte_eth_dev *dev __rte_unused,
		struct rte_eth_dev_info *dev_info)
{
	dev_info->min_rx_bufsize  = 128;
	dev_info->max_rx_pktlen   = 1518;
	dev_info->max_tx_queues   = MPIPE_TX_MAX_QUEUES;
	dev_info->max_rx_queues   = MPIPE_RX_MAX_QUEUES;
	dev_info->max_mac_addrs   = 1;
	dev_info->rx_offload_capa = 0;
	dev_info->tx_offload_capa = 0;
}

static int
mpipe_configure(struct rte_eth_dev *dev)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);

	if (dev->data->nb_tx_queues > MPIPE_TX_MAX_QUEUES) {
		RTE_LOG(ERR, PMD, "%s: Too many tx queues: %d > %d\n",
			mpipe_name(priv), dev->data->nb_tx_queues,
			MPIPE_TX_MAX_QUEUES);
		return -EINVAL;
	}
	priv->nb_tx_queues = dev->data->nb_tx_queues;

	if (dev->data->nb_rx_queues > MPIPE_RX_MAX_QUEUES) {
		RTE_LOG(ERR, PMD, "%s: Too many rx queues: %d > %d\n",
			mpipe_name(priv), dev->data->nb_rx_queues,
			MPIPE_RX_MAX_QUEUES);
	}
	priv->nb_rx_queues = dev->data->nb_rx_queues;

	return 0;
}

static inline int
mpipe_link_compare(struct rte_eth_link *link1,
		   struct rte_eth_link *link2)
{
	return ((*(uint64_t *)link1 == *(uint64_t *)link2)
		? -1 : 0);
}

static int
mpipe_link_update(struct rte_eth_dev *dev, int wait_to_complete)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	struct rte_eth_link old, new;
	int64_t state, speed;
	int count, rc;

	memset(&old, 0, sizeof(old));
	memset(&new, 0, sizeof(new));
	mpipe_dev_atomic_read_link_status(dev, &old);

	for (count = 0, rc = 0; count < MPIPE_LINK_UPDATE_TIMEOUT; count++) {
		if (!priv->initialized)
			break;

		state = gxio_mpipe_link_get_attr(&priv->link,
						 GXIO_MPIPE_LINK_CURRENT_STATE);
		if (state < 0)
			break;

		speed = state & GXIO_MPIPE_LINK_SPEED_MASK;

		if (speed == GXIO_MPIPE_LINK_1G) {
			new.link_speed = ETH_LINK_SPEED_1000;
			new.link_duplex = ETH_LINK_FULL_DUPLEX;
			new.link_status = 1;
		} else if (speed == GXIO_MPIPE_LINK_10G) {
			new.link_speed = ETH_LINK_SPEED_10000;
			new.link_duplex = ETH_LINK_FULL_DUPLEX;
			new.link_status = 1;
		}

		rc = mpipe_link_compare(&old, &new);
		if (rc == 0 || !wait_to_complete)
			break;

		rte_delay_us(MPIPE_LINK_UPDATE_INTERVAL);
	}

	mpipe_dev_atomic_write_link_status(dev, &new);
	return rc;
}

static int
mpipe_set_link(struct rte_eth_dev *dev, int up)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	int rc;

	rc = gxio_mpipe_link_set_attr(&priv->link,
				      GXIO_MPIPE_LINK_DESIRED_STATE,
				      up ? GXIO_MPIPE_LINK_ANYSPEED : 0);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to set link %s on %s.\n",
			up ? "up" : "down", mpipe_name(priv));
	} else {
		mpipe_link_update(dev, 0);
	}

	return rc;
}

static int
mpipe_set_link_up(struct rte_eth_dev *dev)
{
	return mpipe_set_link(dev, 1);
}

static int
mpipe_set_link_down(struct rte_eth_dev *dev)
{
	return mpipe_set_link(dev, 0);
}

static inline int
mpipe_dp_count(struct mpipe_dev_priv *priv, int adjust)
{
	/*
	 * We could in theory have used atomic increment/decrement operations
	 * here, but we cheat as follows to avoid the implicit memory barrier
	 * in GCC sync operations.
	 */
	return arch_atomic_add(&priv->dp_count, adjust);
}

static inline void
mpipe_dp_enter(struct mpipe_dev_priv *priv)
{
	__insn_mtspr(SPR_DSTREAM_PF, 0);
	mpipe_dp_count(priv, 1);
}

static inline void
mpipe_dp_exit(struct mpipe_dev_priv *priv)
{
	mpipe_dp_count(priv, -1);
}

static inline void
mpipe_dp_wait(struct mpipe_dev_priv *priv)
{
	while (mpipe_dp_count(priv, 0) != 0) {
		rte_pause();
	}
}

static int
mpipe_recv_init(struct mpipe_dev_priv *priv)
{
	int rc;

	/* Allocate one NotifRing for each queue. */
	rc = gxio_mpipe_alloc_notif_rings(priv->context, MPIPE_RX_MAX_QUEUES,
					  0, 0);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to allocate notif rings for %s.\n",
			mpipe_name(priv));
		return rc;
	}
	priv->first_ring = rc;

	/* Allocate a NotifGroup. */
	rc = gxio_mpipe_alloc_notif_groups(priv->context, 1, 0, 0);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to allocate rx group for %s.\n",
			mpipe_name(priv));
		return rc;
	}
	priv->notif_group = rc;

	/* Allocate required buckets. */
	rc = gxio_mpipe_alloc_buckets(priv->context, MPIPE_RX_BUCKETS, 0, 0);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to allocate buckets for %s.\n",
			mpipe_name(priv));
		return rc;
	}
	priv->first_bucket = rc;

	return 0;
}

static int
mpipe_xmit_init(struct mpipe_dev_priv *priv)
{
	size_t ring_size;
	void *ring_mem;
	int rc;

	/* Allocate eDMA ring. */
	rc = gxio_mpipe_alloc_edma_rings(priv->context, 1, 0, 0);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to alloc tx ring for %s.\n",
			mpipe_name(priv));
		return rc;
	}
	priv->ering = rc;

	rc = mpipe_equeue_size(MPIPE_TX_DESCS);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Cannot allocate %d equeue descs.\n",
			(int)MPIPE_TX_DESCS);
		return -ENOMEM;
	}
	priv->equeue_size = rc;

	/* Initialize completion array. */
	ring_size = sizeof(priv->tx_comps[0]) * priv->equeue_size;
	priv->tx_comps = rte_zmalloc(NULL, ring_size, CACHE_LINE_SIZE);
	if (!priv->tx_comps) {
		RTE_LOG(ERR, PMD, "Failed to allocate egress completions for %s.\n",
			mpipe_name(priv));
		return -ENOMEM;
	}

	/* Allocate eDMA ring memory. */
	ring_size = sizeof(gxio_mpipe_edesc_t) * priv->equeue_size;
	ring_mem = rte_zmalloc(NULL, ring_size, ring_size);
	if (!ring_mem) {
		RTE_LOG(ERR, PMD, "Failed to allocate egress descs for %s.\n",
			mpipe_name(priv));
		return -ENOMEM;
	}

	/* Initialize eDMA ring. */
	rc = gxio_mpipe_equeue_init(&priv->equeue, priv->context, priv->ering,
				    priv->channel, ring_mem, ring_size, 0);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to init equeue for %s\n",
			mpipe_name(priv));
		return rc;
	}

	return 0;
}

static int
mpipe_link_init(struct mpipe_dev_priv *priv)
{
	int rc;

	/* Open the link. */
	rc = gxio_mpipe_link_open(&priv->link, priv->context,
				  mpipe_name(priv), GXIO_MPIPE_LINK_AUTO_NONE);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to open link %s.\n",
			mpipe_name(priv));
		return rc;
	}

	/* Get the channel index. */
	rc = gxio_mpipe_link_channel(&priv->link);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Bad channel for interface %s\n",
			mpipe_name(priv));
		return rc;
	}
	priv->channel = rc;

	return 0;
}

static int
mpipe_init(struct mpipe_dev_priv *priv)
{
	int rc;

	if (priv->initialized)
		return 0;

	rc = mpipe_link_init(priv);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to init link for %s.\n",
			mpipe_name(priv));
		return rc;
	}

	rc = mpipe_recv_init(priv);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to init rx for %s.\n",
			mpipe_name(priv));
		return rc;
	}

	rc = mpipe_xmit_init(priv);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to init tx for %s.\n",
			mpipe_name(priv));
		rte_free(priv);
		return rc;
	}

	priv->initialized = 1;

	return 0;
}

static int
mpipe_start(struct rte_eth_dev *dev)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	struct rte_eal_mpipe_channel_config config;
	struct mpipe_rx_queue *rx_queue;
	struct rte_eth_link eth_link;
	size_t ring_size;
	void *ring_mem;
	unsigned queue;
	int rc;

	memset(&eth_link, 0, sizeof(eth_link));
	mpipe_dev_atomic_write_link_status(dev, &eth_link);

	rc = mpipe_init(priv);
	if (rc < 0)
		return rc;

	/* Initialize NotifRings. */
	for (queue = 0; queue < priv->nb_rx_queues; queue++) {
		rx_queue = mpipe_rx_queue(priv, queue);
		ring_size = rx_queue->q.nb_desc * sizeof(gxio_mpipe_idesc_t);

		ring_mem = rte_malloc(NULL, ring_size, ring_size);
		if (!ring_mem) {
			RTE_LOG(ERR, PMD, "Failed to alloc rx descs for %s.\n",
				mpipe_name(priv));
			return -ENOMEM;
		}

		rc = gxio_mpipe_iqueue_init(&rx_queue->iqueue, priv->context,
					    priv->first_ring + queue, ring_mem,
					    ring_size, 0);
		if (rc < 0) {
			RTE_LOG(ERR, PMD, "Failed to init rx queue for %s.\n",
				mpipe_name(priv));
			return rc;
		}

		rx_queue->rx_ring_mem = ring_mem;
	}

	/* Initialize ingress NotifGroup and buckets. */
	rc = gxio_mpipe_init_notif_group_and_buckets(priv->context,
			priv->notif_group, priv->first_ring, priv->nb_rx_queues,
			priv->first_bucket, MPIPE_RX_BUCKETS,
			GXIO_MPIPE_BUCKET_STATIC_FLOW_AFFINITY);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to init notif group and buckets for %s.\n",
			mpipe_name(priv));
		return rc;
	}

	/* Configure the classifier to deliver packets from this port. */
	config.enable = 1;
	config.first_bucket = priv->first_bucket;
	config.num_buckets = MPIPE_RX_BUCKETS;
	config.headroom = RTE_PKTMBUF_HEADROOM;
	config.stacks = priv->stacks;

	rc = rte_eal_mpipe_channel_config(priv->instance, priv->channel,
					  &config);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to setup classifier for %s.\n",
			mpipe_name(priv));
		return rc;
	}

	/* Bring up the link. */
	mpipe_set_link_up(dev);

	/* Start xmit/recv on queues. */
	for (queue = 0; queue < priv->nb_tx_queues; queue++)
		mpipe_tx_queue(priv, queue)->q.link_status = 1;
	for (queue = 0; queue < priv->nb_rx_queues; queue++)
		mpipe_rx_queue(priv, queue)->q.link_status = 1;
	priv->running = 1;

	return 0;
}

static void
mpipe_stop(struct rte_eth_dev *dev)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	struct rte_eal_mpipe_channel_config config;
	unsigned queue;
	int rc;

	for (queue = 0; queue < priv->nb_tx_queues; queue++)
		mpipe_tx_queue(priv, queue)->q.link_status = 0;
	for (queue = 0; queue < priv->nb_rx_queues; queue++)
		mpipe_rx_queue(priv, queue)->q.link_status = 0;

	/* Make sure the link_status writes land. */
	rte_wmb();

	/*
	 * Wait for link_status change to register with straggling datapath
	 * threads.
	 */
	mpipe_dp_wait(priv);

	/* Bring down the link. */
	mpipe_set_link_down(dev);

	/* Remove classifier rules. */
	memset(&config, 0, sizeof(config));
	rc = rte_eal_mpipe_channel_config(priv->instance, priv->channel,
					  &config);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to stop classifier for %s.\n",
			mpipe_name(priv));
	}

	/* Flush completed xmit packets. */
	mpipe_xmit_flush(priv);

	/* Flush packets sitting in recv queues. */
	for (queue = 0; queue < priv->nb_rx_queues; queue++) {
		struct mpipe_rx_queue *rx_queue = mpipe_rx_queue(priv, queue);
		mpipe_recv_flush(rx_queue);
		rte_free(rx_queue->rx_ring_mem);
	}

	priv->running = 0;
}

static void
mpipe_close(struct rte_eth_dev *dev)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	if (priv->running)
		mpipe_stop(dev);
}

static void
mpipe_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	struct mpipe_tx_queue *tx_queue;
	struct mpipe_rx_queue *rx_queue;
	unsigned i;
	uint16_t idx;

	memset(stats, 0, sizeof(*stats));

	for (i = 0; i < priv->nb_tx_queues; i++) {
		tx_queue = mpipe_tx_queue(priv, i);

		stats->opackets += tx_queue->q.stats.packets;
		stats->obytes   += tx_queue->q.stats.bytes;
		stats->oerrors  += tx_queue->q.stats.errors;

		idx = tx_queue->q.stat_idx;
		if (idx != (uint16_t)-1) {
			stats->q_opackets[idx] += tx_queue->q.stats.packets;
			stats->q_obytes[idx]   += tx_queue->q.stats.bytes;
			stats->q_errors[idx]   += tx_queue->q.stats.errors;
		}
	}

	for (i = 0; i < priv->nb_rx_queues; i++) {
		rx_queue = mpipe_rx_queue(priv, i);

		stats->ipackets += rx_queue->q.stats.packets;
		stats->ibytes   += rx_queue->q.stats.bytes;
		stats->ierrors  += rx_queue->q.stats.errors;

		idx = rx_queue->q.stat_idx;
		if (idx != (uint16_t)-1) {
			stats->q_ipackets[idx] += rx_queue->q.stats.packets;
			stats->q_ibytes[idx]   += rx_queue->q.stats.bytes;
			stats->q_errors[idx]   += rx_queue->q.stats.errors;
		}
	}
}

static void
mpipe_stats_reset(struct rte_eth_dev *dev)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	struct mpipe_tx_queue *tx_queue;
	struct mpipe_rx_queue *rx_queue;
	unsigned i;

	for (i = 0; i < priv->nb_tx_queues; i++) {
		tx_queue = mpipe_tx_queue(priv, i);
		memset(&tx_queue->q.stats, 0, sizeof(tx_queue->q.stats));
	}

	for (i = 0; i < priv->nb_rx_queues; i++) {
		rx_queue = mpipe_rx_queue(priv, i);
		memset(&rx_queue->q.stats, 0, sizeof(rx_queue->q.stats));
	}
}

static int
mpipe_queue_stats_mapping_set(struct rte_eth_dev *dev, uint16_t queue_id,
			      uint8_t stat_idx, uint8_t is_rx)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);

	if (is_rx) {
		priv->rx_stat_mapping[stat_idx] = queue_id;
	} else {
		priv->tx_stat_mapping[stat_idx] = queue_id;
	}

	return 0;
}

static int
mpipe_tx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
		     uint16_t nb_desc, unsigned int socket_id __rte_unused,
		     const struct rte_eth_txconf *tx_conf __rte_unused)
{
	struct mpipe_tx_queue *tx_queue = dev->data->tx_queues[queue_idx];
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	uint16_t idx;
	
	tx_queue = rte_realloc(tx_queue, sizeof(*tx_queue), CACHE_LINE_SIZE);
	if (!tx_queue) {
		RTE_LOG(ERR, PMD, "Failed to allocate TX queue.\n");
		return -ENOMEM;
	}

	memset(&tx_queue->q, 0, sizeof(tx_queue->q));
	tx_queue->q.priv = priv;
	tx_queue->q.queue_idx = queue_idx;
	tx_queue->q.port_id = dev->data->port_id;
	tx_queue->q.nb_desc = nb_desc;

	tx_queue->q.stat_idx = -1;
	for (idx = 0; idx < RTE_ETHDEV_QUEUE_STAT_CNTRS; idx++) {
		if (priv->tx_stat_mapping[idx] == queue_idx)
			tx_queue->q.stat_idx = idx;
	}

	dev->data->tx_queues[queue_idx] = tx_queue;

	return 0;
}

static void
mpipe_tx_queue_release(void *_txq)
{
	rte_free(_txq);
}

static unsigned
mpipe_get_max_rx_batch(unsigned meta_size)
{
	static unsigned cache_size;

	if (!cache_size) {
		/* Limit batch to occupy no more than half the cache. */
		cache_size = sysconf(_SC_LEVEL1_DCACHE_SIZE) / 2;
	}

	/* Round up to cache line multiples. */
	meta_size  = CACHE_LINE_ROUNDUP(meta_size);
	/* Assume 2 lines of packet data. */
	meta_size += 2 * CACHE_LINE_SIZE;

	return cache_size / meta_size;
}

static int
mpipe_rx_queue_setup(struct rte_eth_dev *dev, uint16_t queue_idx,
		     uint16_t nb_desc, unsigned int socket_id __rte_unused,
		     const struct rte_eth_rxconf *rx_conf __rte_unused,
		     struct rte_mempool *mp)
{
	struct mpipe_rx_queue *rx_queue = dev->data->rx_queues[queue_idx];
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	uint16_t idx;
	int size, rc;

	rc = mpipe_iqueue_size(nb_desc);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Cannot allocate %d iqueue descs.\n",
			(int)nb_desc);
		return -ENOMEM;
	}

	if (rc != nb_desc) {
		PMD_INIT_LOG(WARNING, "Extending RX descs from %d to %d.\n",
			     (int)nb_desc, rc);
		nb_desc = rc;
	}

	size = sizeof(*rx_queue);
	rx_queue = rte_realloc(rx_queue, size, CACHE_LINE_SIZE);
	if (!rx_queue) {
		RTE_LOG(ERR, PMD, "Failed to allocate RX queue.\n");
		return -ENOMEM;
	}

	memset(&rx_queue->q, 0, sizeof(rx_queue->q));
	rx_queue->q.priv = priv;
	rx_queue->q.nb_desc = nb_desc;
	rx_queue->q.port_id = dev->data->port_id;
	rx_queue->q.queue_idx = queue_idx;
	rx_queue->mpool = mp;
	rx_queue->meta_size = mp->meta_size;
	rx_queue->max_rx_batch = mpipe_get_max_rx_batch(mp->meta_size);

	rx_queue->q.stat_idx = -1;
	for (idx = 0; idx < RTE_ETHDEV_QUEUE_STAT_CNTRS; idx++) {
		if (priv->rx_stat_mapping[idx] == queue_idx)
			rx_queue->q.stat_idx = idx;
	}

	dev->data->rx_queues[queue_idx] = rx_queue;

	if (priv->stacks.stacks[mp->size_code] == 0xff) {
		priv->stacks.stacks[mp->size_code] = mp->stack_idx;
	}

	return 0;
}

static void
mpipe_rx_queue_release(void *_rxq)
{
	rte_free(_rxq);
}

static void
mpipe_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	int64_t reg;
	int addr;

	if (priv->is_xaui) {
		addr = MPIPE_XAUI_RECEIVE_CONFIGURATION;
		reg  = gxio_mpipe_link_mac_rd(&priv->link, addr);
		reg &= ~MPIPE_XGBE_ENA_HASH_MULTI;
		reg &= ~MPIPE_XGBE_ENA_HASH_UNI;
		reg |=  MPIPE_XGBE_COPY_ALL;
		gxio_mpipe_link_mac_wr(&priv->link, addr, reg);
	} else {
		addr = MPIPE_GBE_NETWORK_CONFIGURATION;
		reg  = gxio_mpipe_link_mac_rd(&priv->link, addr);
		reg &= ~MPIPE_GBE_ENA_MULTI_HASH;
		reg &= ~MPIPE_GBE_ENA_UNI_HASH;
		reg |=  MPIPE_GBE_COPY_ALL;
		gxio_mpipe_link_mac_wr(&priv->link, addr, reg);
	}
}

static void
mpipe_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct mpipe_dev_priv *priv = mpipe_priv(dev);
	int64_t reg;
	int addr;

	if (priv->is_xaui) {
		addr = MPIPE_XAUI_RECEIVE_CONFIGURATION;
		reg  = gxio_mpipe_link_mac_rd(&priv->link, addr);
		reg |=  MPIPE_XGBE_ENA_HASH_MULTI;
		reg |=  MPIPE_XGBE_ENA_HASH_UNI;
		reg &= ~MPIPE_XGBE_COPY_ALL;
		gxio_mpipe_link_mac_wr(&priv->link, addr, reg);
	} else {
		addr = MPIPE_GBE_NETWORK_CONFIGURATION;
		reg  = gxio_mpipe_link_mac_rd(&priv->link, addr);
		reg |=  MPIPE_GBE_ENA_MULTI_HASH;
		reg |=  MPIPE_GBE_ENA_UNI_HASH;
		reg &= ~MPIPE_GBE_COPY_ALL;
		gxio_mpipe_link_mac_wr(&priv->link, addr, reg);
	}
}

static struct eth_dev_ops mpipe_dev_ops = {
	.dev_infos_get	         = mpipe_infos_get,
	.dev_configure	         = mpipe_configure,
	.dev_start	         = mpipe_start,
	.dev_stop	         = mpipe_stop,
	.dev_close	         = mpipe_close,
	.stats_get	         = mpipe_stats_get,
	.stats_reset	         = mpipe_stats_reset,
	.queue_stats_mapping_set = mpipe_queue_stats_mapping_set,
	.tx_queue_setup	         = mpipe_tx_queue_setup,
	.rx_queue_setup	         = mpipe_rx_queue_setup,
	.tx_queue_release	 = mpipe_tx_queue_release,
	.rx_queue_release	 = mpipe_rx_queue_release,
	.link_update	         = mpipe_link_update,
	.dev_set_link_up         = mpipe_set_link_up,
	.dev_set_link_down       = mpipe_set_link_down,
	.promiscuous_enable      = mpipe_promiscuous_enable,
	.promiscuous_disable     = mpipe_promiscuous_disable,
};

static inline bool
mpipe_xmit_is_hwb(struct rte_mbuf *mbuf)
{
	/*
	 * The code below is logically equivalent to:
	 *	 return ((mbuf->nb_segs == 1) &&
	 *	         (rte_mbuf_refcnt_read(mbuf) == 1) &&
	 *	         (mbuf->data_off < MPIPE_BSM_MAX_OFFSET));
	 *
	 * However, the form chosen below is less branchy than above, and
	 * therefore performs somewhat better.
	 */
	return !((mbuf->nb_segs - 1) |
		 (rte_mbuf_refcnt_read(mbuf) - 1) |
		 (mbuf->data_off & ~(MPIPE_BSM_MAX_OFFSET - 1)));
}

static inline void
mpipe_xmit_put_slot(struct mpipe_dev_priv *priv, uint16_t slot,
		    struct rte_mbuf *mbuf)
{
	uint16_t idx = slot & (priv->equeue_size - 1);
	struct rte_mbuf **slot_p = &priv->tx_comps[idx];

	mbuf = arch_atomic_exchange(slot_p, mbuf);
	if (unlikely(mbuf != NULL)) {
		rte_pktmbuf_free(mbuf);
	}
}

static inline void
mpipe_xmit_null(struct mpipe_dev_priv *priv, int64_t start, int64_t end)
{
	gxio_mpipe_edesc_t null_desc = { { .bound = 1, .ns = 1 } };
	gxio_mpipe_equeue_t *equeue = &priv->equeue;
	int64_t slot;

	for (slot = start; slot < end; slot++) {
		gxio_mpipe_equeue_put_at(equeue, null_desc, slot);
	}
}

static void
mpipe_xmit_flush(struct mpipe_dev_priv *priv)
{
	gxio_mpipe_equeue_t *equeue = &priv->equeue;
	int64_t slot;

	/* Post a dummy descriptor and wait for its return. */
	slot = gxio_mpipe_equeue_reserve(equeue, 1);
	if (slot < 0) {
		RTE_LOG(ERR, PMD, "Failed to reserve stop slot on %s.\n",
			mpipe_name(priv));
		return;
	}

	mpipe_xmit_null(priv, slot, slot + 1);

	while (!gxio_mpipe_equeue_is_complete(equeue, slot, 1)) {
		rte_pause();
	}

	for (slot = 0; slot < priv->equeue_size; slot++)
		mpipe_xmit_put_slot(priv, slot, NULL);
}

static void
mpipe_recv_flush(struct mpipe_rx_queue *rx_queue)
{
	gxio_mpipe_iqueue_t *iqueue = &rx_queue->iqueue;
	gxio_mpipe_idesc_t idesc;

	while (gxio_mpipe_iqueue_try_get(iqueue, &idesc) >= 0) {
		gxio_mpipe_iqueue_drop(iqueue, &idesc);
	}
}

static inline uint16_t
mpipe_do_xmit(struct mpipe_tx_queue *tx_queue, struct rte_mbuf **tx_pkts,
	      uint16_t nb_pkts)
{
	struct mpipe_dev_priv *priv = tx_queue->q.priv;
	gxio_mpipe_equeue_t *equeue = &priv->equeue;
	unsigned nb_bytes = 0;
	unsigned nb_sent = 0;
	int nb_slots, i;

	PMD_TX_LOG(DEBUG, "Trying to transmit %d packets fast on %s:%d.\n",
		   nb_pkts, mpipe_name(tx_queue->q.priv),
		   tx_queue->q.queue_idx);

	/* Optimistic assumption that we need exactly one slot per packet. */
	nb_slots = nb_pkts;

	do {
		struct rte_mbuf *mbuf = NULL, *pkt = NULL;
		bool is_hwb = false;
		int64_t slot;

		/* Reserve eDMA ring slots. */
		slot = gxio_mpipe_equeue_try_reserve_fast(equeue, nb_slots);
		if (unlikely(slot < 0)) {
			break;
		}

		/* Fill up slots with descriptor and completion info. */
		for (i = 0; i < nb_slots; i++) {
			gxio_mpipe_edesc_t desc;
			struct rte_mbuf *next;

			/* Starting on a new packet? */
			if (likely(!mbuf)) {
				int room = nb_slots - i;

				pkt = mbuf = tx_pkts[nb_sent];
				is_hwb = mpipe_xmit_is_hwb(pkt);

				/* Bail out if we run out of descs. */
				if (unlikely(pkt->nb_segs > room)) {
					break;
				}

				nb_sent++;
			}

			/* We have a segment to send. */
			next = mbuf->next;

			desc = (gxio_mpipe_edesc_t) { {
				.va        = rte_pktmbuf_mtod(mbuf, uintptr_t),
				.stack_idx = mbuf->pool->stack_idx,
				.inst      = mbuf->pool->instance,
				.size      = mbuf->pool->size_code,
				.xfer_size = rte_pktmbuf_data_len(mbuf),
				.bound     = 1,
				.hwb       = 1,
			} };

			if (unlikely(!is_hwb)) {
				desc.hwb   = 0;
				desc.bound = next ? 0 : 1;
				mpipe_xmit_put_slot(priv, slot + i,
						    next ? NULL : pkt);
			}

			nb_bytes += rte_pktmbuf_data_len(mbuf);
			gxio_mpipe_equeue_put_at(equeue, desc, slot + i);

			mbuf = next;
		}

		if (unlikely(nb_sent < nb_pkts)) {

			/* Fill remaining slots with null descriptors. */
			mpipe_xmit_null(priv, slot + i, slot + nb_slots);

			/*
			 * Calculate exact number of descriptors needed for
			 * the next go around.
			 */
			nb_slots = 0;
			for (i = nb_sent; i < nb_pkts; i++) {
				nb_slots += tx_pkts[i]->nb_segs;
			}
		}
	} while (nb_sent < nb_pkts);

	tx_queue->q.stats.packets += nb_sent;
	tx_queue->q.stats.bytes   += nb_bytes;

	return nb_sent;
}

static inline uint16_t
mpipe_do_recv(struct mpipe_rx_queue *rx_queue, struct rte_mbuf **rx_pkts,
	      uint16_t nb_pkts)
{
	gxio_mpipe_iqueue_t *iqueue = &rx_queue->iqueue;
	struct rte_mempool *mpool = rx_queue->mpool;
	gxio_mpipe_idesc_t    *first_idesc, *idesc, *last_idesc;
	const unsigned meta_size = mpool->meta_size;
	uint8_t  in_port = rx_queue->q.port_id;
	const unsigned look_ahead = 8;
	int room = nb_pkts, rc = 0;
	unsigned nb_packets = 0;
	unsigned nb_dropped = 0;
	unsigned nb_bytes = 0;
	unsigned nb_descs, i;

	while (room && !rc) {
		if (rx_queue->avail_descs < room) {
			rc = gxio_mpipe_iqueue_try_peek(iqueue,
							&rx_queue->next_desc);
			rx_queue->avail_descs = rc < 0 ? 0 : rc;
		}

		if (unlikely(!rx_queue->avail_descs)) {
			break;
		}

		nb_descs = RTE_MIN(room, rx_queue->avail_descs);

		first_idesc = rx_queue->next_desc;
		last_idesc  = first_idesc + nb_descs;

		rx_queue->next_desc   += nb_descs;
		rx_queue->avail_descs -= nb_descs;

		for (i = 1; i < look_ahead; i++) {
			rte_prefetch0(first_idesc + i);
		}

		PMD_RX_LOG(DEBUG, "Trying to receive %d packets on %s:%d.\n",
			   nb_descs, mpipe_name(rx_queue->q.priv),
			   rx_queue->q.queue_idx);

		for (idesc = first_idesc; idesc < last_idesc; idesc++) {
			unsigned char *va, *buf;
			struct rte_mbuf *mbuf;
			uint32_t flow_hash;
			uint16_t size;
			uint8_t *ms;

			rte_prefetch0(idesc + look_ahead);

			if (unlikely(gxio_mpipe_idesc_has_error(idesc))) {
				nb_dropped++;
				gxio_mpipe_iqueue_drop(iqueue, idesc);
				continue;
			}

			va   = gxio_mpipe_idesc_get_va(idesc);
			buf  = va - RTE_PKTMBUF_HEADROOM;
			mbuf = __mempool_buf_to_obj(mpool, buf);
			ms   = (uint8_t *)mbuf;

			size = gxio_mpipe_idesc_get_xfer_size(idesc);
			flow_hash = gxio_mpipe_idesc_get_flow_hash(idesc);

			mbuf->data_off = RTE_PKTMBUF_HEADROOM;
			mbuf->nb_segs = 1;
			mbuf->port = in_port;
			mbuf->ol_flags = 0;
			mbuf->packet_type = 0;
			mbuf->data_len = size;
			mbuf->pkt_len = size;
			mbuf->hash.rss = flow_hash;
			rte_mbuf_refcnt_set(mbuf, 1);

			for (i = 0; i < meta_size; i += CACHE_LINE_SIZE) {
				rte_prefetch0(ms + i);
			}
			rte_prefetch0(va);

			/* Update results and statistics counters. */
			rx_pkts[nb_packets] = mbuf;
			nb_bytes += size;
			nb_packets++;
		}

		/*
		 * We release the ring in bursts, but do not track and release
		 * buckets.  This therefore breaks dynamic flow affinity, but
		 * we always operate in static affinity mode, and so we're OK
		 * with this optimization.
		 */
		gxio_mpipe_iqueue_advance(iqueue, nb_descs);
		gxio_mpipe_credit(iqueue->context, iqueue->ring, -1, nb_descs);

		/*
		 * Go around once more if we haven't yet peeked the queue, and
		 * if we have more room to receive.
		 */
		room = nb_pkts - nb_packets;
	}

	rx_queue->q.stats.packets += nb_packets;
	rx_queue->q.stats.bytes   += nb_bytes;
	rx_queue->q.stats.errors  += nb_dropped;

	PMD_RX_LOG(DEBUG, "Received %d packets, %d bytes on %s:%d.\n",
		   nb_descs, rx_bytes, mpipe_name(rx_queue->q.priv),
		   rx_queue->q.queue_idx);

	return nb_packets;
}

static uint16_t
mpipe_recv_pkts(void *_rxq, struct rte_mbuf **rx_pkts, uint16_t nb_pkts)
{
	struct mpipe_rx_queue *rx_queue = _rxq;
	uint16_t result = 0;;

	if (rx_queue) {
		mpipe_dp_enter(rx_queue->q.priv);
		if (likely(rx_queue->q.link_status))
			result = mpipe_do_recv(rx_queue, rx_pkts, nb_pkts);
		mpipe_dp_exit(rx_queue->q.priv);
	}

	return result;
}

static uint16_t
mpipe_xmit_pkts(void *_txq, struct rte_mbuf **tx_pkts, uint16_t nb_pkts)
{
	struct mpipe_tx_queue *tx_queue = _txq;
	uint16_t result = 0;;

	if (tx_queue) {
		mpipe_dp_enter(tx_queue->q.priv);
		if (likely(tx_queue->q.link_status))
			result = mpipe_do_xmit(tx_queue, tx_pkts, nb_pkts);
		mpipe_dp_exit(tx_queue->q.priv);
	}

	return result;
}

static int
mpipe_link_mac(const char *ifname, uint8_t *mac)
{
	int rc, idx;
	char name[GXIO_MPIPE_LINK_NAME_LEN];

	for (idx = 0, rc = 0; !rc; idx++) {
		rc = gxio_mpipe_link_enumerate_mac(idx, name, mac);
		if (!rc && !strncmp(name, ifname, GXIO_MPIPE_LINK_NAME_LEN))
			return 0;
	}
	return -ENODEV;
}

static int
rte_pmd_mpipe_devinit(const char *ifname,
		      const char *params __rte_unused)
{
	gxio_mpipe_context_t *context;
	struct rte_eth_dev *eth_dev;
	struct mpipe_dev_priv *priv;
	int instance, rc;
	uint8_t *mac;

	/* Get the mPIPE instance that the device belongs to. */
	instance = gxio_mpipe_link_instance(ifname);
	context = rte_eal_mpipe_context(instance);
	if (!context) {
		RTE_LOG(ERR, PMD, "No device for link %s.\n", ifname);
		return -ENODEV;
	}

	priv = rte_zmalloc(NULL, sizeof(*priv), 0);
	if (!priv) {
		RTE_LOG(ERR, PMD, "Failed to allocate priv for %s.\n", ifname);
		return -ENOMEM;
	}

	memset(&priv->stacks, 0xff, sizeof(priv->stacks));
	memset(&priv->tx_stat_mapping, 0xff, sizeof(priv->tx_stat_mapping));
	memset(&priv->rx_stat_mapping, 0xff, sizeof(priv->rx_stat_mapping));
	priv->context = context;
	priv->instance = instance;
	priv->is_xaui = (strncmp(ifname, "xgbe", 4) == 0);
	priv->pci_dev.numa_node = instance;
	priv->channel = -1;

	mac = priv->mac_addr.addr_bytes;
	rc = mpipe_link_mac(ifname, mac);
	if (rc < 0) {
		RTE_LOG(ERR, PMD, "Failed to enumerate link %s.\n", ifname);
		rte_free(priv);
		return -ENODEV;
	}

	eth_dev = rte_eth_dev_allocate(ifname);
	if (!eth_dev) {
		RTE_LOG(ERR, PMD, "Failed to allocate device %s.\n", ifname);
		rte_free(priv);
	}

	PMD_INIT_LOG(INFO, "Initialized mpipe device %s "
		     "(mac %02x:%02x:%02x:%02x:%02x:%02x).\n", ifname,
		     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	priv->eth_dev = eth_dev;
	priv->port_id = eth_dev->data->port_id;
	eth_dev->data->dev_private = priv;
	eth_dev->pci_dev = &priv->pci_dev;
	eth_dev->data->mac_addrs = &priv->mac_addr;

	eth_dev->dev_ops      = &mpipe_dev_ops;
	eth_dev->rx_pkt_burst = &mpipe_recv_pkts;
	eth_dev->tx_pkt_burst = &mpipe_xmit_pkts;

	return 0;
}

static struct rte_driver pmd_mpipe_xgbe_drv = {
	.name = "xgbe",
	.type = PMD_VDEV,
	.init = rte_pmd_mpipe_devinit,
};

static struct rte_driver pmd_mpipe_gbe_drv = {
	.name = "gbe",
	.type = PMD_VDEV,
	.init = rte_pmd_mpipe_devinit,
};

PMD_REGISTER_DRIVER(pmd_mpipe_xgbe_drv);
PMD_REGISTER_DRIVER(pmd_mpipe_gbe_drv);
