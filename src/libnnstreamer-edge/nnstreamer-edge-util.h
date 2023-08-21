/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-util.h
 * @date   24 August 2022
 * @brief  Utility functions.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Jaeyun Jung <jy1210.jung@samsung.com>
 * @note   This file is internal header for nnstreamer-edge. DO NOT export this file.
 * @bug    No known bugs except for NYI items
 */

#ifndef __NNSTREAMER_EDGE_UTIL_H__
#define __NNSTREAMER_EDGE_UTIL_H__

#include <netinet/tcp.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
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

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

/**
 * @brief The max size to allocate memory. This would be defined in C99 limits.h.
 */
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t) -1)
#endif

#define STR_IS_VALID(s) ((s) && (s)[0] != '\0')
#define PORT_IS_VALID(p) ((p) > 0 && (p) <= 65535)
#define SAFE_FREE(p) do { if (p) { free (p); (p) = NULL; } } while (0)

#define NNS_EDGE_MAGIC 0xfeedfeed
#define NNS_EDGE_MAGIC_DEAD 0xdeaddead
#define nns_edge_handle_is_valid(h) ((h) && *((uint32_t *)(h)) == NNS_EDGE_MAGIC)
#define nns_edge_handle_set_magic(h,m) do { if (h) *((uint32_t *)(h)) = (m); } while (0)

#define nns_edge_lock_init(h) do { pthread_mutex_init (&(h)->lock, NULL); } while (0)
#define nns_edge_lock_destroy(h) do { pthread_mutex_destroy (&(h)->lock); } while (0)
#define nns_edge_lock(h) do { pthread_mutex_lock (&(h)->lock); } while (0)
#define nns_edge_unlock(h) do { pthread_mutex_unlock (&(h)->lock); } while (0)

#define nns_edge_cond_init(h) do { pthread_cond_init (&(h)->cond, NULL); } while (0)
#define nns_edge_cond_destroy(h) do { pthread_cond_destroy (&(h)->cond); } while (0)
#define nns_edge_cond_wait(h) do { pthread_cond_wait (&(h)->cond, &(h)->lock); } while (0)
#define nns_edge_cond_wait_until(h,ms) do { \
    if ((ms) > 0) { \
      struct timespec ts; \
      struct timeval now; \
      gettimeofday (&now, NULL); \
      ts.tv_sec = now.tv_sec + (ms) / 1000; \
      ts.tv_nsec = now.tv_usec * 1000 + ((ms) % 1000) * 1000000; \
      pthread_cond_timedwait (&(h)->cond, &(h)->lock, &ts); \
    } else { \
      pthread_cond_wait (&(h)->cond, &(h)->lock); \
    } \
  } while (0)
#define nns_edge_cond_signal(h) do { pthread_cond_signal (&(h)->cond); } while (0)

/**
 * @brief Internal data structure for raw data.
 */
typedef struct {
  void *data;
  nns_size_t data_len;
  nns_edge_data_destroy_cb destroy_cb;
} nns_edge_raw_data_s;

/**
 * @brief Generate client ID.
 */
int64_t nns_edge_generate_id (void);

/**
 * @brief Generate the version key.
 */
uint64_t nns_edge_generate_version_key (void);

/**
 * @brief Parse the version key.
 */
bool nns_edge_parse_version_key (const uint64_t version_key, unsigned int *major, unsigned int *minor, unsigned int *micro);

/**
 * @brief Get available port number.
 */
int nns_edge_get_available_port (void);

/**
 * @brief Get host string (host:port). Caller should release returned string using nns_edge_free().
 */
char *nns_edge_get_host_string (const char *host, const int port);

/**
 * @brief Parse string and get host string (host:port).
 */
void nns_edge_parse_host_string (const char *host_str, char **host, int *port);

/**
 * @brief Parse string and get port number. Return negative value when failed to get port number.
 */
int nns_edge_parse_port_number (const char *port_str);

/**
 * @brief Allocate new memory. The max size is SIZE_MAX.
 * @note Caller should release newly allocated memory using nns_edge_free().
 */
void *nns_edge_malloc (nns_size_t size);

/**
 * @brief Free allocated memory.
 */
void nns_edge_free (void *data);

/**
 * @brief Allocate new memory and copy bytes.
 * @note Caller should release newly allocated memory using nns_edge_free().
 */
void *nns_edge_memdup (const void *data, nns_size_t size);

/**
 * @brief Allocate new memory and copy string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *nns_edge_strdup (const char *str);

/**
 * @brief Allocate new memory and copy bytes of string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *nns_edge_strndup (const char *str, nns_size_t len);

/**
 * @brief Allocate new memory and print formatted string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *nns_edge_strdup_printf (const char *format, ...);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_UTIL_H__ */
