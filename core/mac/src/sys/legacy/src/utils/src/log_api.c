/*
 * Copyright (c) 2012-2015 The Linux Foundation. All rights reserved.
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

/*
 * log_api.cc - Handles log messages for all the modules.
 * Author:        Kevin Nguyen
 * Date:          02/27/02
 * History:-
 * 02/11/02       Created.
 * 03/12/02       Rearrange log_debug parameter list and add more params.
 * --------------------------------------------------------------------
 *
 */

#include <sir_common.h>
#include <sir_debug.h>
#include <utils_api.h>
#include <wma_types.h>

#include <stdarg.h>
#include "utils_global.h"
#include "mac_init_api.h"

#include "cdf_trace.h"

#ifdef ANI_OS_TYPE_ANDROID
#include <linux/kernel.h>
#endif

/* --------------------------------------------------------------------- */
/**
 * log_init()
 *
 * FUNCTION:
 * This function is called to prepare the logging utility.
 *
 * LOGIC:
 *
 * ASSUMPTIONS:
 * None.
 *
 * NOTE:
 *
 * @param tpAniSirGlobal Sirius software parameter strucutre pointer
 * @return None
 */
tSirRetStatus log_init(tpAniSirGlobal pMac)
{
	uint32_t i;

	/* Add code to initialize debug level from CFG module */
	/* For now, enable all logging */
	for (i = 0; i < LOG_ENTRY_NUM; i++) {
#ifdef SIR_DEBUG
		pMac->utils.gLogEvtLevel[i] = pMac->utils.gLogDbgLevel[i] =
						      LOG1;
#else
		pMac->utils.gLogEvtLevel[i] = pMac->utils.gLogDbgLevel[i] =
						      LOGW;
#endif
	}
	return eSIR_SUCCESS;

} /*** log_init() ***/

void log_deinit(tpAniSirGlobal pMac)
{
	return;
}

/**
 * log_dbg()
 *
 ***FUNCTION:
 * This function is called to log a debug message.
 *
 ***PARAMS:
 *
 ***LOGIC:
 *
 ***ASSUMPTIONS:
 * None.
 *
 ***NOTE:
 *
 * @param tpAniSirGlobal Sirius software parameter strucutre pointer
 * @param ModId        8-bit modID
 * @param debugLevel   debugging level for this message
 * @param pStr         string parameter pointer
 * @return None
 */

void log_dbg(tpAniSirGlobal pMac, uint8_t modId, uint32_t debugLevel,
	     const char *pStr, ...)
{
#ifdef WLAN_DEBUG
	if (debugLevel > pMac->utils.gLogDbgLevel[LOG_INDEX_FOR_MODULE(modId)])
		return;
	else {
		va_list marker;

		va_start(marker, pStr); /* Initialize variable arguments. */

		log_debug(pMac, modId, debugLevel, pStr, marker);

		va_end(marker); /* Reset variable arguments.      */
	}
#endif
}

CDF_TRACE_LEVEL get_vos_debug_level(uint32_t debugLevel)
{
	switch (debugLevel) {
	case LOGP:
		return CDF_TRACE_LEVEL_FATAL;
	case LOGE:
		return CDF_TRACE_LEVEL_ERROR;
	case LOGW:
		return CDF_TRACE_LEVEL_WARN;
	case LOG1:
		return CDF_TRACE_LEVEL_INFO;
	case LOG2:
		return CDF_TRACE_LEVEL_INFO_HIGH;
	case LOG3:
		return CDF_TRACE_LEVEL_INFO_MED;
	case LOG4:
		return CDF_TRACE_LEVEL_INFO_LOW;
	default:
		return CDF_TRACE_LEVEL_INFO_LOW;
	}
}

static inline CDF_MODULE_ID get_vos_module_id(uint8_t modId)
{
	switch (modId) {
	case SIR_HAL_MODULE_ID:
		return CDF_MODULE_ID_WMA;

	case SIR_LIM_MODULE_ID:
	case SIR_SCH_MODULE_ID:
	case SIR_CFG_MODULE_ID:
	case SIR_MNT_MODULE_ID:
	case SIR_DPH_MODULE_ID:
	case SIR_DBG_MODULE_ID:
		return CDF_MODULE_ID_PE;

	case SIR_SYS_MODULE_ID:
		return CDF_MODULE_ID_SYS;

	case SIR_SMS_MODULE_ID:
		return CDF_MODULE_ID_SME;

	default:
		return CDF_MODULE_ID_SYS;
	}
}

#define LOG_SIZE 256
void log_debug(tpAniSirGlobal pMac, uint8_t modId, uint32_t debugLevel,
	       const char *pStr, va_list marker)
{
	CDF_TRACE_LEVEL cdf_debug_level;
	CDF_MODULE_ID cdf_module_id;
	char logBuffer[LOG_SIZE];

	cdf_debug_level = get_vos_debug_level(debugLevel);
	cdf_module_id = get_vos_module_id(modId);

	vsnprintf(logBuffer, LOG_SIZE - 1, pStr, marker);
	CDF_TRACE(cdf_module_id, cdf_debug_level, "%s", logBuffer);

	/* The caller must check loglevel */
	CDF_ASSERT((debugLevel <=
		    pMac->utils.gLogDbgLevel[LOG_INDEX_FOR_MODULE(modId)])
		   && (LOGP != debugLevel));
} /*** end log_debug() ***/
