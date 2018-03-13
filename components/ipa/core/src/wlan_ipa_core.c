/*
 * Copyright (c) 2013-2018 The Linux Foundation. All rights reserved.
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

/* Include Files */
#include "wlan_ipa_core.h"
#include "wlan_ipa_main.h"
#include <ol_txrx.h>
#include "cdp_txrx_ipa.h"
#include "wal_rx_desc.h"

static struct wlan_ipa_priv *gp_ipa;

static struct wlan_ipa_iface_2_client {
	qdf_ipa_client_type_t cons_client;
	qdf_ipa_client_type_t prod_client;
} wlan_ipa_iface_2_client[WLAN_IPA_MAX_IFACE] = {
	{
		QDF_IPA_CLIENT_WLAN2_CONS, QDF_IPA_CLIENT_WLAN1_PROD
	}, {
		QDF_IPA_CLIENT_WLAN3_CONS, QDF_IPA_CLIENT_WLAN1_PROD
	}, {
		QDF_IPA_CLIENT_WLAN4_CONS, QDF_IPA_CLIENT_WLAN1_PROD
	}
};

/* Local Function Prototypes */
static void wlan_ipa_i2w_cb(void *priv, qdf_ipa_dp_evt_type_t evt,
			    unsigned long data);
static void wlan_ipa_w2i_cb(void *priv, qdf_ipa_dp_evt_type_t evt,
			    unsigned long data);

/**
 * wlan_ipa_uc_sta_is_enabled() - Is STA mode IPA uC offload enabled?
 * @ipa_cfg: IPA config
 *
 * Return: true if STA mode IPA uC offload is enabled, false otherwise
 */
static inline bool wlan_ipa_uc_sta_is_enabled(struct wlan_ipa_config *ipa_cfg)
{
	return WLAN_IPA_IS_CONFIG_ENABLED(ipa_cfg, WLAN_IPA_UC_STA_ENABLE_MASK);
}

/**
 * wlan_ipa_is_pre_filter_enabled() - Is IPA pre-filter enabled?
 * @ipa_cfg: IPA config
 *
 * Return: true if pre-filter is enabled, otherwise false
 */
static inline
bool wlan_ipa_is_pre_filter_enabled(struct wlan_ipa_config *ipa_cfg)
{
	return WLAN_IPA_IS_CONFIG_ENABLED(ipa_cfg,
					 WLAN_IPA_PRE_FILTER_ENABLE_MASK);
}

/**
 * wlan_ipa_is_ipv6_enabled() - Is IPA IPv6 enabled?
 * @ipa_cfg: IPA config
 *
 * Return: true if IPv6 is enabled, otherwise false
 */
static inline bool wlan_ipa_is_ipv6_enabled(struct wlan_ipa_config *ipa_cfg)
{
	return WLAN_IPA_IS_CONFIG_ENABLED(ipa_cfg, WLAN_IPA_IPV6_ENABLE_MASK);
}

/**
 * wlan_ipa_msg_free_fn() - Free an IPA message
 * @buff: pointer to the IPA message
 * @len: length of the IPA message
 * @type: type of IPA message
 *
 * Return: None
 */
static void wlan_ipa_msg_free_fn(void *buff, uint32_t len, uint32_t type)
{
	ipa_debug("msg type:%d, len:%d", type, len);
	qdf_mem_free(buff);
}

/**
 * wlan_ipa_uc_loaded_uc_cb() - IPA UC loaded event callback
 * @priv_ctxt: IPA context
 *
 * Will be called by IPA context.
 * It's atomic context, then should be scheduled to kworker thread
 *
 * Return: None
 */
static void wlan_ipa_uc_loaded_uc_cb(void *priv_ctxt)
{
	struct wlan_ipa_priv *ipa_ctx;
	struct op_msg_type *msg;
	struct uc_op_work_struct *uc_op_work;

	if (!priv_ctxt) {
		ipa_err("Invalid IPA context");
		return;
	}

	ipa_ctx = priv_ctxt;

	msg = qdf_mem_malloc(sizeof(*msg));
	if (!msg) {
		ipa_err("op_msg allocation fails");
		return;
	}

	msg->op_code = WLAN_IPA_UC_OPCODE_UC_READY;

	uc_op_work = &ipa_ctx->uc_op_work[msg->op_code];

	/* When the same uC OPCODE is already pended, just return */
	if (uc_op_work->msg)
		goto done;

	uc_op_work->msg = msg;
	qdf_sched_work(0, &uc_op_work->work);
	/* work handler will free the msg buffer */
	return;

done:
	qdf_mem_free(msg);
}

