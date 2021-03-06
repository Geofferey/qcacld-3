/*
 * Copyright (c) 2011, 2014-2015 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

#include <cdf_net_types.h>      /* CDF_NBUF_EXEMPT_NO_EXEMPTION, etc. */
#include <cdf_nbuf.h>           /* cdf_nbuf_t, etc. */
#include <cdf_util.h>           /* cdf_assert */
#include <cdf_lock.h>           /* cdf_spinlock */
#ifdef QCA_COMPUTE_TX_DELAY
#include <cdf_time.h>           /* cdf_system_ticks */
#endif

#include <ol_htt_tx_api.h>      /* htt_tx_desc_id */

#include <ol_txrx_types.h>      /* ol_txrx_pdev_t */
#include <ol_tx_desc.h>
#include <ol_txrx_internal.h>
#ifdef QCA_SUPPORT_SW_TXRX_ENCAP
#include <ol_txrx_encap.h>      /* OL_TX_RESTORE_HDR, etc */
#endif
#include <ol_txrx.h>

#ifdef QCA_SUPPORT_TXDESC_SANITY_CHECKS
extern uint32_t *g_dbg_htt_desc_end_addr, *g_dbg_htt_desc_start_addr;
#endif

#ifdef QCA_SUPPORT_TXDESC_SANITY_CHECKS
static inline void ol_tx_desc_sanity_checks(struct ol_txrx_pdev_t *pdev,
					struct ol_tx_desc_t *tx_desc)
{
	if (tx_desc->pkt_type != 0xff) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
				   "%s Potential tx_desc corruption pkt_type:0x%x pdev:0x%p",
				   __func__, tx_desc->pkt_type, pdev);
		cdf_assert(0);
	}
	if ((uint32_t *) tx_desc->htt_tx_desc <
		    g_dbg_htt_desc_start_addr
		    || (uint32_t *) tx_desc->htt_tx_desc >
		    g_dbg_htt_desc_end_addr) {
			TXRX_PRINT(TXRX_PRINT_LEVEL_ERR,
				   "%s Potential htt_desc curruption:0x%p pdev:0x%p\n",
				   __func__, tx_desc->htt_tx_desc, pdev);
			cdf_assert(0);
	}
}
static inline void ol_tx_desc_reset_pkt_type(struct ol_tx_desc_t *tx_desc)
{
	tx_desc->pkt_type = 0xff;
}
#ifdef QCA_COMPUTE_TX_DELAY
static inline void ol_tx_desc_compute_delay(struct ol_tx_desc_t *tx_desc)
{
	if (tx_desc->entry_timestamp_ticks != 0xffffffff) {
		TXRX_PRINT(TXRX_PRINT_LEVEL_ERR, "%s Timestamp:0x%x\n",
				   __func__, tx_desc->entry_timestamp_ticks);
		cdf_assert(0);
	}
	tx_desc->entry_timestamp_ticks = cdf_system_ticks();
}
static inline void ol_tx_desc_reset_timestamp(struct ol_tx_desc_t *tx_desc)
{
	tx_desc->entry_timestamp_ticks = 0xffffffff;
}
#endif
#else
static inline void ol_tx_desc_sanity_checks(struct ol_txrx_pdev_t *pdev,
						struct ol_tx_desc_t *tx_desc)
{
	return;
}
static inline void ol_tx_desc_reset_pkt_type(struct ol_tx_desc_t *tx_desc)
{
	return;
}
static inline void ol_tx_desc_compute_delay(struct ol_tx_desc_t *tx_desc)
{
	return;
}
static inline void ol_tx_desc_reset_timestamp(struct ol_tx_desc_t *tx_desc)
{
	return;
}
#endif

#ifndef QCA_LL_TX_FLOW_CONTROL_V2
/**
 * ol_tx_desc_alloc() - allocate descriptor from freelist
 * @pdev: pdev handle
 * @vdev: vdev handle
 *
 * Return: tx descriptor pointer/ NULL in case of error
 */
