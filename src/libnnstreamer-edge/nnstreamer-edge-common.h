/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-common.h
 * @date   6 April 2022
 * @brief  Common util functions for nnstreamer edge.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 * @note   This file is internal header for nnstreamer edge utils. DO NOT export this file.
 * @todo   Rename this to nnstreamer-edge-meta after separating util functions.
 */

#ifndef __NNSTREAMER_EDGE_COMMON_H__
#define __NNSTREAMER_EDGE_COMMON_H__

#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_broker_h;

/**
 * @brief Internal data structure for metadata.
 */
typedef struct _nns_edge_metadata_node_s nns_edge_metadata_node_s;

/**
 * @brief Internal data structure for metadata.
 */
struct _nns_edge_metadata_node_s {
  char *key;
  char *value;
  nns_edge_metadata_node_s *next;
};

/**
 * @brief Internal data structure to handle metadata. This struct should be managed in the handle.
 */
typedef struct {
  unsigned int list_len;
  nns_edge_metadata_node_s *list;
} nns_edge_metadata_s;

/**
 * @brief Internal data structure for raw data.
 */
typedef struct {
  void *data;
  size_t data_len;
  nns_edge_data_destroy_cb destroy_cb;
} nns_edge_raw_data_s;

/**
 * @brief Internal data structure for edge data.
 */
typedef struct {
  unsigned int magic;
  pthread_mutex_t lock;
  unsigned int num;
  nns_edge_raw_data_s data[NNS_EDGE_DATA_LIMIT];
  nns_edge_metadata_s metadata;
} nns_edge_data_s;

/**
 * @brief Internal data structure for edge event.
 */
typedef struct {
  unsigned int magic;
  nns_edge_event_e event;
  nns_edge_raw_data_s data;
} nns_edge_event_s;

/**
 * @brief Internal data structure for the header of the serialzied edge data.
 */
typedef struct {
  unsigned int num_mem;
  size_t data_len[NNS_EDGE_DATA_LIMIT];
  size_t meta_len;
} nns_edge_data_header_s;

/**
 * @brief Internal function to initialize metadata structure.
 */
int nns_edge_metadata_init (nns_edge_metadata_s *meta);

/**
 * @brief Internal function to free the list and items in metadata structure.
 */
int nns_edge_metadata_free (nns_edge_metadata_s *meta);

/**
 * @brief Internal function to set the metadata.
 */
int nns_edge_metadata_set (nns_edge_metadata_s *meta, const char *key, const char *value);

/**
 * @brief Internal function to get the metadata in the list. Caller should release the returned value using free().
 */
int nns_edge_metadata_get (nns_edge_metadata_s *meta, const char *key, char **value);

/**
 * @brief Internal function to copy the metadata.
 */
int nns_edge_metadata_copy (nns_edge_metadata_s *dest, nns_edge_metadata_s *src);

/**
 * @brief Internal function to serialize the metadata. Caller should release the returned value using free().
 */
int nns_edge_metadata_serialize (nns_edge_metadata_s *meta, void **data, size_t *data_len);

/**
 * @brief Internal function to deserialize memory into metadata.
 */
int nns_edge_metadata_deserialize (nns_edge_metadata_s *meta, void *data, size_t data_len);

/**
 * @brief Create nnstreamer edge event.
 * @note This is internal function for edge event.
 */
int nns_edge_event_create (nns_edge_event_e event, nns_edge_event_h * event_h);

/**
 * @brief Destroy nnstreamer edge event.
 * @note This is internal function for edge event.
 */
int nns_edge_event_destroy (nns_edge_event_h event_h);

/**
 * @brief Set event data.
 * @note This is internal function for edge event.
 */
int nns_edge_event_set_data (nns_edge_event_h event_h, void *data, size_t data_len, nns_edge_data_destroy_cb destroy_cb);

/**
 * @brief Validate edge data handle.
 * @note This is internal function, DO NOT export this.
 */
int nns_edge_data_is_valid (nns_edge_data_h data_h);

/**
 * @brief Serialize metadata in edge data.
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int nns_edge_data_serialize_meta (nns_edge_data_h data_h, void **data, size_t *data_len);

/**
 * @brief Deserialize metadata in edge data.
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int nns_edge_data_deserialize_meta (nns_edge_data_h data_h, void *data, size_t data_len);

/**
 * @brief Serialize entire edge data (meta data + raw data).
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int
nns_edge_data_serialize (nns_edge_data_h data_h, void **data, size_t *data_len);

/**
 * @brief Deserialize entire edge data (meta data + raw data).
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int
nns_edge_data_deserialize (nns_edge_data_h data_h, void *data);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_COMMON_H__ */
