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
 */

#ifndef __NNSTREAMER_EDGE_LOG_H__
#define __NNSTREAMER_EDGE_LOG_H__

#include <stdio.h>
#include <stdarg.h>

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
 * @brief Internal enumeration for log message.
 */
typedef enum {
  NE_LOG_DEBUG = 0,
  NE_LOG_INFO,
  NE_LOG_WARNING,
  NE_LOG_ERROR,
  NE_LOG_FATAL,
  NE_LOG_NONE
} nns_edge_log_level_e;

/**
 * @brief Internal util function to print log message.
 */
static inline void
nns_edge_print_log (nns_edge_log_level_e level, const char *fmt, ...)
{
  const char *level_str[] = {
    [NE_LOG_DEBUG] = "DEBUG",
    [NE_LOG_INFO] = "INFO",
    [NE_LOG_WARNING] = "WARNING",
    [NE_LOG_ERROR] = "ERROR",
    [NE_LOG_FATAL] = "FATAL",
    [NE_LOG_NONE] = "DEBUG",
  };

  va_list args;

  /** @todo expand log util and handle log level (debug, release) */
  va_start (args, fmt);

  printf ("[%s][%s] ", level_str[level], TAG_NAME);
  vprintf (fmt, args);
  printf ("\n");

  va_end (args);
}

#define nns_edge_logi(...) nns_edge_print_log (NE_LOG_INFO, __VA_ARGS__)
#define nns_edge_logw(...) nns_edge_print_log (NE_LOG_WARNING, __VA_ARGS__)
#define nns_edge_loge(...) nns_edge_print_log (NE_LOG_ERROR, __VA_ARGS__)
#define nns_edge_logd(...) nns_edge_print_log (NE_LOG_DEBUG, __VA_ARGS__)
#define nns_edge_logf(...) nns_edge_print_log (NE_LOG_FATAL, __VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_LOG_H__ */
