/*
 * Copyright (c) 2013-2015 The Linux Foundation. All rights reserved.
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

/**
 * DOC: wlan_hdd_debugfs.c
 *
 * This driver currently supports the following debugfs files:
 * wlan_wcnss/wow_enable to enable/disable WoWL.
 * wlan_wcnss/wow_pattern to configure WoWL patterns.
 * wlan_wcnss/pattern_gen to configure periodic TX patterns.
 */

#ifdef WLAN_OPEN_SOURCE
#include <wlan_hdd_includes.h>
#include <wlan_hdd_wowl.h>
#include <cds_sched.h>

#define MAX_USER_COMMAND_SIZE_WOWL_ENABLE 8
#define MAX_USER_COMMAND_SIZE_WOWL_PATTERN 512
#define MAX_USER_COMMAND_SIZE_FRAME 4096

/**
 * __wcnss_wowenable_write() - wow_enable debugfs handler
 * @file: debugfs file handle
 * @buf: text being written to the debugfs
 * @count: size of @buf
 * @ppos: (unused) offset into the virtual file system
 *
 * Return: number of bytes processed
 */
static ssize_t __wcnss_wowenable_write(struct file *file,
				     const char __user *buf, size_t count,
				     loff_t *ppos)
{
	hdd_adapter_t *pAdapter;
	hdd_context_t *hdd_ctx;
	char cmd[MAX_USER_COMMAND_SIZE_WOWL_ENABLE + 1];
	char *sptr, *token;
	uint8_t wow_enable = 0;
	uint8_t wow_mp = 0;
	uint8_t wow_pbm = 0;
	int ret;

	ENTER();

	pAdapter = (hdd_adapter_t *)file->private_data;
	if ((NULL == pAdapter) || (WLAN_HDD_ADAPTER_MAGIC != pAdapter->magic)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_FATAL,
			  "%s: Invalid adapter or adapter has invalid magic.",
			  __func__);

		return -EINVAL;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(pAdapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;


	if (!sme_is_feature_supported_by_fw(WOW)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Wake-on-Wireless feature is not supported in firmware!",
			  __func__);

		return -EINVAL;
	}

	if (count > MAX_USER_COMMAND_SIZE_WOWL_ENABLE) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Command length is larger than %d bytes.",
			  __func__, MAX_USER_COMMAND_SIZE_WOWL_ENABLE);

		return -EINVAL;
	}

	/* Get command from user */
	if (copy_from_user(cmd, buf, count))
		return -EFAULT;
	cmd[count] = '\0';
	sptr = cmd;

	/* Get enable or disable wow */
	token = strsep(&sptr, " ");
	if (!token)
		return -EINVAL;
	if (kstrtou8(token, 0, &wow_enable))
		return -EINVAL;

	/* Disable wow */
	if (!wow_enable) {
		if (!hdd_exit_wowl(pAdapter)) {
			CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				  "%s: hdd_exit_wowl failed!", __func__);

			return -EFAULT;
		}

		return count;
	}

	/* Get enable or disable magic packet mode */
	token = strsep(&sptr, " ");
	if (!token)
		return -EINVAL;
	if (kstrtou8(token, 0, &wow_mp))
		return -EINVAL;
	if (wow_mp > 1)
		wow_mp = 1;

	/* Get enable or disable pattern byte matching mode */
	token = strsep(&sptr, " ");
	if (!token)
		return -EINVAL;
	if (kstrtou8(token, 0, &wow_pbm))
		return -EINVAL;
	if (wow_pbm > 1)
		wow_pbm = 1;

	if (!hdd_enter_wowl(pAdapter, wow_mp, wow_pbm)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: hdd_enter_wowl failed!", __func__);

		return -EFAULT;
	}
	EXIT();
	return count;
}

/**
 * wcnss_wowenable_write() - SSR wrapper for wcnss_wowenable_write
 * @file: file pointer
 * @buf: buffer
 * @count: count
 * @ppos: position pointer
 *
 * Return: 0 on success, error number otherwise
 */
