/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-common.h
 * @date   6 April 2022
 * @brief  Common util functions for nnstreamer edge.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 * @note   This file is internal header for nnstreamer edge utils. DO NOT export this file.
 */

#ifndef __NNSTREAMER_EDGE_COMMON_H__
#define __NNSTREAMER_EDGE_COMMON_H__

#include <glib.h> /** @todo remove glib */
#include <fcntl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <pthread.h>
#include <unistd.h>
#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Utility to silence unused parameter warning for intentionally unused parameters (e.g., callback functions of a framework)
 */
#ifndef UNUSED
#define UNUSED(expr) do { (void)(expr); } while (0)
#endif

#define STR_IS_VALID(s) ((s) && (s)[0] != '\0')
#define SAFE_FREE(p) do { if (p) { free (p); (p) = NULL; } } while (0)

#define NNS_EDGE_MAGIC 0xfeedfeed
#define NNS_EDGE_MAGIC_DEAD 0xdeaddead
#define NNS_EDGE_MAGIC_IS_VALID(h) ((h) && (h)->magic == NNS_EDGE_MAGIC)

#define nns_edge_lock_init(h) do { pthread_mutex_init (&(h)->lock, NULL); } while (0)
#define nns_edge_lock_destroy(h) do { pthread_mutex_destroy (&(h)->lock); } while (0)
#define nns_edge_lock(h) do { pthread_mutex_lock (&(h)->lock); } while (0)
#define nns_edge_unlock(h) do { pthread_mutex_unlock (&(h)->lock); } while (0)

/**
 * @brief Internal data structure for raw data.
 */
typedef struct {
  void *data;
  size_t data_len;
  nns_edge_data_destroy_cb destroy_cb;
} nns_edge_raw_data_s;

/**
 * @brief Internal data structure for edge data.
 */
typedef struct {
  unsigned int magic;
  unsigned int num;
  nns_edge_raw_data_s data[NNS_EDGE_DATA_LIMIT];
  GHashTable *info_table;
} nns_edge_data_s;

/**
 * @brief Internal data structure for edge event.
 */
typedef struct {
  unsigned int magic;
  nns_edge_event_e event;
  nns_edge_raw_data_s data;
} nns_edge_event_s;

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

/**
 * @brief Internal util function to get available port number.
 */
int nns_edge_get_available_port (void);

/**
 * @brief Free allocated memory.
 */
void nns_edge_free (void *data);

/**
 * @brief Allocate new memory and copy bytes.
 * @note Caller should release newly allocated memory using nns_edge_free().
 */
void *nns_edge_memdup (const void *data, size_t size);

/**
 * @brief Allocate new memory and copy string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *nns_edge_strdup (const char *str);

/**
 * @brief Allocate new memory and copy bytes of string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *nns_edge_strndup (const char *str, size_t len);

/**
 * @brief Allocate new memory and print formatted string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *nns_edge_strdup_printf (const char *format, ...);

/**
 * @brief Create nnstreamer edge event.
 * @note This is internal function for edge event.
 */
int nns_edge_event_create (nns_edge_event_e event, nns_edge_event_h * event_h);

/**
 * @brief Destroy nnstreamer edge event.
 * @note This is internal function for edge event.
 */
int nns_edge_event_destroy (nns_edge_event_h event_h);

/**
 * @brief Set event data.
 * @note This is internal function for edge event.
 */
int nns_edge_event_set_data (nns_edge_event_h event_h, void *data, size_t data_len, nns_edge_data_destroy_cb destroy_cb);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_COMMON_H__ */
