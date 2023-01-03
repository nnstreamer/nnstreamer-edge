/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-metadata.c
 * @date   6 April 2022
 * @brief  Util functions for metadata.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#include "nnstreamer-edge-metadata.h"
#include "nnstreamer-edge-util.h"

/**
 * @brief Internal data structure for metadata.
 */
typedef struct _nns_edge_metadata_node_s nns_edge_metadata_node_s;

/**
 * @brief Internal data structure for metadata.
 */
struct _nns_edge_metadata_node_s
{
  char *key;
  char *value;
  nns_edge_metadata_node_s *next;
};

/**
 * @brief Internal data structure to handle metadata. This struct should be managed in the handle.
 */
typedef struct
{
  uint32_t list_len;
  nns_edge_metadata_node_s *list;
} nns_edge_metadata_s;

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
static int
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
static int
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
 * @brief Internal function to create metadata.
 */
int
nns_edge_metadata_create (nns_edge_metadata_h * metadata_h)
{
  nns_edge_metadata_s *meta;

  if (!metadata_h)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  meta = calloc (1, sizeof (nns_edge_metadata_s));
  if (!meta)
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;

  nns_edge_metadata_init (meta);

  *metadata_h = meta;
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to destroy metadata.
 */
int
nns_edge_metadata_destroy (nns_edge_metadata_h metadata_h)
{
  nns_edge_metadata_s *meta;

  meta = (nns_edge_metadata_s *) metadata_h;

  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  nns_edge_metadata_free (meta);
  SAFE_FREE (meta);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to set the metadata.
 */
int
nns_edge_metadata_set (nns_edge_metadata_h metadata_h,
    const char *key, const char *value)
{
  nns_edge_metadata_s *meta;
  nns_edge_metadata_node_s *node;

  meta = (nns_edge_metadata_s *) metadata_h;

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
nns_edge_metadata_get (nns_edge_metadata_h metadata_h,
    const char *key, char **value)
{
  nns_edge_metadata_s *meta;
  nns_edge_metadata_node_s *node;

  meta = (nns_edge_metadata_s *) metadata_h;

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
nns_edge_metadata_copy (nns_edge_metadata_h dest_h, nns_edge_metadata_h src_h)
{
  nns_edge_metadata_s *dest, *src;
  nns_edge_metadata_s tmp;
  nns_edge_metadata_node_s *node;
  int ret;

  dest = (nns_edge_metadata_s *) dest_h;
  src = (nns_edge_metadata_s *) src_h;

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
nns_edge_metadata_serialize (nns_edge_metadata_h metadata_h,
    void **data, nns_size_t * data_len)
{
  nns_edge_metadata_s *meta;
  nns_edge_metadata_node_s *node;
  char *serialized, *ptr;
  nns_size_t total, len;

  meta = (nns_edge_metadata_s *) metadata_h;

  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  if (!data || !data_len)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  *data = NULL;
  *data_len = 0U;

  if (meta->list_len == 0)
    return NNS_EDGE_ERROR_NONE;

  /* length, # of metadata */
  total = len = sizeof (uint32_t);

  node = meta->list;
  while (node) {
    total += (strlen (node->key) + strlen (node->value) + 2);
    node = node->next;
  }

  serialized = ptr = (char *) nns_edge_malloc (total);
  if (!serialized)
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;

  /* length + list of key-value pair */
  ((uint32_t *) serialized)[0] = meta->list_len;
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
nns_edge_metadata_deserialize (nns_edge_metadata_h metadata_h,
    const void *data, const nns_size_t data_len)
{
  nns_edge_metadata_s *meta;
  char *key, *value;
  nns_size_t cur, total;
  int ret;

  meta = (nns_edge_metadata_s *) metadata_h;

  if (!meta)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  if (!data || data_len <= 0)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  nns_edge_metadata_free (meta);

  /* length + list of key-value pair */
  total = ((uint32_t *) data)[0];

  cur = sizeof (uint32_t);
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
