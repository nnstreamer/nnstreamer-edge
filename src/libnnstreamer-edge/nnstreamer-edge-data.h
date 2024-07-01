/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-data.h
 * @date   7 Sep 2022
 * @brief  Util functions for edge data.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 * @note   This file is internal header for nnstreamer edge utils. DO NOT export this file.
 */

#ifndef __NNSTREAMER_EDGE_DATA_H__
#define __NNSTREAMER_EDGE_DATA_H__

#include "nnstreamer-edge.h"
#include "nnstreamer-edge-metadata.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Internal wrapper function of the nns_edge_data_destroy() to avoid build warning of the incompatibe type casting. (See nns_edge_data_destroy_cb())
 */
void nns_edge_data_release_handle (void *data);

/**
 * @brief Validate edge data handle.
 * @note This is internal function, DO NOT export this.
 */
int nns_edge_data_is_valid (nns_edge_data_h data_h);

/**
 * @brief Serialize metadata in edge data.
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int nns_edge_data_serialize_meta (nns_edge_data_h data_h, void **data, nns_size_t *data_len);

/**
 * @brief Deserialize metadata in edge data.
 * @note This is internal function, DO NOT export this.
 */
int nns_edge_data_deserialize_meta (nns_edge_data_h data_h, const void *data, const nns_size_t data_len);

/**
 * @brief Serialize entire edge data (meta data + raw data).
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int nns_edge_data_serialize (nns_edge_data_h data_h, void **data, nns_size_t *data_len);

/**
 * @brief Deserialize entire edge data (meta data + raw data).
 * @note This is internal function, DO NOT export this.
 */
int nns_edge_data_deserialize (nns_edge_data_h data_h, const void *data, const nns_size_t data_len);

/**
 * @brief Check given data is serialized buffer.
 * @note This is internal function, DO NOT export this.
 */
int nns_edge_data_is_serialized (const void *data, const nns_size_t data_len);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_DATA_H__ */
