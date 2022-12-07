/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-util.c
 * @date   24 August 2022
 * @brief  Utility functions.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Jaeyun Jung <jy1210.jung@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>

#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"

/**
 * @brief Generate ID.
 */
int64_t
nns_edge_generate_id (void)
{
  struct timespec ts;
  int64_t _id;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  _id = ((int64_t) ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;

  return _id;
}

/**
 * @brief Internal util function to get available port number.
 */
int
nns_edge_get_available_port (void)
{
  struct sockaddr_in sin;
  int port = 0, sock;
  socklen_t len = sizeof (struct sockaddr);

  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = INADDR_ANY;
  sin.sin_port = 0;

  sock = socket (AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    nns_edge_loge ("Failed to get available port, socket creation failure.");
    return 0;
  }

  if (bind (sock, (struct sockaddr *) &sin, sizeof (struct sockaddr)) == 0) {
    if (getsockname (sock, (struct sockaddr *) &sin, &len) == 0) {
      port = ntohs (sin.sin_port);
      nns_edge_logd ("Available port number: %d", port);
    } else {
      nns_edge_logw ("Failed to read local socket info.");
    }
  }
  close (sock);

  return port;
}

/**
 * @brief Get host string (host:port). Caller should release returned string using nns_edge_free().
 */
char *
nns_edge_get_host_string (const char *host, const int port)
{
  return nns_edge_strdup_printf ("%s:%d", host, port);
}

/**
 * @brief Parse string and get host string (host:port).
 */
void
nns_edge_parse_host_string (const char *host_str, char **host, int *port)
{
  char *p = strchr (host_str, ':');

  if (p) {
    *host = nns_edge_strndup (host_str, (p - host_str));
    *port = (int) strtoll (p + 1, NULL, 10);
  }
}

/**
 * @brief Parse string and get port number. Return negative value when failed to get port number.
 */
int
nns_edge_parse_port_number (const char *port_str)
{
  int port;

  if (!port_str)
    return -1;

  port = (int) strtoll (port_str, NULL, 10);

  if (!PORT_IS_VALID (port)) {
    nns_edge_loge ("Invalid port number %d.", port);
    port = -1;
  }

  return port;
}

/**
 * @brief Allocate new memory. The max size is SIZE_MAX.
 * @note Caller should release newly allocated memory using nns_edge_free().
 */
void *
nns_edge_malloc (nns_size_t size)
{
  void *mem = NULL;

  if (size > 0 && size <= SIZE_MAX)
    mem = malloc (size);

  if (!mem)
    nns_edge_loge ("Failed to allocate memory (%llu).", size);

  return mem;
}

/**
 * @brief Free allocated memory.
 */
void
nns_edge_free (void *data)
{
  if (data)
    free (data);
}

/**
 * @brief Allocate new memory and copy bytes.
 * @note Caller should release newly allocated memory using nns_edge_free().
 */
void *
nns_edge_memdup (const void *data, nns_size_t size)
{
  void *mem = NULL;

  if (data && size > 0) {
    mem = nns_edge_malloc (size);

    if (mem)
      memcpy (mem, data, size);
  }

  return mem;
}

/**
 * @brief Allocate new memory and copy string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *
nns_edge_strdup (const char *str)
{
  char *new_str = NULL;

  if (str)
    new_str = nns_edge_strndup (str, strlen (str));

  return new_str;
}

/**
 * @brief Allocate new memory and copy bytes of string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *
nns_edge_strndup (const char *str, nns_size_t len)
{
  char *new_str = NULL;

  if (str) {
    new_str = (char *) nns_edge_malloc (len + 1);

    if (new_str) {
      strncpy (new_str, str, len);
      new_str[len] = '\0';
    }
  }

  return new_str;
}

/**
 * @brief Allocate new memory and print formatted string.
 * @note Caller should release newly allocated string using nns_edge_free().
 */
char *
nns_edge_strdup_printf (const char *format, ...)
{
  char *new_str = NULL;
  va_list args;
  int len;

  va_start (args, format);
  len = vasprintf (&new_str, format, args);
  if (len < 0)
    new_str = NULL;
  va_end (args);

  return new_str;
}