static
struct ol_tx_desc_t *ol_tx_desc_alloc(struct ol_txrx_pdev_t *pdev,
					     struct ol_txrx_vdev_t *vdev)
{
	struct ol_tx_desc_t *tx_desc = NULL;

	cdf_spin_lock_bh(&pdev->tx_mutex);
	if (pdev->tx_desc.freelist) {
		tx_desc = ol_tx_get_desc_global_pool(pdev);
		ol_tx_desc_sanity_checks(pdev, tx_desc);
		ol_tx_desc_compute_delay(tx_desc);
	}
	cdf_spin_unlock_bh(&pdev->tx_mutex);
	return tx_desc;
}

/**
 * ol_tx_desc_alloc_wrapper() -allocate tx descriptor
 * @pdev: pdev handler
 * @vdev: vdev handler
 * @msdu_info: msdu handler
 *
 * Return: tx descriptor or NULL
 */
struct ol_tx_desc_t *
ol_tx_desc_alloc_wrapper(struct ol_txrx_pdev_t *pdev,
			 struct ol_txrx_vdev_t *vdev,
			 struct ol_txrx_msdu_info_t *msdu_info)
{
	return ol_tx_desc_alloc(pdev, vdev);
}

#else
/**
 * ol_tx_desc_alloc() -allocate tx descriptor
 * @pdev: pdev handler
 * @vdev: vdev handler
 * @pool: flow pool
 *
 * Return: tx descriptor or NULL
 */
static
struct ol_tx_desc_t *ol_tx_desc_alloc(struct ol_txrx_pdev_t *pdev,
				      struct ol_txrx_vdev_t *vdev,
				      struct ol_tx_flow_pool_t *pool)
{
	struct ol_tx_desc_t *tx_desc = NULL;

	if (pool) {
		cdf_spin_lock_bh(&pool->flow_pool_lock);
		if (pool->avail_desc) {
			tx_desc = ol_tx_get_desc_flow_pool(pool);
			if (cdf_unlikely(pool->avail_desc < pool->stop_th)) {
				pool->status = FLOW_POOL_ACTIVE_PAUSED;
				cdf_spin_unlock_bh(&pool->flow_pool_lock);
				/* pause network queues */
				pdev->pause_cb(vdev->vdev_id,
					       WLAN_STOP_ALL_NETIF_QUEUE,
					       WLAN_DATA_FLOW_CONTROL);
			} else {
				cdf_spin_unlock_bh(&pool->flow_pool_lock);
			}
			ol_tx_desc_sanity_checks(pdev, tx_desc);
			ol_tx_desc_compute_delay(tx_desc);
		} else {
			cdf_spin_unlock_bh(&pool->flow_pool_lock);
			pdev->pool_stats.pkt_drop_no_desc++;
		}
	} else {
		pdev->pool_stats.pkt_drop_no_pool++;
	}

	return tx_desc;
}

/**
 * ol_tx_desc_alloc_wrapper() -allocate tx descriptor
 * @pdev: pdev handler
 * @vdev: vdev handler
 * @msdu_info: msdu handler
 *
 * Return: tx descriptor or NULL
 */
#ifdef QCA_LL_TX_FLOW_GLOBAL_MGMT_POOL
struct ol_tx_desc_t *
ol_tx_desc_alloc_wrapper(struct ol_txrx_pdev_t *pdev,
			 struct ol_txrx_vdev_t *vdev,
			 struct ol_txrx_msdu_info_t *msdu_info)
{
	if (cdf_unlikely(msdu_info->htt.info.frame_type == htt_pkt_type_mgmt))
		return ol_tx_desc_alloc(pdev, vdev, pdev->mgmt_pool);
	else
		return ol_tx_desc_alloc(pdev, vdev, vdev->pool);
}
#else
struct ol_tx_desc_t *
ol_tx_desc_alloc_wrapper(struct ol_txrx_pdev_t *pdev,
			 struct ol_txrx_vdev_t *vdev,
			 struct ol_txrx_msdu_info_t *msdu_info)
{
	return ol_tx_desc_alloc(pdev, vdev, vdev->pool);
}
#endif
#endif

