/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-aitt.h
 * @date   28 Mar 2022
 * @brief  Common library to support communication among devices using aitt.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Sangjung Woo <sangjung.woo@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#ifndef __NNSTREAMER_EDGE_AITT_H__
#define __NNSTREAMER_EDGE_AITT_H__

#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_aitt_h;

#if defined(ENABLE_AITT)
/**
 * @brief Connect to AITT.
 * @note This is internal function for AITT.
 */
int nns_edge_aitt_connect (nns_edge_aitt_h handle, const char *id, const char *topic, const char *host, const int port);

/**
 * @brief Release the AITT handle.
 * @note This is internal function for AITT.
 */
int nns_edge_aitt_close (nns_edge_aitt_h handle);

/**
 * @brief Publish raw data.
 * @note This is internal function for AITT.
 */
int nns_edge_aitt_publish (nns_edge_aitt_h handle, const void *data, const int length);

/**
 * @brief Subscribe a topic.
 * @note This is internal function for AITT.
 */
int nns_edge_aitt_subscribe (nns_edge_aitt_h handle);

/**
 * @brief Set event callback for new message.
 */
int nns_edge_aitt_set_event_callback (nns_edge_aitt_h handle, nns_edge_event_cb cb, void *user_data);

/**
 * @brief Check whether aitt handle exists or not.
 */
int nns_edge_aitt_is_connected (nns_edge_aitt_h handle);

/**
 * @brief Internal util function to send edge-data.
 */
int nns_edge_aitt_send_data (nns_edge_aitt_h handle, nns_edge_data_h data_h);

/**
 * @brief Internal util function to set AITT option.
 */
int nns_edge_aitt_set_option (nns_edge_aitt_h handle, const char *key, const char *value);

/**
 * @brief Internal util function to get AITT option.
 */
const char *nns_edge_aitt_get_option (nns_edge_aitt_h handle, const char *key);

/**
 * @brief Create AITT handle.
 */
int nns_edge_aitt_create (nns_edge_aitt_h *handle);

#else
#define nns_edge_aitt_connect(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_close(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_publish(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_subscribe(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_set_event_callback(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_is_connected(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_send_data(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_set_option(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_get_option(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_create(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#endif /* ENABLE_AITT */

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_AITT_H__ */
