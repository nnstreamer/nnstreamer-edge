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

#if defined(__TIZEN__)
#include <dlog.h>

#define nns_edge_logd(...) dlog_print (DLOG_DEBUG, TAG_NAME, __VA_ARGS__)
#define nns_edge_logi(...) dlog_print (DLOG_INFO, TAG_NAME, __VA_ARGS__)
#define nns_edge_logw(...) dlog_print (DLOG_WARN, TAG_NAME, __VA_ARGS__)
#define nns_edge_loge(...) dlog_print (DLOG_ERROR, TAG_NAME, __VA_ARGS__)
#define nns_edge_logf(...) dlog_print (DLOG_FATAL, TAG_NAME, __VA_ARGS__)
#elif defined(__ANDROID__)
#include <android/log.h>

#define nns_edge_logd(...) __android_log_print (ANDROID_LOG_DEBUG, TAG_NAME, __VA_ARGS__)
#define nns_edge_logi(...) __android_log_print (ANDROID_LOG_INFO, TAG_NAME, __VA_ARGS__)
#define nns_edge_logw(...) __android_log_print (ANDROID_LOG_WARN, TAG_NAME, __VA_ARGS__)
#define nns_edge_loge(...) __android_log_print (ANDROID_LOG_ERROR, TAG_NAME, __VA_ARGS__)
#define nns_edge_logf(...) __android_log_print (ANDROID_LOG_FATAL, TAG_NAME, __VA_ARGS__)
#else
/**
 * @brief Enumeration for log message.
 */
typedef enum {
  NNS_EDGE_LOG_DEBUG = 0,
  NNS_EDGE_LOG_INFO,
  NNS_EDGE_LOG_WARNING,
  NNS_EDGE_LOG_ERROR,
  NNS_EDGE_LOG_FATAL,
  NNS_EDGE_LOG_NONE
} nns_edge_log_level_e;

/**
 * @brief Internal util function to print log message.
 */
void nns_edge_print_log (nns_edge_log_level_e level, const char *fmt, ...);

#define nns_edge_logi(...) nns_edge_print_log (NNS_EDGE_LOG_INFO, __VA_ARGS__)
#define nns_edge_logw(...) nns_edge_print_log (NNS_EDGE_LOG_WARNING, __VA_ARGS__)
#define nns_edge_loge(...) nns_edge_print_log (NNS_EDGE_LOG_ERROR, __VA_ARGS__)
#define nns_edge_logd(...) nns_edge_print_log (NNS_EDGE_LOG_DEBUG, __VA_ARGS__)
#define nns_edge_logf(...) nns_edge_print_log (NNS_EDGE_LOG_FATAL, __VA_ARGS__)
#endif
#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_LOG_H__ */