/**
 * wlan_ipa_uc_send_wdi_control_msg() - Set WDI control message
 * @ctrl: WDI control value
 *
 * Send WLAN_WDI_ENABLE for ctrl = true and WLAN_WDI_DISABLE otherwise.
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS wlan_ipa_uc_send_wdi_control_msg(bool ctrl)
{
	qdf_ipa_msg_meta_t meta;
	qdf_ipa_wlan_msg_t *ipa_msg;
	int ret = 0;

	/* WDI enable message to IPA */
	QDF_IPA_MSG_META_MSG_LEN(&meta) = sizeof(*ipa_msg);
	ipa_msg = qdf_mem_malloc(QDF_IPA_MSG_META_MSG_LEN(&meta));
	if (!ipa_msg) {
		ipa_err("msg allocation failed");
		return QDF_STATUS_E_NOMEM;
	}

	if (ctrl)
		QDF_IPA_SET_META_MSG_TYPE(&meta, QDF_WDI_ENABLE);
	else
		QDF_IPA_SET_META_MSG_TYPE(&meta, QDF_WDI_DISABLE);

	ipa_debug("ipa_send_msg(Evt:%d)", QDF_IPA_MSG_META_MSG_TYPE(&meta));
	ret = qdf_ipa_send_msg(&meta, ipa_msg, wlan_ipa_msg_free_fn);
	if (ret) {
		ipa_err("ipa_send_msg(Evt:%d)-fail=%d",
			QDF_IPA_MSG_META_MSG_TYPE(&meta), ret);
		qdf_mem_free(ipa_msg);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

struct wlan_ipa_priv *wlan_ipa_get_obj_context(void)
{
	return gp_ipa;
}

#ifdef CONFIG_IPA_WDI_UNIFIED_API

/*
 * TODO: Get WDI version through FW capabilities
 */
#ifdef CONFIG_LITHIUM
static inline void wlan_ipa_wdi_get_wdi_version(struct wlan_ipa_priv *ipa_ctx)
{
	ipa_ctx->wdi_version = IPA_WDI_3;
}
#elif defined(QCA_WIFI_3_0)
static inline void wlan_ipa_wdi_get_wdi_version(struct wlan_ipa_priv *ipa_ctx)
{
	ipa_ctx->wdi_version = IPA_WDI_2;
}
#else
static inline void wlan_ipa_wdi_get_wdi_version(struct wlan_ipa_priv *ipa_ctx)
{
	ipa_ctx->wdi_version = IPA_WDI_1;
}
#endif

static inline bool wlan_ipa_wdi_is_smmu_enabled(struct wlan_ipa_priv *ipa_ctx,
					       qdf_device_t osdev)
{
	/* TODO: Need to check if SMMU is supported on cld_3.2 */
	/* return hdd_ipa->is_smmu_enabled && qdf_mem_smmu_s1_enabled(osdev); */
	return 0;
}

static inline QDF_STATUS wlan_ipa_wdi_setup(struct wlan_ipa_priv *ipa_ctx,
					    qdf_device_t osdev)
{
	qdf_ipa_sys_connect_params_t sys_in[WLAN_IPA_MAX_IFACE];
	int i;

	for (i = 0; i < WLAN_IPA_MAX_IFACE; i++)
		qdf_mem_copy(&sys_in[i],
			     &ipa_ctx->sys_pipe[i].ipa_sys_params,
			     sizeof(qdf_ipa_sys_connect_params_t));

	return cdp_ipa_setup(ipa_ctx->dp_soc, ipa_ctx->dp_pdev,
			     wlan_ipa_i2w_cb, wlan_ipa_w2i_cb,
			     wlan_ipa_wdi_meter_notifier_cb,
			     ipa_ctx->config->desc_size,
			     ipa_ctx, wlan_ipa_is_rm_enabled(ipa_ctx->config),
			     &ipa_ctx->tx_pipe_handle,
			     &ipa_ctx->rx_pipe_handle,
			     wlan_ipa_wdi_is_smmu_enabled(ipa_ctx, osdev),
			     sys_in);
}

#ifdef FEATURE_METERING
/**
 * wlan_ipa_wdi_init_metering() - IPA WDI metering init
 * @ipa_ctx: IPA context
 * @in: IPA WDI in param
 *
 * Return: QDF_STATUS
 */
static inline void wlan_ipa_wdi_init_metering(struct wlan_ipa_priv *ipa_ctxt,
					      qdf_ipa_wdi_init_in_params_t *in)
{
	QDF_IPA_WDI_INIT_IN_PARAMS_WDI_NOTIFY(in) =
				wlan_ipa_wdi_meter_notifier_cb;
}
#else
static inline void wlan_ipa_wdi_init_metering(struct wlan_ipa_priv *ipa_ctxt,
					      qdf_ipa_wdi_init_in_params_t *in)
{
}
#endif

/**
 * wlan_ipa_wdi_init() - IPA WDI init
 * @ipa_ctx: IPA context
 *
 * Return: QDF_STATUS
 */
static inline QDF_STATUS wlan_ipa_wdi_init(struct wlan_ipa_priv *ipa_ctx)
{
	qdf_ipa_wdi_init_in_params_t in;
	qdf_ipa_wdi_init_out_params_t out;
	int ret;

	ipa_ctx->uc_loaded = false;

	QDF_IPA_WDI_INIT_IN_PARAMS_WDI_VERSION(&in) = ipa_ctx->wdi_version;
	QDF_IPA_WDI_INIT_IN_PARAMS_NOTIFY(&in) = wlan_ipa_uc_loaded_uc_cb;
	QDF_IPA_WDI_INIT_IN_PARAMS_PRIV(&in) = ipa_ctx;
	wlan_ipa_wdi_init_metering(ipa_ctx, &in);

	ret = qdf_ipa_wdi_init(&in, &out);
	if (ret) {
		ipa_err("ipa_wdi_init failed with ret=%d", ret);
		return QDF_STATUS_E_FAILURE;
	}

	if (QDF_IPA_WDI_INIT_OUT_PARAMS_IS_UC_READY(&out)) {
		ipa_info("IPA uC READY");
		ipa_ctx->uc_loaded = true;
		ipa_ctx->is_smmu_enabled =
			QDF_IPA_WDI_INIT_OUT_PARAMS_IS_SMMU_ENABLED(&out);
		ipa_info("is_smmu_enabled=%d", ipa_ctx->is_smmu_enabled);
	} else {
		return QDF_STATUS_E_BUSY;
	}

	return QDF_STATUS_SUCCESS;
}

static inline int wlan_ipa_wdi_cleanup(void)
{
	int ret;

	ret = qdf_ipa_wdi_cleanup();
	if (ret)
		ipa_info("ipa_wdi_cleanup failed ret=%d", ret);
	return ret;
}

static inline int wlan_ipa_wdi_setup_sys_pipe(struct wlan_ipa_priv *ipa_ctx,
					     struct ipa_sys_connect_params *sys,
					     uint32_t *handle)
{
	return 0;
}

static inline int wlan_ipa_wdi_teardown_sys_pipe(struct wlan_ipa_priv *ipa_ctx,
						uint32_t handle)
{
	return 0;
}

#else /* CONFIG_IPA_WDI_UNIFIED_API */

static inline void wlan_ipa_wdi_get_wdi_version(struct wlan_ipa_priv *ipa_ctx)
{
}

static inline QDF_STATUS wlan_ipa_wdi_setup(struct wlan_ipa_priv *ipa_ctx,
					    qdf_device_t osdev)
{
	return cdp_ipa_setup(ipa_ctx->dp_soc, ipa_ctx->dp_pdev,
			     wlan_ipa_i2w_cb, wlan_ipa_w2i_cb,
			     wlan_ipa_wdi_meter_notifier_cb,
			     ipa_ctx->config->desc_size,
			     ipa_ctx, wlan_ipa_is_rm_enabled(ipa_ctx->config),
			     &ipa_ctx->tx_pipe_handle,
			     &ipa_ctx->rx_pipe_handle);
}

static inline QDF_STATUS wlan_ipa_wdi_init(struct wlan_ipa_priv *ipa_ctx)
{
	struct ipa_wdi_uc_ready_params uc_ready_param;

	ipa_ctx->uc_loaded = false;
	uc_ready_param.priv = (void *)ipa_ctx;
	uc_ready_param.notify = wlan_ipa_uc_loaded_uc_cb;
	if (qdf_ipa_uc_reg_rdyCB(&uc_ready_param)) {
		ipa_info("UC Ready CB register fail");
		return QDF_STATUS_E_FAILURE;
	}

	if (true == uc_ready_param.is_uC_ready) {
		ipa_info("UC Ready");
		ipa_ctx->uc_loaded = true;
	} else {
		return QDF_STATUS_E_BUSY;
	}

	return QDF_STATUS_SUCCESS;
}

static inline int wlan_ipa_wdi_cleanup(void)
{
	int ret;

	ret = qdf_ipa_uc_dereg_rdyCB();
	if (ret)
		ipa_info("UC Ready CB deregister fail");
	return ret;
}

static inline int wlan_ipa_wdi_setup_sys_pipe(
		struct wlan_ipa_priv *ipa_ctx,
		struct ipa_sys_connect_params *sys, uint32_t *handle)
{
	return qdf_ipa_setup_sys_pipe(sys, handle);
}

static inline int wlan_ipa_wdi_teardown_sys_pipe(
		struct wlan_ipa_priv *ipa_ctx,
		uint32_t handle)
{
	return qdf_ipa_teardown_sys_pipe(handle);
}

#endif /* CONFIG_IPA_WDI_UNIFIED_API */

/**
 * wlan_ipa_send_skb_to_network() - Send skb to kernel
 * @skb: network buffer
 * @iface_ctx: IPA interface context
 *
 * Called when a network buffer is received which should not be routed
 * to the IPA module.
 *
 * Return: None
 */
static void
wlan_ipa_send_skb_to_network(qdf_nbuf_t skb,
			     struct wlan_ipa_iface_context *iface_ctx)
{
	struct wlan_ipa_priv *ipa_ctx = gp_ipa;

	if (!iface_ctx->dev) {
		WLAN_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Invalid interface");
		ipa_ctx->ipa_rx_internal_drop_count++;
		qdf_nbuf_free(skb);
		return;
	}

	skb->destructor = wlan_ipa_uc_rt_debug_destructor;

	if (ipa_ctx->send_to_nw)
		ipa_ctx->send_to_nw(skb, iface_ctx->dev);

	ipa_ctx->ipa_rx_net_send_count++;
}

/**
 * wlan_ipa_forward() - handle packet forwarding to wlan tx
 * @ipa_ctx: pointer to ipa ipa context
 * @iface_ctx: interface context
 * @skb: data pointer
 *
 * if exception packet has set forward bit, copied new packet should be
 * forwarded to wlan tx. if wlan subsystem is in suspend state, packet should
 * put into pm queue and tx procedure will be differed
 *
 * Return: None
 */
static void wlan_ipa_forward(struct wlan_ipa_priv *ipa_ctx,
			     struct wlan_ipa_iface_context *iface_ctx,
			     qdf_nbuf_t skb)
{
	struct wlan_ipa_pm_tx_cb *pm_tx_cb;

	qdf_spin_lock_bh(&ipa_ctx->pm_lock);

	/* Set IPA ownership for intra-BSS Tx packets to avoid skb_orphan */
	qdf_nbuf_ipa_owned_set(skb);

	/* WLAN subsystem is in suspend, put in queue */
	if (ipa_ctx->suspended) {
		qdf_spin_unlock_bh(&ipa_ctx->pm_lock);
		WLAN_IPA_LOG(QDF_TRACE_LEVEL_INFO,
			"Tx in suspend, put in queue");
		qdf_mem_set(skb->cb, sizeof(skb->cb), 0);
		pm_tx_cb = (struct wlan_ipa_pm_tx_cb *)skb->cb;
		pm_tx_cb->exception = true;
		pm_tx_cb->iface_context = iface_ctx;
		qdf_spin_lock_bh(&ipa_ctx->pm_lock);
		qdf_nbuf_queue_add(&ipa_ctx->pm_queue_head, skb);
		qdf_spin_unlock_bh(&ipa_ctx->pm_lock);
		ipa_ctx->stats.num_tx_queued++;
	} else {
		/* Resume, put packet into WLAN TX */
		qdf_spin_unlock_bh(&ipa_ctx->pm_lock);

		if (ipa_ctx->softap_xmit) {
			if (ipa_ctx->softap_xmit(skb, iface_ctx->dev)) {
				WLAN_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					     "packet Tx fail");
				ipa_ctx->stats.num_tx_fwd_err++;
			} else {
				ipa_ctx->stats.num_tx_fwd_ok++;
			}
		} else {
			pm_tx_cb = (struct wlan_ipa_pm_tx_cb *)skb->cb;
			ipa_free_skb(pm_tx_cb->ipa_tx_desc);
		}
	}
}

