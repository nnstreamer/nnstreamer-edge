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

#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief NNStreamer Edge custom connection definition. This is used to define a custom connection.
 * The user should implement the functions and provide them using nns_edge_custom_get_instance().
 * Refer to the example in nnstreamer-edge-custom-test.c for more details.
 */
typedef struct
{
  const char *(*nns_edge_custom_get_description) ();
  int (*nns_edge_custom_create) (void **priv);
  int (*nns_edge_custom_close) (void *priv);
  int (*nns_edge_custom_start) (void *priv);
  int (*nns_edge_custom_stop) (void *priv);
  int (*nns_edge_custom_connect) (void *priv);
  int (*nns_edge_custom_subscribe) (void *priv);
  int (*nns_edge_custom_is_connected) (void *priv);
  int (*nns_edge_custom_discover) (void *priv);
  int (*nns_edge_custom_set_event_cb) (void *priv, nns_edge_event_cb cb, void *user_data);
  int (*nns_edge_custom_send_data) (void *priv, nns_edge_data_h data_h);
  int (*nns_edge_custom_set_info) (void *priv, const char *key, const char *value);
  int (*nns_edge_custom_get_info) (void *priv, const char *key, char **value);
} nns_edge_custom_s;

/**
 * @brief Get nns edge custom connection instance.
 */
const nns_edge_custom_s * nns_edge_custom_get_instance (void);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_CUSTOM_H__ */