#ifndef QCA_LL_TX_FLOW_CONTROL_V2
/**
 * ol_tx_desc_free() - put descriptor to freelist
 * @pdev: pdev handle
 * @tx_desc: tx descriptor
 *
 * Return: None
 */
void ol_tx_desc_free(struct ol_txrx_pdev_t *pdev, struct ol_tx_desc_t *tx_desc)
{
	cdf_spin_lock_bh(&pdev->tx_mutex);
#if defined(FEATURE_TSO)
	if (tx_desc->pkt_type == ol_tx_frm_tso) {
		if (cdf_unlikely(tx_desc->tso_desc == NULL))
			cdf_print("%s %d TSO desc is NULL!\n",
				 __func__, __LINE__);
		else
			ol_tso_free_segment(pdev, tx_desc->tso_desc);
	}
#endif
	ol_tx_desc_reset_pkt_type(tx_desc);
	ol_tx_desc_reset_timestamp(tx_desc);

	ol_tx_put_desc_global_pool(pdev, tx_desc);
	cdf_spin_unlock_bh(&pdev->tx_mutex);
}

#else
/**
 * ol_tx_desc_free() - put descriptor to pool freelist
 * @pdev: pdev handle
 * @tx_desc: tx descriptor
 *
 * Return: None
 */
void ol_tx_desc_free(struct ol_txrx_pdev_t *pdev, struct ol_tx_desc_t *tx_desc)
{
	struct ol_tx_flow_pool_t *pool = tx_desc->pool;

#if defined(FEATURE_TSO)
	if (tx_desc->pkt_type == ol_tx_frm_tso) {
		if (cdf_unlikely(tx_desc->tso_desc == NULL))
			cdf_print("%s %d TSO desc is NULL!\n",
				 __func__, __LINE__);
		else
			ol_tso_free_segment(pdev, tx_desc->tso_desc);
	}
#endif
	ol_tx_desc_reset_pkt_type(tx_desc);
	ol_tx_desc_reset_timestamp(tx_desc);

	cdf_spin_lock_bh(&pool->flow_pool_lock);
	ol_tx_put_desc_flow_pool(pool, tx_desc);
	switch (pool->status) {
	case FLOW_POOL_ACTIVE_PAUSED:
		if (pool->avail_desc > pool->start_th) {
			pdev->pause_cb(pool->member_flow_id,
				       WLAN_WAKE_ALL_NETIF_QUEUE,
				       WLAN_DATA_FLOW_CONTROL);
			pool->status = FLOW_POOL_ACTIVE_UNPAUSED;
		}
		break;
	case FLOW_POOL_INVALID:
		if (pool->avail_desc == pool->flow_pool_size) {
			cdf_spin_unlock_bh(&pool->flow_pool_lock);
			ol_tx_free_invalid_flow_pool(pool);
			cdf_print("%s %d pool is INVALID State!!\n",
				 __func__, __LINE__);
			return;
		}
		break;
	case FLOW_POOL_ACTIVE_UNPAUSED:
		break;
	default:
		cdf_print("%s %d pool is INACTIVE State!!\n",
				 __func__, __LINE__);
		break;
	};
	cdf_spin_unlock_bh(&pool->flow_pool_lock);

}
#endif

extern void
dump_frag_desc(char *msg, struct ol_tx_desc_t *tx_desc);

void
dump_pkt(cdf_nbuf_t nbuf, uint32_t nbuf_paddr, int len)
{
	cdf_print("%s: Pkt: VA 0x%p PA 0x%x len %d\n", __func__,
		  cdf_nbuf_data(nbuf), nbuf_paddr, len);
	print_hex_dump(KERN_DEBUG, "Pkt:   ", DUMP_PREFIX_NONE, 16, 4,
		       cdf_nbuf_data(nbuf), len, true);
}