/**
 * wlan_ipa_intrabss_forward() - Forward intra bss packets.
 * @ipa_ctx: pointer to IPA IPA struct
 * @iface_ctx: ipa interface context
 * @desc: Firmware descriptor
 * @skb: Data buffer
 *
 * Return:
 *      WLAN_IPA_FORWARD_PKT_NONE
 *      WLAN_IPA_FORWARD_PKT_DISCARD
 *      WLAN_IPA_FORWARD_PKT_LOCAL_STACK
 *
 */

static enum wlan_ipa_forward_type wlan_ipa_intrabss_forward(
		struct wlan_ipa_priv *ipa_ctx,
		struct wlan_ipa_iface_context *iface_ctx,
		uint8_t desc,
		qdf_nbuf_t skb)
{
	int ret = WLAN_IPA_FORWARD_PKT_NONE;
	void *soc = cds_get_context(QDF_MODULE_ID_SOC);
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);

	if ((desc & FW_RX_DESC_FORWARD_M)) {
		if (!ol_txrx_fwd_desc_thresh_check(
			(struct ol_txrx_vdev_t *)cdp_get_vdev_from_vdev_id(soc,
						(struct cdp_pdev *)pdev,
						iface_ctx->session_id))) {
			/* Drop the packet*/
			ipa_ctx->stats.num_tx_fwd_err++;
			kfree_skb(skb);
			ret = WLAN_IPA_FORWARD_PKT_DISCARD;
			return ret;
		}
		WLAN_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
				"Forward packet to Tx (fw_desc=%d)", desc);
		ipa_ctx->ipa_tx_forward++;

		if ((desc & FW_RX_DESC_DISCARD_M)) {
			wlan_ipa_forward(ipa_ctx, iface_ctx, skb);
			ipa_ctx->ipa_rx_internal_drop_count++;
			ipa_ctx->ipa_rx_discard++;
			ret = WLAN_IPA_FORWARD_PKT_DISCARD;
		} else {
			struct sk_buff *cloned_skb = skb_clone(skb, GFP_ATOMIC);

			if (cloned_skb)
				wlan_ipa_forward(ipa_ctx, iface_ctx,
						 cloned_skb);
			else
				WLAN_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
						"tx skb alloc failed");
			ret = WLAN_IPA_FORWARD_PKT_LOCAL_STACK;
		}
	}

	return ret;
}

