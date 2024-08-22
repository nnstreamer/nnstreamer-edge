/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2024 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-custom-impl.h
 * @date   21 Aug 2024
 * @brief  Internal interface to support communication using custom library.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#ifndef __NNSTREAMER_EDGE_CUSTOM_IMPL_H__
#define __NNSTREAMER_EDGE_CUSTOM_IMPL_H__

#include "nnstreamer-edge-custom.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Data structure for edge custom connection.
 */
typedef struct
{
  void *dl_handle;
  nns_edge_custom_s *instance;
  void *priv;
} custom_connection_s;

/**
 * @brief Internal function to load custom connection from library.
 */
int nns_edge_custom_load (custom_connection_s *custom, const char *lib_path);

/**
 * @brief Internal function to release custom connection.
 */
int nns_edge_custom_release (custom_connection_s *custom);

/**
 * @brief Internal function to start custom connection.
 */
int nns_edge_custom_start (custom_connection_s *custom);

/**
 * @brief Internal function to stop custom connection.
 */
int nns_edge_custom_stop (custom_connection_s *custom);

/**
 * @brief Internal function to set the event callback of custom connection.
 */
int nns_edge_custom_set_event_callback (custom_connection_s *custom, nns_edge_event_cb cb, void *user_data);

/**
 * @brief Internal function to connect custom connection.
 */
int nns_edge_custom_connect (custom_connection_s *custom);

/**
 * @brief Internal function to check custom connection.
 */
int nns_edge_custom_is_connected (custom_connection_s *custom);

/**
 * @brief Internal function to send data to custom connection.
 */
int nns_edge_custom_send_data (custom_connection_s *custom, nns_edge_data_h data_h);

/**
 * @brief Internal function to set information to custom connection.
 */
int nns_edge_custom_set_info (custom_connection_s *custom, const char *key, const char *value);

/**
 * @brief Internal function to get information from custom connection.
 */
int nns_edge_custom_get_info (custom_connection_s *custom, const char *key, char **value);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_CUSTOM_IMPL_H__ */