const uint32_t htt_to_ce_pkt_type[] = {
	[htt_pkt_type_raw] = tx_pkt_type_raw,
	[htt_pkt_type_native_wifi] = tx_pkt_type_native_wifi,
	[htt_pkt_type_ethernet] = tx_pkt_type_802_3,
	[htt_pkt_type_mgmt] = tx_pkt_type_mgmt,
	[htt_pkt_type_eth2] = tx_pkt_type_eth2,
	[htt_pkt_num_types] = 0xffffffff
};

struct ol_tx_desc_t *ol_tx_desc_ll(struct ol_txrx_pdev_t *pdev,
				   struct ol_txrx_vdev_t *vdev,
				   cdf_nbuf_t netbuf,
				   struct ol_txrx_msdu_info_t *msdu_info)
{
	struct ol_tx_desc_t *tx_desc;
	unsigned int i;
	uint32_t num_frags;

	msdu_info->htt.info.vdev_id = vdev->vdev_id;
	msdu_info->htt.action.cksum_offload = cdf_nbuf_get_tx_cksum(netbuf);
	switch (cdf_nbuf_get_exemption_type(netbuf)) {
	case CDF_NBUF_EXEMPT_NO_EXEMPTION:
	case CDF_NBUF_EXEMPT_ON_KEY_MAPPING_KEY_UNAVAILABLE:
		/* We want to encrypt this frame */
		msdu_info->htt.action.do_encrypt = 1;
		break;
	case CDF_NBUF_EXEMPT_ALWAYS:
		/* We don't want to encrypt this frame */
		msdu_info->htt.action.do_encrypt = 0;
		break;
	default:
		cdf_assert(0);
		break;
	}

	/* allocate the descriptor */
	tx_desc = ol_tx_desc_alloc_wrapper(pdev, vdev, msdu_info);
	if (!tx_desc)
		return NULL;

	/* initialize the SW tx descriptor */
	tx_desc->netbuf = netbuf;

	if (msdu_info->tso_info.is_tso) {
		tx_desc->tso_desc = msdu_info->tso_info.curr_seg;
		tx_desc->pkt_type = ol_tx_frm_tso;
		TXRX_STATS_MSDU_INCR(pdev, tx.tso.tso_pkts, netbuf);
	} else {
		tx_desc->pkt_type = ol_tx_frm_std;
	}

	/* initialize the HW tx descriptor */

	htt_tx_desc_init(pdev->htt_pdev, tx_desc->htt_tx_desc,
			 tx_desc->htt_tx_desc_paddr,
			 ol_tx_desc_id(pdev, tx_desc), netbuf, &msdu_info->htt,
			 &msdu_info->tso_info,
			 NULL, vdev->opmode == wlan_op_mode_ocb);

	/*
	 * Initialize the fragmentation descriptor.
	 * Skip the prefix fragment (HTT tx descriptor) that was added
	 * during the call to htt_tx_desc_init above.
	 */
	num_frags = cdf_nbuf_get_num_frags(netbuf);
	/* num_frags are expected to be 2 max */
	num_frags = (num_frags > CVG_NBUF_MAX_EXTRA_FRAGS)
		? CVG_NBUF_MAX_EXTRA_FRAGS
		: num_frags;
#if defined(HELIUMPLUS_PADDR64)
	/*
	 * Use num_frags - 1, since 1 frag is used to store
	 * the HTT/HTC descriptor
	 * Refer to htt_tx_desc_init()
	 */
	htt_tx_desc_num_frags(pdev->htt_pdev, tx_desc->htt_frag_desc,
			      num_frags - 1);
#else /* ! defined(HELIUMPLUSPADDR64) */
	htt_tx_desc_num_frags(pdev->htt_pdev, tx_desc->htt_tx_desc,
			      num_frags - 1);
#endif /* defined(HELIUMPLUS_PADDR64) */