/**
 * wlan_ipa_w2i_cb() - WLAN to IPA callback handler
 * @priv: pointer to private data registered with IPA (we register a
 *	pointer to the global IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void wlan_ipa_w2i_cb(void *priv, qdf_ipa_dp_evt_type_t evt,
			    unsigned long data)
{
	struct wlan_ipa_priv *ipa_ctx = NULL;
	qdf_nbuf_t skb;
	uint8_t iface_id;
	uint8_t session_id;
	struct wlan_ipa_iface_context *iface_context;
	uint8_t fw_desc;

	ipa_ctx = (struct wlan_ipa_priv *)priv;

	if (!ipa_ctx)
		return;

	switch (evt) {
	case IPA_RECEIVE:
		skb = (qdf_nbuf_t) data;

		if (wlan_ipa_uc_is_enabled(ipa_ctx->config)) {
			session_id = (uint8_t)skb->cb[0];
			iface_id = ipa_ctx->vdev_to_iface[session_id];
			ipa_debug("IPA_RECEIVE: session_id=%u, iface_id=%u",
				  session_id, iface_id);
		} else {
			iface_id = WLAN_IPA_GET_IFACE_ID(skb->data);
		}
		if (iface_id >= WLAN_IPA_MAX_IFACE) {
			ipa_err("IPA_RECEIVE: Invalid iface_id: %u",
				iface_id);
			ipa_ctx->ipa_rx_internal_drop_count++;
			qdf_nbuf_free(skb);
			return;
		}

		iface_context = &ipa_ctx->iface_context[iface_id];
		if (!iface_context->tl_context) {
			ipa_err("IPA_RECEIVE: TL context is NULL");
			ipa_ctx->ipa_rx_internal_drop_count++;
			qdf_nbuf_free(skb);
			return;
		}

		if (wlan_ipa_uc_is_enabled(ipa_ctx->config)) {
			ipa_ctx->stats.num_rx_excep++;
			qdf_nbuf_pull_head(skb, WLAN_IPA_UC_WLAN_CLD_HDR_LEN);
		} else {
			qdf_nbuf_pull_head(skb, WLAN_IPA_WLAN_CLD_HDR_LEN);
		}

		iface_context->stats.num_rx_ipa_excep++;

		/* Disable to forward Intra-BSS Rx packets when
		 * ap_isolate=1 in hostapd.conf
		 */
		if (!ipa_ctx->ap_intrabss_fwd) {
			/*
			 * When INTRA_BSS_FWD_OFFLOAD is enabled, FW will send
			 * all Rx packets to IPA uC, which need to be forwarded
			 * to other interface.
			 * And, IPA driver will send back to WLAN host driver
			 * through exception pipe with fw_desc field set by FW.
			 * Here we are checking fw_desc field for FORWARD bit
			 * set, and forward to Tx. Then copy to kernel stack
			 * only when DISCARD bit is not set.
			 */
			fw_desc = (uint8_t)skb->cb[1];
			if (WLAN_IPA_FORWARD_PKT_DISCARD ==
			    wlan_ipa_intrabss_forward(ipa_ctx, iface_context,
						     fw_desc, skb))
				break;
		} else {
			ipa_debug("Intra-BSS FWD is disabled-skip forward to Tx");
		}

		wlan_ipa_send_skb_to_network(skb, iface_context);
		break;

	default:
		ipa_err("w2i cb wrong event: 0x%x", evt);
		return;
	}
}

/**
 * wlan_ipa_send_pkt_to_tl() - Send an IPA packet to TL
 * @iface_context: interface-specific IPA context
 * @ipa_tx_desc: packet data descriptor
 *
 * Return: None
 */
static void wlan_ipa_send_pkt_to_tl(
		struct wlan_ipa_iface_context *iface_context,
		qdf_ipa_rx_data_t *ipa_tx_desc)
{
	struct wlan_ipa_priv *ipa_ctx = iface_context->ipa_ctx;
	qdf_nbuf_t skb;
	struct wlan_ipa_tx_desc *tx_desc;

	qdf_spin_lock_bh(&iface_context->interface_lock);
	/*
	 * During CAC period, data packets shouldn't be sent over the air so
	 * drop all the packets here
	 */
	if (iface_context->device_mode == QDF_SAP_MODE ||
	    iface_context->device_mode == QDF_P2P_GO_MODE) {
		if (ipa_ctx->dfs_cac_block_tx) {
			ipa_free_skb(ipa_tx_desc);
			qdf_spin_unlock_bh(&iface_context->interface_lock);
			iface_context->stats.num_tx_cac_drop++;
			wlan_ipa_wdi_rm_try_release(ipa_ctx);
			return;
		}
	}
	qdf_spin_unlock_bh(&iface_context->interface_lock);

	skb = QDF_IPA_RX_DATA_SKB(ipa_tx_desc);

	qdf_mem_set(skb->cb, sizeof(skb->cb), 0);

	/* Store IPA Tx buffer ownership into SKB CB */
	qdf_nbuf_ipa_owned_set(skb);
	if (wlan_ipa_uc_sta_is_enabled(ipa_ctx->config)) {
		qdf_nbuf_mapped_paddr_set(skb,
					  QDF_IPA_RX_DATA_DMA_ADDR(ipa_tx_desc)
					  + WLAN_IPA_WLAN_FRAG_HEADER
					  + WLAN_IPA_WLAN_IPA_HEADER);
		QDF_IPA_RX_DATA_SKB_LEN(ipa_tx_desc) -=
			WLAN_IPA_WLAN_FRAG_HEADER + WLAN_IPA_WLAN_IPA_HEADER;
	} else
		qdf_nbuf_mapped_paddr_set(skb, ipa_tx_desc->dma_addr);

	qdf_spin_lock_bh(&ipa_ctx->q_lock);
	/* get free Tx desc and assign ipa_tx_desc pointer */
	if (qdf_list_remove_front(&ipa_ctx->tx_desc_list,
				 (qdf_list_node_t **)&tx_desc) ==
	    QDF_STATUS_SUCCESS) {
		tx_desc->ipa_tx_desc_ptr = ipa_tx_desc;
		ipa_ctx->stats.num_tx_desc_q_cnt++;
		qdf_spin_unlock_bh(&ipa_ctx->q_lock);
		/* Store Tx Desc index into SKB CB */
		QDF_NBUF_CB_TX_IPA_PRIV(skb) = tx_desc->id;
	} else {
		ipa_ctx->stats.num_tx_desc_error++;
		qdf_spin_unlock_bh(&ipa_ctx->q_lock);
		qdf_ipa_free_skb(ipa_tx_desc);
		wlan_ipa_wdi_rm_try_release(ipa_ctx);
		return;
	}

	skb = cdp_ipa_tx_send_data_frame(cds_get_context(QDF_MODULE_ID_SOC),
			(struct cdp_vdev *)iface_context->tl_context,
			QDF_IPA_RX_DATA_SKB(ipa_tx_desc));
	if (skb) {
		qdf_nbuf_free(skb);
		iface_context->stats.num_tx_err++;
		return;
	}

	atomic_inc(&ipa_ctx->tx_ref_cnt);

	iface_context->stats.num_tx++;
}

