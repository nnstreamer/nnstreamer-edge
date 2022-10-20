/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-log.h
 * @date   24 August 2022
 * @brief  Log util.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Jaeyun Jung <jy1210.jung@samsung.com>
 * @note   This file is internal header for nnstreamer-edge. DO NOT export this file.
 * @bug    No known bugs except for NYI items.
 */

#ifndef __NNSTREAMER_EDGE_LOG_H__
#define __NNSTREAMER_EDGE_LOG_H__

#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define TAG_NAME "nnstreamer-edge"

/**
 * @brief Internal util function to print log message.
 */
void nns_edge_print_log (nns_edge_log_level_e level, const char *fmt, ...);

#define nns_edge_logi(...) nns_edge_print_log (NNS_EDGE_LOG_INFO, __VA_ARGS__)
#define nns_edge_logw(...) nns_edge_print_log (NNS_EDGE_LOG_WARNING, __VA_ARGS__)
#define nns_edge_loge(...) nns_edge_print_log (NNS_EDGE_LOG_ERROR, __VA_ARGS__)
#define nns_edge_logd(...) nns_edge_print_log (NNS_EDGE_LOG_DEBUG, __VA_ARGS__)
#define nns_edge_logf(...) nns_edge_print_log (NNS_EDGE_LOG_FATAL, __VA_ARGS__)

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_LOG_H__ */