	if (msdu_info->tso_info.is_tso) {
		htt_tx_desc_fill_tso_info(pdev->htt_pdev,
			 tx_desc->htt_frag_desc, &msdu_info->tso_info);
		TXRX_STATS_TSO_SEG_UPDATE(pdev,
			 msdu_info->tso_info.curr_seg->seg);
	} else {
		for (i = 1; i < num_frags; i++) {
			cdf_size_t frag_len;
			uint32_t frag_paddr;

			frag_len = cdf_nbuf_get_frag_len(netbuf, i);
			frag_paddr = cdf_nbuf_get_frag_paddr_lo(netbuf, i);
#if defined(HELIUMPLUS_PADDR64)
			htt_tx_desc_frag(pdev->htt_pdev, tx_desc->htt_frag_desc, i - 1,
				 frag_paddr, frag_len);
#if defined(HELIUMPLUS_DEBUG)
			cdf_print("%s:%d: htt_fdesc=%p frag_paddr=%u len=%zu\n",
					  __func__, __LINE__, tx_desc->htt_frag_desc,
					  frag_paddr, frag_len);
			dump_pkt(netbuf, frag_paddr, 64);
#endif /* HELIUMPLUS_DEBUG */
#else /* ! defined(HELIUMPLUSPADDR64) */
			htt_tx_desc_frag(pdev->htt_pdev, tx_desc->htt_tx_desc, i - 1,
							 frag_paddr, frag_len);
#endif /* defined(HELIUMPLUS_PADDR64) */
		}
	}

#if defined(HELIUMPLUS_DEBUG)
	dump_frag_desc("ol_tx_desc_ll()", tx_desc);
#endif
	return tx_desc;
}

void ol_tx_desc_frame_list_free(struct ol_txrx_pdev_t *pdev,
				ol_tx_desc_list *tx_descs, int had_error)
{
	struct ol_tx_desc_t *tx_desc, *tmp;
	cdf_nbuf_t msdus = NULL;

	TAILQ_FOREACH_SAFE(tx_desc, tx_descs, tx_desc_list_elem, tmp) {
		cdf_nbuf_t msdu = tx_desc->netbuf;

		cdf_atomic_init(&tx_desc->ref_cnt);   /* clear the ref cnt */
#ifdef QCA_SUPPORT_SW_TXRX_ENCAP
		/* restore original hdr offset */
		OL_TX_RESTORE_HDR(tx_desc, msdu);
#endif
		cdf_nbuf_unmap(pdev->osdev, msdu, CDF_DMA_TO_DEVICE);
		/* free the tx desc */
		ol_tx_desc_free(pdev, tx_desc);
		/* link the netbuf into a list to free as a batch */
		cdf_nbuf_set_next(msdu, msdus);
		msdus = msdu;
	}
	/* free the netbufs as a batch */
	cdf_nbuf_tx_free(msdus, had_error);
}

void ol_tx_desc_frame_free_nonstd(struct ol_txrx_pdev_t *pdev,
				  struct ol_tx_desc_t *tx_desc, int had_error)
{
	int mgmt_type;
	ol_txrx_mgmt_tx_cb ota_ack_cb;
	char *trace_str;