/**
 * wlan_ipa_i2w_cb() - IPA to WLAN callback
 * @priv: pointer to private data registered with IPA (we register a
 *	pointer to the interface-specific IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void wlan_ipa_i2w_cb(void *priv, qdf_ipa_dp_evt_type_t evt,
			     unsigned long data)
{
	struct wlan_ipa_priv *ipa_ctx = NULL;
	qdf_ipa_rx_data_t *ipa_tx_desc;
	struct wlan_ipa_iface_context *iface_context;
	qdf_nbuf_t skb;
	struct wlan_ipa_pm_tx_cb *pm_tx_cb = NULL;

	iface_context = (struct wlan_ipa_iface_context *)priv;
	ipa_tx_desc = (qdf_ipa_rx_data_t *)data;
	ipa_ctx = iface_context->ipa_ctx;

	if (evt != IPA_RECEIVE) {
		WLAN_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Event is not IPA_RECEIVE");
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_drop++;
		return;
	}

	skb = QDF_IPA_RX_DATA_SKB(ipa_tx_desc);

	/*
	 * If PROD resource is not requested here then there may be cases where
	 * IPA hardware may be clocked down because of not having proper
	 * dependency graph between WLAN CONS and modem PROD pipes. Adding the
	 * workaround to request PROD resource while data is going over CONS
	 * pipe to prevent the IPA hardware clockdown.
	 */
	wlan_ipa_wdi_rm_request(ipa_ctx);

	qdf_spin_lock_bh(&ipa_ctx->pm_lock);
	/*
	 * If host is still suspended then queue the packets and these will be
	 * drained later when resume completes. When packet is arrived here and
	 * host is suspended, this means that there is already resume is in
	 * progress.
	 */
	if (ipa_ctx->suspended) {
		qdf_mem_set(skb->cb, sizeof(skb->cb), 0);
		pm_tx_cb = (struct wlan_ipa_pm_tx_cb *)skb->cb;
		pm_tx_cb->iface_context = iface_context;
		pm_tx_cb->ipa_tx_desc = ipa_tx_desc;
		qdf_nbuf_queue_add(&ipa_ctx->pm_queue_head, skb);
		ipa_ctx->stats.num_tx_queued++;

		qdf_spin_unlock_bh(&ipa_ctx->pm_lock);
		return;
	}

	qdf_spin_unlock_bh(&ipa_ctx->pm_lock);

	/*
	 * If we are here means, host is not suspended, wait for the work queue
	 * to finish.
	 */
	qdf_flush_work(&ipa_ctx->pm_work);

	return wlan_ipa_send_pkt_to_tl(iface_context, ipa_tx_desc);
}

