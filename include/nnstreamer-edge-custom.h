/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2024 Gichan Jang <gichan2.jang@samsung.com>
 *
 * @file   nnstreamer-edge-custom.h
 * @date   14 Aug 2024
 * @brief  APIs for NNStreamer-edge custom connection.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#ifndef __NNSTREAMER_EDGE_CUSTOM_H__
#define __NNSTREAMER_EDGE_CUSTOM_H__

#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "nnstreamer-edge.h"

typedef struct _NnsEdgeCustomDef
{
  char *(*nns_edge_get_connection_type) ();
  int (*nns_edge_custom_create) (void **priv);
  int (*nns_edge_custom_close) (void *priv);
  int (*nns_edge_custom_start) (void *priv);
  int (*nns_edge_custom_stop) (void *priv);
  int (*nns_edge_custom_connect) (void *priv);
  int (*nns_edge_custom_subscribe) (void *priv);
  int (*nns_edge_custom_is_connected) (void *priv);
  int (*nns_edge_custom_set_event_cb) (void *priv, nns_edge_event_cb cb, void *user_data);
  int (*nns_edge_custom_send_data) (void *priv, nns_edge_data_h data_h);
  int (*nns_edge_custom_set_option) (void *priv, const char *key, const char *value);
  char *(*nns_edge_custom_get_option) (void *priv, const char *key);
} NnsEdgeCustomDef;

void* nns_edge_custom_get_handle ();

#endif /* __NNSTREAMER_EDGE_CUSTOM_H__ */