	cdf_atomic_init(&tx_desc->ref_cnt);     /* clear the ref cnt */
#ifdef QCA_SUPPORT_SW_TXRX_ENCAP
	/* restore original hdr offset */
	OL_TX_RESTORE_HDR(tx_desc, (tx_desc->netbuf));
#endif
	trace_str = (had_error) ? "OT:C:F:" : "OT:C:S:";
	cdf_nbuf_trace_update(tx_desc->netbuf, trace_str);
	if (tx_desc->pkt_type == ol_tx_frm_no_free) {
		/* free the tx desc but don't unmap or free the frame */
		if (pdev->tx_data_callback.func) {
			cdf_nbuf_set_next(tx_desc->netbuf, NULL);
			pdev->tx_data_callback.func(pdev->tx_data_callback.ctxt,
						    tx_desc->netbuf, had_error);
			ol_tx_desc_free(pdev, tx_desc);
			return;
		}
		/* let the code below unmap and free the frame */
	}
	cdf_nbuf_unmap(pdev->osdev, tx_desc->netbuf, CDF_DMA_TO_DEVICE);
	/* check the frame type to see what kind of special steps are needed */
	if ((tx_desc->pkt_type >= OL_TXRX_MGMT_TYPE_BASE) &&
		   (tx_desc->pkt_type != 0xff)) {
		uint32_t frag_desc_paddr_lo = 0;

#if defined(HELIUMPLUS_PADDR64)
		frag_desc_paddr_lo = tx_desc->htt_frag_desc_paddr;
		/* FIX THIS -
		 * The FW currently has trouble using the host's fragments
		 * table for management frames.  Until this is fixed,
		 * rather than specifying the fragment table to the FW,
		 * the host SW will specify just the address of the initial
		 * fragment.
		 * Now that the mgmt frame is done, the HTT tx desc's frags
		 * table pointer needs to be reset.
		 */
#if defined(HELIUMPLUS_DEBUG)
		cdf_print("%s %d: Frag Descriptor Reset [%d] to 0x%x\n",
			  __func__, __LINE__, tx_desc->id,
			  frag_desc_paddr_lo);
#endif /* HELIUMPLUS_DEBUG */
#endif /* HELIUMPLUS_PADDR64 */
		htt_tx_desc_frags_table_set(pdev->htt_pdev,
					    tx_desc->htt_tx_desc, 0,
					    frag_desc_paddr_lo, 1);

		mgmt_type = tx_desc->pkt_type - OL_TXRX_MGMT_TYPE_BASE;
		/*
		 *  we already checked the value when the mgmt frame was
		 *  provided to the txrx layer.
		 *  no need to check it a 2nd time.
		 */
		ota_ack_cb = pdev->tx_mgmt.callbacks[mgmt_type].ota_ack_cb;
		if (ota_ack_cb) {
			void *ctxt;
			ctxt = pdev->tx_mgmt.callbacks[mgmt_type].ctxt;
			ota_ack_cb(ctxt, tx_desc->netbuf, had_error);
		}
		/* free the netbuf */
		cdf_nbuf_free(tx_desc->netbuf);
	} else {
		/* single regular frame */
		cdf_nbuf_set_next(tx_desc->netbuf, NULL);
		cdf_nbuf_tx_free(tx_desc->netbuf, had_error);
	}
	/* free the tx desc */
	ol_tx_desc_free(pdev, tx_desc);
}

#if defined(FEATURE_TSO)
/**
 * htt_tso_alloc_segment() - function to allocate a TSO segment
 * element
 * @pdev:   HTT pdev
 * @tso_seg:    This is the output. The TSO segment element.
 *
 * Allocates a TSO segment element from the free list held in
 * the HTT pdev
 *
 * Return: none
 */
struct cdf_tso_seg_elem_t *ol_tso_alloc_segment(struct ol_txrx_pdev_t *pdev)
{
	struct cdf_tso_seg_elem_t *tso_seg = NULL;

	cdf_spin_lock_bh(&pdev->tso_seg_pool.tso_mutex);
	if (pdev->tso_seg_pool.freelist) {
		pdev->tso_seg_pool.num_free--;
		tso_seg = pdev->tso_seg_pool.freelist;
		pdev->tso_seg_pool.freelist = pdev->tso_seg_pool.freelist->next;
	}
	cdf_spin_unlock_bh(&pdev->tso_seg_pool.tso_mutex);

	return tso_seg;
}

/**
 * ol_tso_free_segment() - function to free a TSO segment
 * element
 * @pdev:   HTT pdev
 * @tso_seg: The TSO segment element to be freed
 *
 * Returns a TSO segment element to the free list held in the
 * HTT pdev
 *
 * Return: none
 */

void ol_tso_free_segment(struct ol_txrx_pdev_t *pdev,
	 struct cdf_tso_seg_elem_t *tso_seg)
{
	cdf_spin_lock_bh(&pdev->tso_seg_pool.tso_mutex);
	tso_seg->next = pdev->tso_seg_pool.freelist;
	pdev->tso_seg_pool.freelist = tso_seg;
	pdev->tso_seg_pool.num_free++;
	cdf_spin_unlock_bh(&pdev->tso_seg_pool.tso_mutex);
}
#endif
