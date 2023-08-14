/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-data.c
 * @date   7 Sep 2022
 * @brief  Util functions for edge data.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"

#define NNS_EDGE_DATA_KEY (0xeddaedda)

/**
 * @brief Internal data structure for the header of the serialzied edge data.
 */
typedef struct
{
  uint32_t key;
  uint32_t num_mem;
  nns_size_t data_len[NNS_EDGE_DATA_LIMIT];
  nns_size_t meta_len;
} nns_edge_data_header_s;

/**
 * @brief Internal data structure for edge data.
 */
typedef struct
{
  uint32_t magic;
  pthread_mutex_t lock;
  uint32_t num;
  nns_edge_raw_data_s data[NNS_EDGE_DATA_LIMIT];
  nns_edge_metadata_h metadata;
} nns_edge_data_s;

/**
 * @brief Create nnstreamer edge data.
 */
int
nns_edge_data_create (nns_edge_data_h * data_h)
{
  nns_edge_data_s *ed;

  if (!data_h) {
    nns_edge_loge ("Invalid param, data_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ed = (nns_edge_data_s *) calloc (1, sizeof (nns_edge_data_s));
  if (!ed) {
    nns_edge_loge ("Failed to allocate memory for edge data.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  nns_edge_lock_init (ed);
  nns_edge_handle_set_magic (ed, NNS_EDGE_MAGIC);
  nns_edge_metadata_create (&ed->metadata);

  *data_h = ed;
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Destroy nnstreamer edge data.
 */
int
nns_edge_data_destroy (nns_edge_data_h data_h)
{
  nns_edge_data_s *ed;
  unsigned int i;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  nns_edge_handle_set_magic (ed, NNS_EDGE_MAGIC_DEAD);

  for (i = 0; i < ed->num; i++) {
    if (ed->data[i].destroy_cb)
      ed->data[i].destroy_cb (ed->data[i].data);
  }

  nns_edge_metadata_destroy (ed->metadata);

  nns_edge_unlock (ed);
  nns_edge_lock_destroy (ed);
  SAFE_FREE (ed);
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal wrapper function of the nns_edge_data_destory() to avoid build warning of the incompatibe type casting. (See nns_edge_data_destroy_cb())
 */
void
nns_edge_data_release_handle (void *data)
{
  nns_edge_data_h data_h = (nns_edge_data_h) data;

  if (data_h) {
    if (NNS_EDGE_ERROR_NONE != nns_edge_data_destroy (data_h))
      nns_edge_logw ("Failed to destory the nns-edge data handle.");
  }
}

/**
 * @brief Validate edge data handle.
 */
int
nns_edge_data_is_valid (nns_edge_data_h data_h)
{
  nns_edge_data_s *ed;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, edge data handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Copy edge data and return new handle.
 */
int
nns_edge_data_copy (nns_edge_data_h data_h, nns_edge_data_h * new_data_h)
{
  nns_edge_data_s *ed;
  nns_edge_data_s *copied;
  unsigned int i;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!new_data_h) {
    nns_edge_loge ("Invalid param, new_data_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, edge data handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  ret = nns_edge_data_create (new_data_h);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create new data handle.");
    nns_edge_unlock (ed);
    return ret;
  }

  copied = (nns_edge_data_s *) (*new_data_h);

  copied->num = ed->num;
  for (i = 0; i < ed->num; i++) {
    copied->data[i].data = nns_edge_memdup (ed->data[i].data,
        ed->data[i].data_len);

    if (!copied->data[i].data) {
      nns_edge_loge ("Failed to copy data, error while allocating new memory.");
      copied->num = i;
      ret = NNS_EDGE_ERROR_OUT_OF_MEMORY;
      goto done;
    }

    copied->data[i].data_len = ed->data[i].data_len;
    copied->data[i].destroy_cb = nns_edge_free;
  }

  ret = nns_edge_metadata_copy (copied->metadata, ed->metadata);

done:
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_data_destroy (*new_data_h);
    *new_data_h = NULL;
  }

  nns_edge_unlock (ed);
  return ret;
}

/**
 * @brief Add raw data into nnstreamer edge data.
 */
int
nns_edge_data_add (nns_edge_data_h data_h, void *data, nns_size_t data_len,
    nns_edge_data_destroy_cb destroy_cb)
{
  nns_edge_data_s *ed;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data || data_len <= 0) {
    nns_edge_loge ("Invalid param, data should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  if (ed->num >= NNS_EDGE_DATA_LIMIT) {
    nns_edge_loge ("Cannot add data, the maximum number of edge data is %d.",
        NNS_EDGE_DATA_LIMIT);
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ed->data[ed->num].data = data;
  ed->data[ed->num].data_len = data_len;
  ed->data[ed->num].destroy_cb = destroy_cb;
  ed->num++;

  nns_edge_unlock (ed);
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Remove raw data in edge data.
 */
int
nns_edge_data_clear (nns_edge_data_h data_h)
{
  nns_edge_data_s *ed;
  unsigned int i;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  for (i = 0; i < ed->num; i++) {
    if (ed->data[i].destroy_cb)
      ed->data[i].destroy_cb (ed->data[i].data);
    ed->data[i].data = NULL;
    ed->data[i].data_len = 0;
    ed->data[i].destroy_cb = NULL;
  }
  ed->num = 0;

  nns_edge_unlock (ed);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Get the n'th edge data.
 */
int
nns_edge_data_get (nns_edge_data_h data_h, unsigned int index, void **data,
    nns_size_t * data_len)
{
  nns_edge_data_s *ed;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data || !data_len) {
    nns_edge_loge ("Invalid param, data and len should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  if (index >= ed->num) {
    nns_edge_loge
        ("Invalid param, the number of edge data is %u but requested %uth data.",
        ed->num, index);
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  *data = ed->data[index].data;
  *data_len = ed->data[index].data_len;

  nns_edge_unlock (ed);
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Get the number of edge data in handle.
 */
int
nns_edge_data_get_count (nns_edge_data_h data_h, unsigned int *count)
{
  nns_edge_data_s *ed;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!count) {
    nns_edge_loge ("Invalid param, count should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  *count = ed->num;
  nns_edge_unlock (ed);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Set the information of edge data.
 */
int
nns_edge_data_set_info (nns_edge_data_h data_h, const char *key,
    const char *value)
{
  nns_edge_data_s *ed;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (key)) {
    nns_edge_loge ("Invalid param, given key is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  ret = nns_edge_metadata_set (ed->metadata, key, value);
  nns_edge_unlock (ed);

  return ret;
}

/**
 * @brief Get the information of edge data. Caller should release the returned value using free().
 */
int
nns_edge_data_get_info (nns_edge_data_h data_h, const char *key, char **value)
{
  nns_edge_data_s *ed;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (key)) {
    nns_edge_loge ("Invalid param, given key is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!value) {
    nns_edge_loge ("Invalid param, value should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  ret = nns_edge_metadata_get (ed->metadata, key, value);
  nns_edge_unlock (ed);

  return ret;
}

/**
 * @brief Clear information of edge data.
 */
int
nns_edge_data_clear_info (nns_edge_data_h data_h)
{
  nns_edge_data_s *ed;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  ret = nns_edge_metadata_destroy (ed->metadata);
  if (NNS_EDGE_ERROR_NONE != ret)
    goto done;
  ret = nns_edge_metadata_create (&ed->metadata);
done:
  nns_edge_unlock (ed);

  return ret;
}

/**
 * @brief Serialize metadata in edge data.
 */
int
nns_edge_data_serialize_meta (nns_edge_data_h data_h, void **data,
    nns_size_t * data_len)
{
  nns_edge_data_s *ed;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  ret = nns_edge_metadata_serialize (ed->metadata, data, data_len);
  nns_edge_unlock (ed);

  return ret;
}

/**
 * @brief Deserialize metadata in edge data.
 */
int
nns_edge_data_deserialize_meta (nns_edge_data_h data_h, const void *data,
    const nns_size_t data_len)
{
  nns_edge_data_s *ed;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  ret = nns_edge_metadata_deserialize (ed->metadata, data, data_len);
  nns_edge_unlock (ed);

  return ret;
}

/**
 * @brief Serialize edge data (meta data + raw data).
 */
int
nns_edge_data_serialize (nns_edge_data_h data_h, void **data, nns_size_t * len)
{
  nns_edge_data_s *ed;
  nns_edge_data_header_s edata_header;
  void *meta_serialized = NULL;
  nns_size_t total, header_len, data_len;
  char *serialized, *ptr;
  unsigned int n;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed || !data || !len) {
    nns_edge_loge ("Invalid param, one of the given param is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);
  header_len = sizeof (nns_edge_data_header_s);

  data_len = 0;
  edata_header.key = NNS_EDGE_DATA_KEY;
  edata_header.num_mem = ed->num;
  for (n = 0; n < ed->num; n++) {
    edata_header.data_len[n] = ed->data[n].data_len;
    data_len += ed->data[n].data_len;
  }

  ret = nns_edge_metadata_serialize (ed->metadata, &meta_serialized,
      &edata_header.meta_len);
  if (NNS_EDGE_ERROR_NONE != ret) {
    goto done;
  }

  total = header_len + data_len + edata_header.meta_len;

  serialized = ptr = (char *) nns_edge_malloc (total);
  if (!serialized) {
    ret = NNS_EDGE_ERROR_OUT_OF_MEMORY;
    goto done;
  }

  /** Copy serialization header of edge data */
  memcpy (ptr, &edata_header, header_len);
  ptr += header_len;

  /** Copy edge data */
  for (n = 0; n < ed->num; n++) {
    memcpy (ptr, ed->data[n].data, ed->data[n].data_len);
    ptr += ed->data[n].data_len;
  }

  /** Copy edge meta data */
  memcpy (ptr, meta_serialized, edata_header.meta_len);

  *data = serialized;
  *len = total;

done:
  SAFE_FREE (meta_serialized);
  nns_edge_unlock (ed);
  return ret;
}

/**
 * @brief Deserialize metadata in edge data.
 */
int
nns_edge_data_deserialize (nns_edge_data_h data_h, const void *data,
    const nns_size_t data_len)
{
  nns_edge_data_s *ed;
  nns_edge_data_header_s *header;
  int ret;
  unsigned int n;
  char *ptr;

  ed = (nns_edge_data_s *) data_h;
  if (!ed || !data) {
    nns_edge_loge ("Invalid param, one of the given param is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = nns_edge_data_is_serialized (data, data_len);
  if (ret != NNS_EDGE_ERROR_NONE)
    return ret;

  nns_edge_lock (ed);
  header = (nns_edge_data_header_s *) data;
  ptr = (char *) data + sizeof (nns_edge_data_header_s);

  ed->num = header->num_mem;
  for (n = 0; n < ed->num; n++) {
    ed->data[n].data = nns_edge_memdup (ptr, header->data_len[n]);
    ed->data[n].data_len = header->data_len[n];
    ed->data[n].destroy_cb = nns_edge_free;

    ptr += header->data_len[n];
  }

  ret = nns_edge_metadata_deserialize (ed->metadata, ptr, header->meta_len);

  nns_edge_unlock (ed);
  return ret;
}

/**
 * @brief Check given data is serialized buffer.
 */
int
nns_edge_data_is_serialized (const void *data, const nns_size_t data_len)
{
  nns_edge_data_header_s *header;
  nns_size_t total;
  unsigned int n;

  if (!data) {
    nns_edge_loge ("Invalid param, given data is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  header = (nns_edge_data_header_s *) data;
  if (header->key != NNS_EDGE_DATA_KEY) {
    nns_edge_loge ("Invalid param, given data has invalid format.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /* Check mem size */
  total = sizeof (nns_edge_data_header_s) + header->meta_len;
  for (n = 0; n < header->num_mem; n++)
    total += header->data_len[n];

  if (total != data_len) {
    nns_edge_loge ("Invalid param, given data has invalid data size.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  return NNS_EDGE_ERROR_NONE;
}
