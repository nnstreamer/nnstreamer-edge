/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2024 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-mcf.h
 * @date   02 July 2024
 * @brief  Common library to support communication among devices using MCF.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#ifndef __NNSTREAMER_EDGE_MCF_H__
#define __NNSTREAMER_EDGE_MCF_H__

#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_mcf_h;

#if defined(ENABLE_MCF)
#include <mcf.h>

/**
 * @brief Create MCF handle.
 */
int nns_edge_mcf_create (nns_edge_mcf_h *handle);

/**
 * @brief Connect to MCF.
 * @note This is internal function for MCF.
 */
int nns_edge_mcf_connect (nns_edge_mcf_h handle, const char *id, const char *topic, const char *host, const int port);

/**
 * @brief Release the MCF handle.
 * @note This is internal function for MCF.
 */
int nns_edge_mcf_close (nns_edge_mcf_h handle);

/**
 * @brief Start the MCF handle.
 */
int nns_edge_mcf_start (nns_edge_mcf_h handle);

/**
 * @brief Publish raw data.
 * @note This is internal function for MCF.
 */
int nns_edge_mcf_publish (nns_edge_mcf_h handle, const void *data, const int length);

/**
 * @brief Set event callback for new message.
 */
int nns_edge_mcf_set_event_callback (nns_edge_mcf_h handle, nns_edge_event_cb cb, void *user_data);

/**
 * @brief Check whether mcf handle exists or not.
 */
int nns_edge_mcf_is_connected (nns_edge_mcf_h handle);

/**
 * @brief Internal util function to send edge-data.
 */
int nns_edge_mcf_send_data (nns_edge_mcf_h handle, nns_edge_data_h data_h);

/**
 * @brief Internal util function to set MCF option.
 */
int nns_edge_mcf_set_option (nns_edge_mcf_h handle, const char *key, const char *value);

/**
 * @brief Internal util function to get MCF option.
 */
const char *nns_edge_mcf_get_option (nns_edge_mcf_h handle, const char *key);

#else
#define nns_edge_mcf_create(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_connect(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_close(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_publish(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_start(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_set_event_callback(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_is_connected(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_send_data(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_set_option(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mcf_get_option(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#endif /* ENABLE_MCF */

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_MCF_H__ */