static ssize_t wcnss_wowenable_write(struct file *file,
				 const char __user *buf,
				 size_t count, loff_t *ppos)
{
	ssize_t ret;

	cds_ssr_protect(__func__);
	ret = __wcnss_wowenable_write(file, buf, count, ppos);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __wcnss_wowpattern_write() - wow_pattern debugfs handler
 * @file: debugfs file handle
 * @buf: text being written to the debugfs
 * @count: size of @buf
 * @ppos: (unused) offset into the virtual file system
 *
 * Return: number of bytes processed
 */
static ssize_t __wcnss_wowpattern_write(struct file *file,
				      const char __user *buf, size_t count,
				      loff_t *ppos)
{
	hdd_adapter_t *pAdapter = (hdd_adapter_t *) file->private_data;
	hdd_context_t *hdd_ctx;
	char cmd[MAX_USER_COMMAND_SIZE_WOWL_PATTERN + 1];
	char *sptr, *token;
	uint8_t pattern_idx = 0;
	uint8_t pattern_offset = 0;
	char *pattern_buf;
	char *pattern_mask;
	int ret;

	ENTER();

	if ((NULL == pAdapter) || (WLAN_HDD_ADAPTER_MAGIC != pAdapter->magic)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_FATAL,
			  "%s: Invalid adapter or adapter has invalid magic.",
			  __func__);

		return -EINVAL;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(pAdapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;

	if (!sme_is_feature_supported_by_fw(WOW)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Wake-on-Wireless feature is not supported in firmware!",
			  __func__);

		return -EINVAL;
	}

	if (count > MAX_USER_COMMAND_SIZE_WOWL_PATTERN) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Command length is larger than %d bytes.",
			  __func__, MAX_USER_COMMAND_SIZE_WOWL_PATTERN);

		return -EINVAL;
	}

	/* Get command from user */
	if (copy_from_user(cmd, buf, count))
		return -EFAULT;
	cmd[count] = '\0';
	sptr = cmd;

	/* Get pattern idx */
	token = strsep(&sptr, " ");
	if (!token)
		return -EINVAL;

	if (kstrtou8(token, 0, &pattern_idx))
		return -EINVAL;

	/* Get pattern offset */
	token = strsep(&sptr, " ");

	/* Delete pattern if no further argument */
	if (!token) {
		hdd_del_wowl_ptrn_debugfs(pAdapter, pattern_idx);

		return count;
	}

	if (kstrtou8(token, 0, &pattern_offset))
		return -EINVAL;

	/* Get pattern */
	token = strsep(&sptr, " ");
	if (!token)
		return -EINVAL;

	pattern_buf = token;

	/* Get pattern mask */
	token = strsep(&sptr, " ");
	if (!token)
		return -EINVAL;

	pattern_mask = token;
	pattern_mask[strlen(pattern_mask) - 1] = '\0';

	hdd_add_wowl_ptrn_debugfs(pAdapter, pattern_idx, pattern_offset,
				  pattern_buf, pattern_mask);
	EXIT();
	return count;
}

/**
 * wcnss_wowpattern_write() - SSR wrapper for __wcnss_wowpattern_write
 * @file: file pointer
 * @buf: buffer
 * @count: count
 * @ppos: position pointer
 *
 * Return: 0 on success, error number otherwise
 */
static ssize_t wcnss_wowpattern_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	ssize_t ret;

	cds_ssr_protect(__func__);
	ret = __wcnss_wowpattern_write(file, buf, count, ppos);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * wcnss_patterngen_write() - pattern_gen debugfs handler
 * @file: debugfs file handle
 * @buf: text being written to the debugfs
 * @count: size of @buf
 * @ppos: (unused) offset into the virtual file system
 *
 * Return: number of bytes processed
 */
