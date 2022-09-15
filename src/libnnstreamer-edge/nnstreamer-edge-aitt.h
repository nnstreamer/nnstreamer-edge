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

#if defined(ENABLE_AITT)
/**
 * @brief Create AITT handle and connect to AITT.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_connect (nns_edge_h edge_h);

/**
 * @brief Release the AITT handle.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_close (nns_edge_h edge_h);

/**
 * @brief Check whether aitt handle exists or not.
 */
int nns_edge_aitt_is_connected (nns_edge_h edge_h);

/**
 * @brief Publish raw data.
 * @note This is internal function forAITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_publish (nns_edge_h edge_h, const void *data, const int length);

/**
 * @brief Subscribe a topic.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_subscribe (nns_edge_h edge_h);
#else
#define nns_edge_aitt_connect(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_close(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_publish(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_subscribe(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_is_connected(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#endif /* ENABLE_AITT */

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_AITT_H__ */
