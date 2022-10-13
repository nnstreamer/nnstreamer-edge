/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-metadata.h
 * @date   6 April 2022
 * @brief  Util functions for metadata.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 * @note   This file is internal header to handle metadata. DO NOT export this file.
 */

#ifndef __NNSTREAMER_EDGE_METADATA_H__
#define __NNSTREAMER_EDGE_METADATA_H__

#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_metadata_h;

/**
 * @brief Internal function to create metadata.
 */
int nns_edge_metadata_create (nns_edge_metadata_h *metadata_h);

/**
 * @brief Internal function to destroy metadata.
 */
int nns_edge_metadata_destroy (nns_edge_metadata_h metadata_h);

/**
 * @brief Internal function to set the metadata.
 */
int nns_edge_metadata_set (nns_edge_metadata_h metadata_h, const char *key, const char *value);

/**
 * @brief Internal function to get the metadata in the list. Caller should release the returned value using free().
 */
int nns_edge_metadata_get (nns_edge_metadata_h metadata_h, const char *key, char **value);

/**
 * @brief Internal function to copy the metadata.
 */
int nns_edge_metadata_copy (nns_edge_metadata_h dest_h, nns_edge_metadata_h src_h);

/**
 * @brief Internal function to serialize the metadata. Caller should release the returned value using free().
 */
int nns_edge_metadata_serialize (nns_edge_metadata_h metadata_h, void **data, nns_size_t *data_len);

/**
 * @brief Internal function to deserialize memory into metadata.
 */
int nns_edge_metadata_deserialize (nns_edge_metadata_h metadata_h, void *data, nns_size_t data_len);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_METADATA_H__ */