static ssize_t __wcnss_patterngen_write(struct file *file,
				      const char __user *buf, size_t count,
				      loff_t *ppos)
{
	hdd_adapter_t *pAdapter;
	hdd_context_t *pHddCtx;
	tSirAddPeriodicTxPtrn *addPeriodicTxPtrnParams;
	tSirDelPeriodicTxPtrn *delPeriodicTxPtrnParams;

	char *cmd, *sptr, *token;
	uint8_t pattern_idx = 0;
	uint8_t pattern_duration = 0;
	char *pattern_buf;
	uint16_t pattern_len = 0;
	uint16_t i = 0;
	CDF_STATUS status;
	int ret;

	ENTER();

	pAdapter = (hdd_adapter_t *)file->private_data;
	if ((NULL == pAdapter) || (WLAN_HDD_ADAPTER_MAGIC != pAdapter->magic)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_FATAL,
			  "%s: Invalid adapter or adapter has invalid magic.",
			  __func__);

		return -EINVAL;
	}

	pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
	ret = wlan_hdd_validate_context(pHddCtx);
	if (0 != ret)
		return ret;

	if (!sme_is_feature_supported_by_fw(WLAN_PERIODIC_TX_PTRN)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Periodic Tx Pattern Offload feature is not supported in firmware!",
			  __func__);
		return -EINVAL;
	}

	/* Get command from user */
	if (count <= MAX_USER_COMMAND_SIZE_FRAME)
		cmd = cdf_mem_malloc(count + 1);
	else {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Command length is larger than %d bytes.",
			  __func__, MAX_USER_COMMAND_SIZE_FRAME);

		return -EINVAL;
	}

	if (!cmd) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  FL("Memory allocation for cmd failed!"));
		return -ENOMEM;
	}

	if (copy_from_user(cmd, buf, count)) {
		cdf_mem_free(cmd);
		return -EFAULT;
	}
	cmd[count] = '\0';
	sptr = cmd;

	/* Get pattern idx */
	token = strsep(&sptr, " ");
	if (!token)
		goto failure;
	if (kstrtou8(token, 0, &pattern_idx))
		goto failure;

	if (pattern_idx > (MAXNUM_PERIODIC_TX_PTRNS - 1)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Pattern index %d is not in the range (0 ~ %d).",
			  __func__, pattern_idx, MAXNUM_PERIODIC_TX_PTRNS - 1);

		goto failure;
	}

	/* Get pattern duration */
	token = strsep(&sptr, " ");
	if (!token)
		goto failure;
	if (kstrtou8(token, 0, &pattern_duration))
		goto failure;

	/* Delete pattern using index if duration is 0 */
	if (!pattern_duration) {
		delPeriodicTxPtrnParams =
			cdf_mem_malloc(sizeof(tSirDelPeriodicTxPtrn));
		if (!delPeriodicTxPtrnParams) {
			CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				  FL("Memory allocation failed!"));
			cdf_mem_free(cmd);
			return -ENOMEM;
		}
		delPeriodicTxPtrnParams->ucPtrnId = pattern_idx;
		delPeriodicTxPtrnParams->ucPatternIdBitmap = 1 << pattern_idx;
		cdf_copy_macaddr(&delPeriodicTxPtrnParams->mac_address,
				 &pAdapter->macAddressCurrent);

		/* Delete pattern */
		status = sme_del_periodic_tx_ptrn(pHddCtx->hHal,
						  delPeriodicTxPtrnParams);
		if (CDF_STATUS_SUCCESS != status) {
			CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				  "%s: sme_del_periodic_tx_ptrn() failed!",
				  __func__);

			cdf_mem_free(delPeriodicTxPtrnParams);
			goto failure;
		}
		cdf_mem_free(cmd);
		cdf_mem_free(delPeriodicTxPtrnParams);
		return count;
	}

	/*
	 * In SAP mode allow configuration without any connection check
	 * In STA mode check if it's in connected state before adding
	 * patterns
	 */
	hdd_info("device mode %d", pAdapter->device_mode);
	if ((WLAN_HDD_INFRA_STATION == pAdapter->device_mode) &&
	    (!hdd_conn_is_connected(WLAN_HDD_GET_STATION_CTX_PTR(pAdapter)))) {
			CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
				"%s: Not in Connected state!", __func__);
			goto failure;
	}

	/* Get pattern */
	token = strsep(&sptr, " ");
	if (!token)
		goto failure;

	pattern_buf = token;
	pattern_buf[strlen(pattern_buf) - 1] = '\0';
	pattern_len = strlen(pattern_buf);

	/* Since the pattern is a hex string, 2 characters represent 1 byte. */
	if (pattern_len % 2) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Malformed pattern!", __func__);

		goto failure;
	} else
		pattern_len >>= 1;

	if (pattern_len < 14 || pattern_len > PERIODIC_TX_PTRN_MAX_SIZE) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: Not an 802.3 frame!", __func__);

		goto failure;
	}

	addPeriodicTxPtrnParams = cdf_mem_malloc(sizeof(tSirAddPeriodicTxPtrn));
	if (!addPeriodicTxPtrnParams) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  FL("Memory allocation failed!"));
		cdf_mem_free(cmd);
		return -ENOMEM;
	}

	addPeriodicTxPtrnParams->ucPtrnId = pattern_idx;
	addPeriodicTxPtrnParams->usPtrnIntervalMs = pattern_duration * 500;
	addPeriodicTxPtrnParams->ucPtrnSize = pattern_len;
	cdf_copy_macaddr(&addPeriodicTxPtrnParams->mac_address,
			 &pAdapter->macAddressCurrent);

	/* Extract the pattern */
	for (i = 0; i < addPeriodicTxPtrnParams->ucPtrnSize; i++) {
		addPeriodicTxPtrnParams->ucPattern[i] =
			(hex_to_bin(pattern_buf[0]) << 4) +
			hex_to_bin(pattern_buf[1]);

		/* Skip to next byte */
		pattern_buf += 2;
	}

	/* Add pattern */
	status = sme_add_periodic_tx_ptrn(pHddCtx->hHal,
					  addPeriodicTxPtrnParams);
	if (CDF_STATUS_SUCCESS != status) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_ERROR,
			  "%s: sme_add_periodic_tx_ptrn() failed!", __func__);

		cdf_mem_free(addPeriodicTxPtrnParams);
		goto failure;
	}
	cdf_mem_free(cmd);
	cdf_mem_free(addPeriodicTxPtrnParams);
	EXIT();
	return count;