QDF_STATUS wlan_ipa_suspend(struct wlan_ipa_priv *ipa_ctx)
{
	/*
	 * Check if IPA is ready for suspend, If we are here means, there is
	 * high chance that suspend would go through but just to avoid any race
	 * condition after suspend started, these checks are conducted before
	 * allowing to suspend.
	 */
	if (atomic_read(&ipa_ctx->tx_ref_cnt))
		return QDF_STATUS_E_AGAIN;

	qdf_spin_lock_bh(&ipa_ctx->rm_lock);

	if (ipa_ctx->rm_state != WLAN_IPA_RM_RELEASED) {
		qdf_spin_unlock_bh(&ipa_ctx->rm_lock);
		return QDF_STATUS_E_AGAIN;
	}
	qdf_spin_unlock_bh(&ipa_ctx->rm_lock);

	qdf_spin_lock_bh(&ipa_ctx->pm_lock);
	ipa_ctx->suspended = true;
	qdf_spin_unlock_bh(&ipa_ctx->pm_lock);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wlan_ipa_resume(struct wlan_ipa_priv *ipa_ctx)
{
	qdf_sched_work(0, &ipa_ctx->pm_work);

	qdf_spin_lock_bh(&ipa_ctx->pm_lock);
	ipa_ctx->suspended = false;
	qdf_spin_unlock_bh(&ipa_ctx->pm_lock);

	return QDF_STATUS_SUCCESS;
}

/**
 * wlan_ipa_pm_flush() - flush queued packets
 * @work: pointer to the scheduled work
 *
 * Called during PM resume to send packets to TL which were queued
 * while host was in the process of suspending.
 *
 * Return: None
 */
static void wlan_ipa_pm_flush(void *data)
{
	struct wlan_ipa_priv *ipa_ctx = (struct wlan_ipa_priv *)data;
	struct wlan_ipa_pm_tx_cb *pm_tx_cb = NULL;
	qdf_nbuf_t skb;
	uint32_t dequeued = 0;

	qdf_wake_lock_acquire(&ipa_ctx->wake_lock,
			      WIFI_POWER_EVENT_WAKELOCK_IPA);
	qdf_spin_lock_bh(&ipa_ctx->pm_lock);
	while (((skb = qdf_nbuf_queue_remove(&ipa_ctx->pm_queue_head)) !=
	       NULL)) {
		qdf_spin_unlock_bh(&ipa_ctx->pm_lock);

		pm_tx_cb = (struct wlan_ipa_pm_tx_cb *)skb->cb;
		dequeued++;

		wlan_ipa_send_pkt_to_tl(pm_tx_cb->iface_context,
					pm_tx_cb->ipa_tx_desc);

		qdf_spin_lock_bh(&ipa_ctx->pm_lock);
	}
	qdf_spin_unlock_bh(&ipa_ctx->pm_lock);
	qdf_wake_lock_release(&ipa_ctx->wake_lock,
			      WIFI_POWER_EVENT_WAKELOCK_IPA);

	ipa_ctx->stats.num_tx_dequeued += dequeued;
	if (dequeued > ipa_ctx->stats.num_max_pm_queue)
		ipa_ctx->stats.num_max_pm_queue = dequeued;
}

QDF_STATUS wlan_ipa_uc_enable_pipes(struct wlan_ipa_priv *ipa_ctx)
{
	int result;

	ipa_debug("enter");

	if (!ipa_ctx->ipa_pipes_down) {
		/*
		 * IPA WDI Pipes are already activated due to
		 * rm deferred resources grant
		 */
		ipa_warn("IPA WDI Pipes are already activated");
		goto end;
	}

	result = cdp_ipa_enable_pipes(ipa_ctx->dp_soc,
				      ipa_ctx->dp_pdev);
	if (result) {
		ipa_err("Enable IPA WDI PIPE failed: ret=%d", result);
		return QDF_STATUS_E_FAILURE;
	}

	qdf_event_reset(&ipa_ctx->ipa_resource_comp);
	ipa_ctx->ipa_pipes_down = false;

	cdp_ipa_enable_autonomy(ipa_ctx->dp_soc,
				ipa_ctx->dp_pdev);

end:
	ipa_debug("exit: ipa_pipes_down=%d", ipa_ctx->ipa_pipes_down);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS wlan_ipa_uc_disable_pipes(struct wlan_ipa_priv *ipa_ctx)
{
	int result;

	ipa_debug("enter");

	if (ipa_ctx->ipa_pipes_down) {
		/*
		 * This shouldn't happen :
		 * IPA WDI Pipes are already deactivated
		 */
		QDF_ASSERT(0);
		ipa_warn("IPA WDI Pipes are already deactivated");
		goto end;
	}

	cdp_ipa_disable_autonomy(ipa_ctx->dp_soc,
				 ipa_ctx->dp_pdev);

	result = cdp_ipa_disable_pipes(ipa_ctx->dp_soc,
				       ipa_ctx->dp_pdev);
	if (result) {
		ipa_err("Disable IPA WDI PIPE failed: ret=%d", result);
		return QDF_STATUS_E_FAILURE;
	}

	ipa_ctx->ipa_pipes_down = true;

end:
	ipa_debug("exit: ipa_pipes_down=%d", ipa_ctx->ipa_pipes_down);

	return QDF_STATUS_SUCCESS;
}

/**
 * wlan_ipa_alloc_tx_desc_list() - Allocate IPA Tx desc list
 * @ipa_ctx: IPA context
 *
 * Return: QDF_STATUS
 */
static int wlan_ipa_alloc_tx_desc_list(struct wlan_ipa_priv *ipa_ctx)
{
	int i;
	uint32_t max_desc_cnt;
	struct wlan_ipa_tx_desc *tmp_desc;

	max_desc_cnt = ipa_ctx->config->txbuf_count;

	qdf_list_create(&ipa_ctx->tx_desc_list, max_desc_cnt);

	qdf_spin_lock_bh(&ipa_ctx->q_lock);
	for (i = 0; i < max_desc_cnt; i++) {
		tmp_desc = qdf_mem_malloc(sizeof(*tmp_desc));
		tmp_desc->id = i;
		tmp_desc->ipa_tx_desc_ptr = NULL;
		qdf_list_insert_back(&ipa_ctx->tx_desc_list,
				     &tmp_desc->node);
		tmp_desc++;
	}

	ipa_ctx->stats.num_tx_desc_q_cnt = 0;
	ipa_ctx->stats.num_tx_desc_error = 0;

	qdf_spin_unlock_bh(&ipa_ctx->q_lock);

	return QDF_STATUS_SUCCESS;
}

#ifndef QCA_LL_TX_FLOW_CONTROL_V2
/**
 * wlan_ipa_setup_tx_sys_pipe() - Setup IPA Tx system pipes
 * @ipa_ctx: Global IPA IPA context
 * @desc_fifo_sz: Number of descriptors
 *
 * Return: 0 on success, negative errno on error
 */
static int wlan_ipa_setup_tx_sys_pipe(struct wlan_ipa_priv *ipa_ctx,
				     int32_t desc_fifo_sz)
{
	int i, ret = 0;
	qdf_ipa_sys_connect_params_t *ipa;

	/*setup TX pipes */
	for (i = 0; i < WLAN_IPA_MAX_IFACE; i++) {
		ipa = &ipa_ctx->sys_pipe[i].ipa_sys_params;

		ipa->client = wlan_ipa_iface_2_client[i].cons_client;
		ipa->desc_fifo_sz = desc_fifo_sz;
		ipa->priv = &ipa_ctx->iface_context[i];
		ipa->notify = wlan_ipa_i2w_cb;

		if (wlan_ipa_uc_sta_is_enabled(ipa_ctx->config)) {
			ipa->ipa_ep_cfg.hdr.hdr_len =
				WLAN_IPA_UC_WLAN_TX_HDR_LEN;
			ipa->ipa_ep_cfg.nat.nat_en = IPA_BYPASS_NAT;
			ipa->ipa_ep_cfg.hdr.hdr_ofst_pkt_size_valid = 1;
			ipa->ipa_ep_cfg.hdr.hdr_ofst_pkt_size = 0;
			ipa->ipa_ep_cfg.hdr.hdr_additional_const_len =
				WLAN_IPA_UC_WLAN_8023_HDR_SIZE;
			ipa->ipa_ep_cfg.hdr_ext.hdr_little_endian = true;
		} else {
			ipa->ipa_ep_cfg.hdr.hdr_len = WLAN_IPA_WLAN_TX_HDR_LEN;
		}
		ipa->ipa_ep_cfg.mode.mode = IPA_BASIC;

		ret = wlan_ipa_wdi_setup_sys_pipe(ipa_ctx, ipa,
				&ipa_ctx->sys_pipe[i].conn_hdl);
		if (ret) {
			ipa_err("Failed for pipe %d ret: %d", i, ret);
			return ret;
		}
		ipa_ctx->sys_pipe[i].conn_hdl_valid = 1;
	}

	return ret;
}
#else
/**
 * wlan_ipa_setup_tx_sys_pipe() - Setup IPA Tx system pipes
 * @ipa_ctx: Global IPA IPA context
 * @desc_fifo_sz: Number of descriptors
 *
 * Return: 0 on success, negative errno on error
 */
static int wlan_ipa_setup_tx_sys_pipe(struct wlan_ipa_priv *ipa_ctx,
				     int32_t desc_fifo_sz)
{
	/*
	 * The Tx system pipes are not needed for MCC when TX_FLOW_CONTROL_V2
	 * is enabled, where per vdev descriptors are supported in firmware.
	 */
	return 0;
}
#endif

/**
 * wlan_ipa_setup_rx_sys_pipe() - Setup IPA Rx system pipes
 * @ipa_ctx: Global IPA IPA context
 * @desc_fifo_sz: Number of descriptors
 *
 * Return: 0 on success, negative errno on error
 */
static int wlan_ipa_setup_rx_sys_pipe(struct wlan_ipa_priv *ipa_ctx,
				     int32_t desc_fifo_sz)
{
	int ret = 0;
	qdf_ipa_sys_connect_params_t *ipa;

	/*
	 * Hard code it here, this can be extended if in case
	 * PROD pipe is also per interface.
	 * Right now there is no advantage of doing this.
	 */
	ipa = &ipa_ctx->sys_pipe[WLAN_IPA_RX_PIPE].ipa_sys_params;

	ipa->client = IPA_CLIENT_WLAN1_PROD;

	ipa->desc_fifo_sz = desc_fifo_sz;
	ipa->priv = ipa_ctx;
	ipa->notify = wlan_ipa_w2i_cb;

	ipa->ipa_ep_cfg.nat.nat_en = IPA_BYPASS_NAT;
	ipa->ipa_ep_cfg.hdr.hdr_len = WLAN_IPA_WLAN_RX_HDR_LEN;
	ipa->ipa_ep_cfg.hdr.hdr_ofst_metadata_valid = 1;
	ipa->ipa_ep_cfg.mode.mode = IPA_BASIC;

	ret = qdf_ipa_setup_sys_pipe(ipa,
			&ipa_ctx->sys_pipe[WLAN_IPA_RX_PIPE].conn_hdl);
	if (ret) {
		ipa_err("Failed for RX pipe: %d", ret);
		return ret;
	}
	ipa_ctx->sys_pipe[WLAN_IPA_RX_PIPE].conn_hdl_valid = 1;

	return ret;
}

/**
 * wlan_ipa_setup_sys_pipe() - Setup all IPA system pipes
 * @ipa_ctx: Global IPA IPA context
 *
 * Return: 0 on success, negative errno on error
 */
static int wlan_ipa_setup_sys_pipe(struct wlan_ipa_priv *ipa_ctx)
{
	int i = WLAN_IPA_MAX_IFACE, ret = 0;
	uint32_t desc_fifo_sz;

	/* The maximum number of descriptors that can be provided to a BAM at
	 * once is one less than the total number of descriptors that the buffer
	 * can contain.
	 * If max_num_of_descriptors = (BAM_PIPE_DESCRIPTOR_FIFO_SIZE / sizeof
	 * (SPS_DESCRIPTOR)), then (max_num_of_descriptors - 1) descriptors can
	 * be provided at once.
	 * Because of above requirement, one extra descriptor will be added to
	 * make sure hardware always has one descriptor.
	 */
	desc_fifo_sz = ipa_ctx->config->desc_size
		       + SPS_DESC_SIZE;

	ret = wlan_ipa_setup_tx_sys_pipe(ipa_ctx, desc_fifo_sz);
	if (ret) {
		ipa_err("Failed for TX pipe: %d", ret);
		goto setup_sys_pipe_fail;
	}

	if (!wlan_ipa_uc_sta_is_enabled(ipa_ctx->config)) {
		ret = wlan_ipa_setup_rx_sys_pipe(ipa_ctx, desc_fifo_sz);
		if (ret) {
			ipa_err("Failed for RX pipe: %d", ret);
			goto setup_sys_pipe_fail;
		}
	}

       /* Allocate free Tx desc list */
	ret = wlan_ipa_alloc_tx_desc_list(ipa_ctx);
	if (ret)
		goto setup_sys_pipe_fail;

	return ret;

setup_sys_pipe_fail:

	for (i = 0; i < WLAN_IPA_MAX_SYSBAM_PIPE; i++) {
		if (ipa_ctx->sys_pipe[i].conn_hdl_valid)
			qdf_ipa_teardown_sys_pipe(
				ipa_ctx->sys_pipe[i].conn_hdl);
		qdf_mem_zero(&ipa_ctx->sys_pipe[i],
			     sizeof(struct wlan_ipa_sys_pipe));
	}

	return ret;
}

/**
 * wlan_ipa_teardown_sys_pipe() - Tear down all IPA Sys pipes
 * @ipa_ctx: Global IPA IPA context
 *
 * Return: None
 */
static void wlan_ipa_teardown_sys_pipe(struct wlan_ipa_priv *ipa_ctx)
{
	int ret = 0, i;
	struct wlan_ipa_tx_desc *tmp_desc;
	qdf_ipa_rx_data_t *ipa_tx_desc;
	qdf_list_node_t *node;

	for (i = 0; i < WLAN_IPA_MAX_SYSBAM_PIPE; i++) {
		if (ipa_ctx->sys_pipe[i].conn_hdl_valid) {
			ret = wlan_ipa_wdi_teardown_sys_pipe(ipa_ctx,
					ipa_ctx->sys_pipe[i].conn_hdl);
			if (ret)
				ipa_err("Failed:%d", ret);

			ipa_ctx->sys_pipe[i].conn_hdl_valid = 0;
		}
	}

	while (qdf_list_remove_front(&ipa_ctx->tx_desc_list, &node) ==
	       QDF_STATUS_SUCCESS) {
		tmp_desc = qdf_container_of(node, struct wlan_ipa_tx_desc,
					    node);
		ipa_tx_desc = tmp_desc->ipa_tx_desc_ptr;
		if (ipa_tx_desc)
			qdf_ipa_free_skb(ipa_tx_desc);

		qdf_mem_free(tmp_desc);
	}
}

/**
 * wlan_ipa_setup() - IPA initialization function
 * @ipa_ctx: IPA context
 * @ipa_cfg: IPA config
 *
 * Allocate ipa_ctx resources, ipa pipe resource and register
 * wlan interface with IPA module.
 *
 * Return: QDF_STATUS enumeration
 */
QDF_STATUS wlan_ipa_setup(struct wlan_ipa_priv *ipa_ctx,
			  struct wlan_ipa_config *ipa_cfg)
{
	int ret, i;
	struct wlan_ipa_iface_context *iface_context = NULL;
	QDF_STATUS status;

	ipa_debug("enter");

	gp_ipa = ipa_ctx;
	ipa_ctx->num_iface = 0;
	ipa_ctx->config = ipa_cfg;

	wlan_ipa_wdi_get_wdi_version(ipa_ctx);

	/* Create the interface context */
	for (i = 0; i < WLAN_IPA_MAX_IFACE; i++) {
		iface_context = &ipa_ctx->iface_context[i];
		iface_context->ipa_ctx = ipa_ctx;
		iface_context->cons_client =
			wlan_ipa_iface_2_client[i].cons_client;
		iface_context->prod_client =
			wlan_ipa_iface_2_client[i].prod_client;
		iface_context->iface_id = i;
		iface_context->dev = NULL;
		iface_context->device_mode = QDF_MAX_NO_OF_MODE;
		iface_context->tl_context = NULL;
		qdf_spinlock_create(&iface_context->interface_lock);
	}

	qdf_create_work(0, &ipa_ctx->pm_work, wlan_ipa_pm_flush, ipa_ctx);
	qdf_spinlock_create(&ipa_ctx->pm_lock);
	qdf_spinlock_create(&ipa_ctx->q_lock);
	qdf_nbuf_queue_init(&ipa_ctx->pm_queue_head);
	qdf_list_create(&ipa_ctx->pending_event, 1000);
	qdf_mutex_create(&ipa_ctx->event_lock);
	qdf_mutex_create(&ipa_ctx->ipa_lock);

	status = wlan_ipa_wdi_setup_rm(ipa_ctx);
	if (status != QDF_STATUS_SUCCESS)
		goto fail_setup_rm;

	for (i = 0; i < WLAN_IPA_MAX_SYSBAM_PIPE; i++)
		qdf_mem_zero(&ipa_ctx->sys_pipe[i],
			     sizeof(struct wlan_ipa_sys_pipe));

	if (wlan_ipa_uc_is_enabled(ipa_ctx->config)) {
		qdf_mem_zero(&ipa_ctx->stats, sizeof(ipa_ctx->stats));
		ipa_ctx->sap_num_connected_sta = 0;
		ipa_ctx->ipa_tx_packets_diff = 0;
		ipa_ctx->ipa_rx_packets_diff = 0;
		ipa_ctx->ipa_p_tx_packets = 0;
		ipa_ctx->ipa_p_rx_packets = 0;
		ipa_ctx->resource_loading = false;
		ipa_ctx->resource_unloading = false;
		ipa_ctx->sta_connected = 0;
		ipa_ctx->ipa_pipes_down = true;
		ipa_ctx->wdi_enabled = false;
		/* Setup IPA system pipes */
		if (wlan_ipa_uc_sta_is_enabled(ipa_ctx->config)) {
			ret = wlan_ipa_setup_sys_pipe(ipa_ctx);
			if (ret)
				goto fail_create_sys_pipe;
		}

		status = wlan_ipa_wdi_init(ipa_ctx);
		if (status == QDF_STATUS_E_BUSY)
			status = wlan_ipa_uc_send_wdi_control_msg(false);
		if (status != QDF_STATUS_SUCCESS) {
			ipa_err("IPA WDI init failed: ret=%d", ret);
			goto fail_create_sys_pipe;
		}
	} else {
		ret = wlan_ipa_setup_sys_pipe(ipa_ctx);
		if (ret)
			goto fail_create_sys_pipe;
	}

	qdf_event_create(&ipa_ctx->ipa_resource_comp);

	ipa_debug("exit: success");

	return QDF_STATUS_SUCCESS;

fail_create_sys_pipe:
	wlan_ipa_wdi_destroy_rm(ipa_ctx);

fail_setup_rm:
	qdf_spinlock_destroy(&ipa_ctx->pm_lock);
	qdf_spinlock_destroy(&ipa_ctx->q_lock);
	for (i = 0; i < WLAN_IPA_MAX_IFACE; i++) {
		iface_context = &ipa_ctx->iface_context[i];
		qdf_spinlock_destroy(&iface_context->interface_lock);
	}
	qdf_mutex_destroy(&ipa_ctx->event_lock);
	qdf_mutex_destroy(&ipa_ctx->ipa_lock);
	qdf_list_destroy(&ipa_ctx->pending_event);
	gp_ipa = NULL;
	ipa_debug("exit: fail");

	return QDF_STATUS_E_FAILURE;
}

void wlan_ipa_flush(struct wlan_ipa_priv *ipa_ctx)
{
	qdf_nbuf_t skb;
	struct wlan_ipa_pm_tx_cb *pm_tx_cb;

	if (!wlan_ipa_is_enabled(ipa_ctx->config))
		return;

	qdf_cancel_work(&ipa_ctx->pm_work);

	qdf_spin_lock_bh(&ipa_ctx->pm_lock);

	while (((skb = qdf_nbuf_queue_remove(&ipa_ctx->pm_queue_head))
	       != NULL)) {
		qdf_spin_unlock_bh(&ipa_ctx->pm_lock);

		pm_tx_cb = (struct wlan_ipa_pm_tx_cb *)skb->cb;
		if (pm_tx_cb->ipa_tx_desc)
			ipa_free_skb(pm_tx_cb->ipa_tx_desc);

		qdf_spin_lock_bh(&ipa_ctx->pm_lock);
	}
	qdf_spin_unlock_bh(&ipa_ctx->pm_lock);
}

QDF_STATUS wlan_ipa_cleanup(struct wlan_ipa_priv *ipa_ctx)
{
	struct wlan_ipa_iface_context *iface_context;
	int i;

	if (!wlan_ipa_uc_is_enabled(ipa_ctx->config))
		wlan_ipa_teardown_sys_pipe(ipa_ctx);

	/* Teardown IPA sys_pipe for MCC */
	if (wlan_ipa_uc_sta_is_enabled(ipa_ctx->config))
		wlan_ipa_teardown_sys_pipe(ipa_ctx);

	wlan_ipa_wdi_destroy_rm(ipa_ctx);

	wlan_ipa_flush(ipa_ctx);

	qdf_spinlock_destroy(&ipa_ctx->pm_lock);
	qdf_spinlock_destroy(&ipa_ctx->q_lock);

	/* destroy the interface lock */
	for (i = 0; i < WLAN_IPA_MAX_IFACE; i++) {
		iface_context = &ipa_ctx->iface_context[i];
		qdf_spinlock_destroy(&iface_context->interface_lock);
	}

	if (wlan_ipa_uc_is_enabled(ipa_ctx->config)) {
		wlan_ipa_wdi_cleanup();
		qdf_mutex_destroy(&ipa_ctx->event_lock);
		qdf_mutex_destroy(&ipa_ctx->ipa_lock);
		qdf_list_destroy(&ipa_ctx->pending_event);

	}

	gp_ipa = NULL;

	return QDF_STATUS_SUCCESS;
}

struct wlan_ipa_iface_context
*wlan_ipa_get_iface(struct wlan_ipa_priv *ipa_ctx, uint8_t mode)
{
	struct wlan_ipa_iface_context *iface_ctx = NULL;
	int i;

	for (i = 0; i < WLAN_IPA_MAX_IFACE; i++) {
		iface_ctx = &ipa_ctx->iface_context[i];

		if (iface_ctx->device_mode == mode)
			return iface_ctx;
	}

	return NULL;
}

QDF_STATUS wlan_ipa_uc_ol_init(struct wlan_ipa_priv *ipa_ctx,
			       qdf_device_t osdev)
{
	uint8_t i;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!wlan_ipa_uc_is_enabled(ipa_ctx->config))
		return QDF_STATUS_SUCCESS;

	ipa_debug("enter");

	for (i = 0; i < WLAN_IPA_MAX_SESSION; i++) {
		ipa_ctx->vdev_to_iface[i] = WLAN_IPA_MAX_SESSION;
		ipa_ctx->vdev_offload_enabled[i] = false;
	}

	/* Set the lowest bandwidth to start with */
	status = wlan_ipa_set_perf_level(ipa_ctx, 0, 0);
	if (status != QDF_STATUS_SUCCESS) {
		ipa_err("Set perf level failed: %d", status);
		qdf_ipa_rm_inactivity_timer_destroy(
					QDF_IPA_RM_RESOURCE_WLAN_PROD);
		goto fail_return;
	}

	if (cdp_ipa_get_resource(ipa_ctx->dp_soc, ipa_ctx->dp_pdev)) {
		ipa_err("IPA UC resource alloc fail");
		status = QDF_STATUS_E_FAILURE;
		goto fail_return;
	}

	if (true == ipa_ctx->uc_loaded) {
		status = wlan_ipa_wdi_setup(ipa_ctx, osdev);
		if (status) {
			ipa_err("Failure to setup IPA pipes (status=%d)",
				status);
			status = QDF_STATUS_E_FAILURE;
			goto fail_return;
		}

		cdp_ipa_set_doorbell_paddr(ipa_ctx->dp_soc, ipa_ctx->dp_pdev);
		wlan_ipa_init_metering(ipa_ctx);
	}

fail_return:
	ipa_debug("exit: status=%d", status);
	return status;
}

QDF_STATUS wlan_ipa_uc_ol_deinit(struct wlan_ipa_priv *ipa_ctx)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	ipa_debug("enter");

	if (!wlan_ipa_uc_is_enabled(ipa_ctx->config))
		return status;

	if (!ipa_ctx->ipa_pipes_down)
		wlan_ipa_uc_disable_pipes(ipa_ctx);

	if (true == ipa_ctx->uc_loaded) {
		status = cdp_ipa_cleanup(ipa_ctx->dp_soc,
					 ipa_ctx->tx_pipe_handle,
					 ipa_ctx->rx_pipe_handle);
		if (status)
			ipa_err("Failure to cleanup IPA pipes (status=%d)",
				status);
	}

	ipa_debug("exit: ret=%d", status);
	return status;
}
