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

typedef void *nns_edge_custom_connection_h;

#if defined(ENABLE_CUSTOM_CONNECTION)
/**
 * @brief Internal function to load custom connection from library.
 */
int nns_edge_custom_load (const char *lib_path, nns_edge_custom_connection_h *handle);

/**
 * @brief Internal function to release custom connection.
 */
int nns_edge_custom_release (nns_edge_custom_connection_h handle);

/**
 * @brief Internal function to start custom connection.
 */
int nns_edge_custom_start (nns_edge_custom_connection_h handle);

/**
 * @brief Internal function to stop custom connection.
 */
int nns_edge_custom_stop (nns_edge_custom_connection_h handle);

/**
 * @brief Internal function to discover devices of custom connection.
 */
int nns_edge_custom_discover (nns_edge_custom_connection_h handle);

/**
 * @brief Internal function to set the event callback of custom connection.
 */
int nns_edge_custom_set_event_callback (nns_edge_custom_connection_h handle, nns_edge_event_cb cb, void *user_data);

/**
 * @brief Internal function to connect custom connection.
 */
int nns_edge_custom_connect (nns_edge_custom_connection_h handle);

/**
 * @brief Internal function to check custom connection.
 */
int nns_edge_custom_is_connected (nns_edge_custom_connection_h handle);

/**
 * @brief Internal function to send data to custom connection.
 */
int nns_edge_custom_send_data (nns_edge_custom_connection_h handle, nns_edge_data_h data_h);

/**
 * @brief Internal function to set information to custom connection.
 */
int nns_edge_custom_set_info (nns_edge_custom_connection_h handle, const char *key, const char *value);

/**
 * @brief Internal function to get information from custom connection.
 */
int nns_edge_custom_get_info (nns_edge_custom_connection_h handle, const char *key, char **value);
#else
#define nns_edge_custom_load(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_release(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_start(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_stop(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_discover(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_set_event_callback(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_connect(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_is_connected(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_send_data(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_set_info(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_custom_get_info(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#endif /* ENABLE_CUSTOM_CONNECTION */

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_CUSTOM_IMPL_H__ */