failure:
	cdf_mem_free(cmd);
	return -EINVAL;
}

/**
 * wcnss_patterngen_write() - SSR wrapper for __wcnss_patterngen_write
 * @file: file pointer
 * @buf: buffer
 * @count: count
 * @ppos: position pointer
 *
 * Return: 0 on success, error number otherwise
 */
static ssize_t wcnss_patterngen_write(struct file *file,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	ssize_t ret;

	cds_ssr_protect(__func__);
	ret = __wcnss_patterngen_write(file, buf, count, ppos);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __wcnss_debugfs_open() - Generic debugfs open() handler
 * @inode: inode of the debugfs file
 * @file: file handle of the debugfs file
 *
 * Return: 0
 */
static int __wcnss_debugfs_open(struct inode *inode, struct file *file)
{
	hdd_adapter_t *adapter;
	hdd_context_t *hdd_ctx;
	int ret;

	ENTER();

	if (inode->i_private)
		file->private_data = inode->i_private;

	adapter = (hdd_adapter_t *)file->private_data;
	if ((NULL == adapter) || (WLAN_HDD_ADAPTER_MAGIC != adapter->magic)) {
		CDF_TRACE(CDF_MODULE_ID_HDD, CDF_TRACE_LEVEL_FATAL,
			  "%s: Invalid adapter or adapter has invalid magic.",
			  __func__);
		return -EINVAL;
	}

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	ret = wlan_hdd_validate_context(hdd_ctx);
	if (0 != ret)
		return ret;
	EXIT();
	return 0;
}

/**
 * wcnss_debugfs_open() - SSR wrapper for __wcnss_debugfs_open
 * @inode: inode pointer
 * @file: file pointer
 *
 * Return: 0 on success, error number otherwise
 */
static int wcnss_debugfs_open(struct inode *inode, struct file *file)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __wcnss_debugfs_open(inode, file);
	cds_ssr_unprotect(__func__);

	return ret;
}

static const struct file_operations fops_wowenable = {
	.write = wcnss_wowenable_write,
	.open = wcnss_debugfs_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

static const struct file_operations fops_wowpattern = {
	.write = wcnss_wowpattern_write,
	.open = wcnss_debugfs_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

static const struct file_operations fops_patterngen = {
	.write = wcnss_patterngen_write,
	.open = wcnss_debugfs_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

/**
 * hdd_debugfs_init() - Initialize debugfs interface
 * @pAdapter: primary wlan adapter
 *
 * Register support for the debugfs files supported by the driver.
 *
 * NB: The current implementation only supports debugfs operations
 * on the primary interface, i.e. wlan0
 *
 * Return: CDF_STATUS_SUCCESS if all files registered,
 *	   CDF_STATUS_E_FAILURE on failure
 */
CDF_STATUS hdd_debugfs_init(hdd_adapter_t *pAdapter)
{
	hdd_context_t *pHddCtx = WLAN_HDD_GET_CTX(pAdapter);
	pHddCtx->debugfs_phy = debugfs_create_dir("wlan_wcnss", 0);

	if (NULL == pHddCtx->debugfs_phy)
		return CDF_STATUS_E_FAILURE;

	if (NULL == debugfs_create_file("wow_enable", S_IRUSR | S_IWUSR,
					pHddCtx->debugfs_phy, pAdapter,
					&fops_wowenable))
		return CDF_STATUS_E_FAILURE;

	if (NULL == debugfs_create_file("wow_pattern", S_IRUSR | S_IWUSR,
					pHddCtx->debugfs_phy, pAdapter,
					&fops_wowpattern))
		return CDF_STATUS_E_FAILURE;

	if (NULL == debugfs_create_file("pattern_gen", S_IRUSR | S_IWUSR,
					pHddCtx->debugfs_phy, pAdapter,
					&fops_patterngen))
		return CDF_STATUS_E_FAILURE;

	return CDF_STATUS_SUCCESS;
}

/**
 * hdd_debugfs_exit() - Shutdown debugfs interface
 * @pHddCtx: the global HDD context
 *
 * Unregister support for the debugfs files supported by the driver.
 *
 * Return: None
 */
void hdd_debugfs_exit(hdd_context_t *pHddCtx)
{
	debugfs_remove_recursive(pHddCtx->debugfs_phy);
}
#endif /* #ifdef WLAN_OPEN_SOURCE */
