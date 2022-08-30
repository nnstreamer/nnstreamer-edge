/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-common.c
 * @date   6 April 2022
 * @brief  Common util functions for nnstreamer edge.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#include "nnstreamer-edge-common.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"

/**
 * @brief Internal function to find node in the list.
 */
static nns_edge_metadata_node_s *
nns_edge_metadata_find (nns_edge_metadata_s * meta, const char *key)
{
  nns_edge_metadata_node_s *node;

  if (!meta)
    return NULL;

  if (!STR_IS_VALID (key))
    return NULL;

  node = meta->list;
  while (node) {
    if (strcasecmp (key, node->key) == 0)
      return node;

    node = node->next;
  }

  return NULL;
}

/**
 * @brief Internal function to initialize metadata structure.
 */
int
nns_edge_metadata_init (nns_edge_metadata_s * meta)
{
  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  memset (meta, 0, sizeof (nns_edge_metadata_s));
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to free the list and items in metadata structure.
 */
int
nns_edge_metadata_free (nns_edge_metadata_s * meta)
{
  nns_edge_metadata_node_s *node, *tmp;

  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  node = meta->list;
  meta->list_len = 0;
  meta->list = NULL;

  while (node) {
    tmp = node->next;

    SAFE_FREE (node->key);
    SAFE_FREE (node->value);
    SAFE_FREE (node);

    node = tmp;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to set the metadata.
 */
int
nns_edge_metadata_set (nns_edge_metadata_s * meta,
    const char *key, const char *value)
{
  nns_edge_metadata_node_s *node;

  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  if (!STR_IS_VALID (key))
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  if (!STR_IS_VALID (value))
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  /* Replace old value if key exists in the list. */
  node = nns_edge_metadata_find (meta, key);
  if (node) {
    char *val = nns_edge_strdup (value);

    if (!val)
      return NNS_EDGE_ERROR_OUT_OF_MEMORY;

    SAFE_FREE (node->value);
    node->value = val;
    return NNS_EDGE_ERROR_NONE;
  }

  node = calloc (1, sizeof (nns_edge_metadata_node_s));
  if (!node)
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;

  node->key = nns_edge_strdup (key);
  node->value = nns_edge_strdup (value);

  if (!node->key || !node->value) {
    SAFE_FREE (node->key);
    SAFE_FREE (node->value);
    SAFE_FREE (node);
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  /* Prepend new node to the start of the list. */
  meta->list_len++;
  node->next = meta->list;
  meta->list = node;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to get the metadata in the list. Caller should release the returned value using free().
 */
int
nns_edge_metadata_get (nns_edge_metadata_s * meta,
    const char *key, char **value)
{
  nns_edge_metadata_node_s *node;

  if (!value)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  node = nns_edge_metadata_find (meta, key);
  if (node) {
    *value = nns_edge_strdup (node->value);
    return (*value) ? NNS_EDGE_ERROR_NONE : NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  return NNS_EDGE_ERROR_INVALID_PARAMETER;
}

/**
 * @brief Internal function to copy the metadata.
 */
int
nns_edge_metadata_copy (nns_edge_metadata_s * dest, nns_edge_metadata_s * src)
{
  nns_edge_metadata_s tmp;
  nns_edge_metadata_node_s *node;
  int ret;

  if (!dest || !src)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  nns_edge_metadata_init (&tmp);

  node = src->list;
  while (node) {
    ret = nns_edge_metadata_set (&tmp, node->key, node->value);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_metadata_free (&tmp);
      return ret;
    }

    node = node->next;
  }

  /* Replace dest when new metadata is successfully copied. */
  nns_edge_metadata_free (dest);
  *dest = tmp;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to serialize the metadata. Caller should release the returned value using free().
 */
int
nns_edge_metadata_serialize (nns_edge_metadata_s * meta,
    void **data, size_t *data_len)
{
  nns_edge_metadata_node_s *node;
  char *serialized, *ptr;
  size_t total, len;

  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  if (!data || !data_len)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  *data = NULL;
  *data_len = 0U;

  if (meta->list_len == 0)
    return NNS_EDGE_ERROR_NONE;

  total = len = sizeof (unsigned int);

  node = meta->list;
  while (node) {
    total += (strlen (node->key) + strlen (node->value) + 2);
    node = node->next;
  }

  serialized = ptr = (char *) malloc (total);
  if (!serialized)
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;

  /* length + list of key-value pair */
  ((unsigned int *) serialized)[0] = meta->list_len;
  ptr += len;

  node = meta->list;
  while (node) {
    len = strlen (node->key);
    memcpy (ptr, node->key, len);
    ptr[len] = '\0';
    ptr += (len + 1);

    len = strlen (node->value);
    memcpy (ptr, node->value, len);
    ptr[len] = '\0';
    ptr += (len + 1);

    node = node->next;
  }

  *data = serialized;
  *data_len = total;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to deserialize memory into metadata.
 */
int
nns_edge_metadata_deserialize (nns_edge_metadata_s * meta,
    void *data, size_t data_len)
{
  char *key, *value;
  size_t cur;
  unsigned int total;
  int ret;

  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  if (!data || data_len <= 0)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  nns_edge_metadata_free (meta);

  /* length + list of key-value pair */
  total = ((unsigned int *) data)[0];

  cur = sizeof (unsigned int);
  while (cur < data_len || meta->list_len < total) {
    key = (char *) data + cur;
    cur += (strlen (key) + 1);

    value = (char *) data + cur;
    cur += (strlen (value) + 1);

    ret = nns_edge_metadata_set (meta, key, value);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_metadata_free (meta);
      return ret;
    }
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Create nnstreamer edge event.
 */
int
nns_edge_event_create (nns_edge_event_e event, nns_edge_event_h * event_h)
{
  nns_edge_event_s *ee;

  if (!event_h) {
    nns_edge_loge ("Invalid param, event_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (event <= NNS_EDGE_EVENT_UNKNOWN) {
    nns_edge_loge ("Invalid param, given event type is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ee = (nns_edge_event_s *) calloc (1, sizeof (nns_edge_event_s));
  if (!ee) {
    nns_edge_loge ("Failed to allocate memory for edge event.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  ee->magic = NNS_EDGE_MAGIC;
  ee->event = event;

  *event_h = ee;
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Destroy nnstreamer edge event.
 */
int
nns_edge_event_destroy (nns_edge_event_h event_h)
{
  nns_edge_event_s *ee;

  ee = (nns_edge_event_s *) event_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (ee)) {
    nns_edge_loge ("Invalid param, given edge event is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ee->magic = NNS_EDGE_MAGIC_DEAD;

  if (ee->data.destroy_cb)
    ee->data.destroy_cb (ee->data.data);

  SAFE_FREE (ee);
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Set event data.
 */
int
nns_edge_event_set_data (nns_edge_event_h event_h, void *data, size_t data_len,
    nns_edge_data_destroy_cb destroy_cb)
{
  nns_edge_event_s *ee;

  ee = (nns_edge_event_s *) event_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (ee)) {
    nns_edge_loge ("Invalid param, given edge event is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data || data_len <= 0) {
    nns_edge_loge ("Invalid param, data should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /* Clear old data and set new one. */
  if (ee->data.destroy_cb)
    ee->data.destroy_cb (ee->data.data);

  ee->data.data = data;
  ee->data.data_len = data_len;
  ee->data.destroy_cb = destroy_cb;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Get the nnstreamer edge event type.
 */
int
nns_edge_event_get_type (nns_edge_event_h event_h, nns_edge_event_e * event)
{
  nns_edge_event_s *ee;

  ee = (nns_edge_event_s *) event_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (ee)) {
    nns_edge_loge ("Invalid param, given edge event is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!event) {
    nns_edge_loge ("Invalid param, event should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  *event = ee->event;
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Parse edge event (NNS_EDGE_EVENT_NEW_DATA_RECEIVED) and get received data.
 * @note Caller should release returned edge data using nns_edge_data_destroy().
 */
int
nns_edge_event_parse_new_data (nns_edge_event_h event_h,
    nns_edge_data_h * data_h)
{
  nns_edge_event_s *ee;

  ee = (nns_edge_event_s *) event_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (ee)) {
    nns_edge_loge ("Invalid param, given edge event is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data_h) {
    nns_edge_loge ("Invalid param, data_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (ee->event != NNS_EDGE_EVENT_NEW_DATA_RECEIVED) {
    nns_edge_loge ("The edge event has invalid event type.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  return nns_edge_data_copy ((nns_edge_data_h) ee->data.data, data_h);
}

/**
 * @brief Parse edge event (NNS_EDGE_EVENT_CAPABILITY) and get capability string.
 * @note Caller should release returned string using free().
 */
int
nns_edge_event_parse_capability (nns_edge_event_h event_h, char **capability)
{
  nns_edge_event_s *ee;

  ee = (nns_edge_event_s *) event_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (ee)) {
    nns_edge_loge ("Invalid param, given edge event is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!capability) {
    nns_edge_loge ("Invalid param, capability should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (ee->event != NNS_EDGE_EVENT_CAPABILITY) {
    nns_edge_loge ("The edge event has invalid event type.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  *capability = nns_edge_strdup (ee->data.data);

  return NNS_EDGE_ERROR_NONE;
}

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
  ed->magic = NNS_EDGE_MAGIC;
  nns_edge_metadata_init (&ed->metadata);

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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ed->magic = NNS_EDGE_MAGIC_DEAD;

  for (i = 0; i < ed->num; i++) {
    if (ed->data[i].destroy_cb)
      ed->data[i].destroy_cb (ed->data[i].data);
  }

  nns_edge_metadata_free (&ed->metadata);

  nns_edge_unlock (ed);
  nns_edge_lock_destroy (ed);
  SAFE_FREE (ed);
  return NNS_EDGE_ERROR_NONE;
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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, edge data handle is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_unlock (ed);
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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, edge data handle is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

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

  ret = nns_edge_metadata_copy (&copied->metadata, &ed->metadata);

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
nns_edge_data_add (nns_edge_data_h data_h, void *data, size_t data_len,
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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

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
 * @brief Get the n'th edge data.
 * @note DO NOT release returned data. You should copy the data to another buffer if the returned data is necessary.
 */
int
nns_edge_data_get (nns_edge_data_h data_h, unsigned int index, void **data,
    size_t *data_len)
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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = nns_edge_metadata_set (&ed->metadata, key, value);

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

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = nns_edge_metadata_get (&ed->metadata, key, value);

  nns_edge_unlock (ed);
  return ret;
}

/**
 * @brief Serialize metadata in edge data.
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int
nns_edge_data_serialize_meta (nns_edge_data_h data_h, void **data,
    size_t *data_len)
{
  nns_edge_data_s *ed;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = nns_edge_metadata_serialize (&ed->metadata, data, data_len);

  nns_edge_unlock (ed);
  return ret;
}

/**
 * @brief Deserialize metadata in edge data.
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int
nns_edge_data_deserialize_meta (nns_edge_data_h data_h, void *data,
    size_t data_len)
{
  nns_edge_data_s *ed;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed) {
    nns_edge_loge ("Invalid param, given edge data handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = nns_edge_metadata_deserialize (&ed->metadata, data, data_len);

  nns_edge_unlock (ed);
  return ret;
}

/**
 * @brief Serialize edge data (meta data + raw data).
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int
nns_edge_data_serialize (nns_edge_data_h data_h, void **data, size_t *len)
{
  nns_edge_data_s *ed;
  nns_edge_data_header_s edata_header;
  void *meta_serialized = NULL;
  size_t total, header_len, data_len;
  char *serialized, *ptr;
  unsigned int n;
  int ret;

  ed = (nns_edge_data_s *) data_h;
  if (!ed || !data || !len) {
    nns_edge_loge ("Invalid param, one of the given param is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  header_len = sizeof (nns_edge_data_header_s);

  data_len = 0;
  edata_header.num_mem = ed->num;
  for (n = 0; n < ed->num; n++) {
    edata_header.data_len[n] = ed->data[n].data_len;
    data_len += ed->data[n].data_len;
  }

  ret =
      nns_edge_metadata_serialize (&ed->metadata, &meta_serialized,
      &edata_header.meta_len);
  if (NNS_EDGE_ERROR_NONE != ret) {
    goto done;
  }

  total = header_len + data_len + edata_header.meta_len;

  serialized = ptr = (char *) malloc (total);
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
  nns_edge_free (meta_serialized);
  nns_edge_unlock (ed);
  return ret;
}

/**
 * @brief Deserialize metadata in edge data.
 * @note This is internal function, DO NOT export this. Caller should release the returned value using free().
 */
int
nns_edge_data_deserialize (nns_edge_data_h data_h, void *data)
{
  nns_edge_data_s *ed;
  nns_edge_data_header_s *header;
  int ret;
  unsigned int n;
  size_t meta_len;
  char *ptr;

  ed = (nns_edge_data_s *) data_h;
  if (!ed || !data) {
    nns_edge_loge ("Invalid param, one of the given param is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (ed);

  if (!NNS_EDGE_MAGIC_IS_VALID (ed)) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    nns_edge_unlock (ed);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ptr = (char *) data;
  header = (nns_edge_data_header_s *) ptr;

  ed->num = header->num_mem;
  for (n = 0; n < ed->num; n++) {
    ed->data[n].data_len = header->data_len[n];
  }
  meta_len = header->meta_len;

  ptr += sizeof (nns_edge_data_header_s);

  for (n = 0; n < ed->num; n++) {
    ed->data[n].data = nns_edge_memdup (ptr, ed->data[n].data_len);
    ptr += ed->data[n].data_len;
  }

  ret = nns_edge_metadata_deserialize (&ed->metadata, ptr, meta_len);

  nns_edge_unlock (ed);
  return ret;
}
