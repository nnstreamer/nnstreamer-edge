/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-log.c
 * @date   20 October 2022
 * @brief  Logging functions
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Jaeyun Jung <jy1210.jung@samsung.com>
 * @bug    No known bugs except for NYI items.
 */

#include <stdarg.h>
#include "nnstreamer-edge-log.h"

#if defined(__TIZEN__)
#include <dlog.h>

#define _print_logd(...) dlog_print (DLOG_DEBUG, TAG_NAME, __VA_ARGS__)
#define _print_logi(...) dlog_print (DLOG_INFO, TAG_NAME, __VA_ARGS__)
#define _print_logw(...) dlog_print (DLOG_WARN, TAG_NAME, __VA_ARGS__)
#define _print_loge(...) dlog_print (DLOG_ERROR, TAG_NAME, __VA_ARGS__)
#define _print_logf(...) dlog_print (DLOG_FATAL, TAG_NAME, __VA_ARGS__)
#elif defined(__ANDROID__)
#include <android/log.h>

#define _print_logd(...) __android_log_print (ANDROID_LOG_DEBUG, TAG_NAME, __VA_ARGS__)
#define _print_logi(...) __android_log_print (ANDROID_LOG_INFO, TAG_NAME, __VA_ARGS__)
#define _print_logw(...) __android_log_print (ANDROID_LOG_WARN, TAG_NAME, __VA_ARGS__)
#define _print_loge(...) __android_log_print (ANDROID_LOG_ERROR, TAG_NAME, __VA_ARGS__)
#define _print_logf(...) __android_log_print (ANDROID_LOG_FATAL, TAG_NAME, __VA_ARGS__)
#else
/**
 * @brief Internal util function to print log message.
 */
static inline void
_ne_log_print (nns_edge_log_level_e level, const char *fmt, va_list args)
{
  const char *level_str[] = {
    [NNS_EDGE_LOG_DEBUG] = "DEBUG",
    [NNS_EDGE_LOG_INFO] = "INFO",
    [NNS_EDGE_LOG_WARNING] = "WARNING",
    [NNS_EDGE_LOG_ERROR] = "ERROR",
    [NNS_EDGE_LOG_FATAL] = "FATAL",
    [NNS_EDGE_LOG_NONE] = NULL,
  };

  switch (level) {
    case NNS_EDGE_LOG_WARNING:
      printf ("\033[33m");
      break;
    case NNS_EDGE_LOG_ERROR:
      printf ("\033[31m");
      break;
    case NNS_EDGE_LOG_FATAL:
      printf ("\033[1;31m");
      break;
    default:
      printf ("\033[20m");
      break;
  }

  printf ("[%s][%s] ", level_str[level], TAG_NAME);
  vprintf (fmt, args);
  printf ("\n");
  printf ("\033[0m");
}

#define _print_logd(...) _ne_log_print (NNS_EDGE_LOG_DEBUG, __VA_ARGS__)
#define _print_logi(...) _ne_log_print (NNS_EDGE_LOG_INFO, __VA_ARGS__)
#define _print_logw(...) _ne_log_print (NNS_EDGE_LOG_WARNING, __VA_ARGS__)
#define _print_loge(...) _ne_log_print (NNS_EDGE_LOG_ERROR, __VA_ARGS__)
#define _print_logf(...) _ne_log_print (NNS_EDGE_LOG_FATAL, __VA_ARGS__)
#endif

/**
 * @brief Internal logging level.
 */
static nns_edge_log_level_e g_ne_log_level =
    (DEBUG) ? NNS_EDGE_LOG_DEBUG : NNS_EDGE_LOG_INFO;

/**
 * @brief Set the logging level.
 */
void
nns_edge_set_log_level (nns_edge_log_level_e level)
{
  g_ne_log_level = level;
}

/**
 * @brief Internal util function to print log message.
 */
void
nns_edge_print_log (nns_edge_log_level_e level, const char *fmt, ...)
{
  va_list args;

  if (level < g_ne_log_level)
    return;

  va_start (args, fmt);

  switch (level) {
    case NNS_EDGE_LOG_DEBUG:
      _print_logd (fmt, args);
      break;
    case NNS_EDGE_LOG_INFO:
      _print_logi (fmt, args);
      break;
    case NNS_EDGE_LOG_WARNING:
      _print_logw (fmt, args);
      break;
    case NNS_EDGE_LOG_ERROR:
      _print_loge (fmt, args);
      break;
    case NNS_EDGE_LOG_FATAL:
      _print_logf (fmt, args);
      break;
    default:
      break;
  }

  va_end (args);
}
