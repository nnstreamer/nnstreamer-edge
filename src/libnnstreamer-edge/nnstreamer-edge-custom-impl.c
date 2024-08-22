/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2024 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-custom-impl.c
 * @date   14 Aug 2024
 * @brief  Internal interface to support communication using custom library.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#include <dlfcn.h>

#include "nnstreamer-edge-custom-impl.h"
#include "nnstreamer-edge-log.h"

typedef const nns_edge_custom_s *custom_get_instance (void);

/**
 * @brief Internal function to load custom library.
 */
static int
_load_custom_library (custom_connection_s * custom, const char *lib_path)
{
  void *handle;
  nns_edge_custom_s *custom_h;
  int ret = NNS_EDGE_ERROR_UNKNOWN;

  handle = dlopen (lib_path, RTLD_LAZY);
  if (NULL == handle) {
    nns_edge_loge ("Failed to open custom library: %s", dlerror ());
    goto error;
  }

  custom_get_instance *get_instance =
      (custom_get_instance *) dlsym (handle, "nns_edge_custom_get_instance");
  if (!get_instance) {
    nns_edge_loge ("Failed to find nns_edge_custom_get_instance: %s",
        dlerror ());
    goto error;
  }

  custom_h = (nns_edge_custom_s *) get_instance ();
  if (!custom_h) {
    nns_edge_loge ("Failed to get custom instance from library.");
    goto error;
  }

  custom->dl_handle = handle;
  custom->instance = custom_h;
  ret = NNS_EDGE_ERROR_NONE;

error:
  if (NNS_EDGE_ERROR_NONE != ret) {
    if (handle)
      dlclose (handle);
  }

  return ret;
}

/**
 * @brief Internal function to load custom connection from library.
 */
int
nns_edge_custom_load (custom_connection_s * custom, const char *lib_path)
{
  nns_edge_custom_s *custom_h;
  int ret;

  ret = _load_custom_library (custom, lib_path);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge
        ("Failed to load custom library. Please check the library path or permission.");
    goto error;
  }

  custom_h = custom->instance;

  ret = custom_h->nns_edge_custom_create (&custom->priv);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to create custom connection handle.");
  }

error:
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_custom_release (custom);
  }

  return ret;
}

/**
 * @brief Internal function to release custom connection.
 */
int
nns_edge_custom_release (custom_connection_s * custom)
{
  nns_edge_custom_s *custom_h;
  int ret;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  ret = custom_h->nns_edge_custom_close (custom->priv);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to stop custom connection.");
  }

  if (custom->dl_handle) {
    dlclose (custom->dl_handle);
  }

  custom->dl_handle = NULL;
  custom->instance = NULL;
  custom->priv = NULL;

  return ret;
}

/**
 * @brief Internal function to start custom connection.
 */
int
nns_edge_custom_start (custom_connection_s * custom)
{
  nns_edge_custom_s *custom_h;
  int ret;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  ret = custom_h->nns_edge_custom_start (custom->priv);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to start custom connection.");
  }

  return ret;
}

/**
 * @brief Internal function to stop custom connection.
 */
int
nns_edge_custom_stop (custom_connection_s * custom)
{
  nns_edge_custom_s *custom_h;
  int ret;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  ret = custom_h->nns_edge_custom_stop (custom->priv);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to stop custom connection.");
  }

  return ret;
}

/**
 * @brief Internal function to set the event callback of custom connection.
 */
int
nns_edge_custom_set_event_callback (custom_connection_s * custom,
    nns_edge_event_cb cb, void *user_data)
{
  nns_edge_custom_s *custom_h;
  int ret;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  ret = custom_h->nns_edge_custom_set_event_cb (custom->priv, cb, user_data);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to set event callback to custom connection.");
  }

  return ret;
}

/**
 * @brief Internal function to connect custom connection.
 */
int
nns_edge_custom_connect (custom_connection_s * custom)
{
  nns_edge_custom_s *custom_h;
  int ret;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  ret = custom_h->nns_edge_custom_connect (custom->priv);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to connect custom connection.");
  }

  return ret;
}

/**
 * @brief Internal function to check custom connection.
 */
int
nns_edge_custom_is_connected (custom_connection_s * custom)
{
  nns_edge_custom_s *custom_h;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  return custom_h->nns_edge_custom_is_connected (custom->priv);
}

/**
 * @brief Internal function to send data to custom connection.
 */
int
nns_edge_custom_send_data (custom_connection_s * custom, nns_edge_data_h data_h)
{
  nns_edge_custom_s *custom_h;
  int ret;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  ret = custom_h->nns_edge_custom_send_data (custom->priv, data_h);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to send data to custom connection.");
  }

  return ret;
}

/**
 * @brief Internal function to set information to custom connection.
 */
int
nns_edge_custom_set_info (custom_connection_s * custom, const char *key,
    const char *value)
{
  nns_edge_custom_s *custom_h;
  int ret = NNS_EDGE_ERROR_NOT_SUPPORTED;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  if (custom_h->nns_edge_custom_set_info) {
    ret = custom_h->nns_edge_custom_set_info (custom->priv, key, value);
    if (NNS_EDGE_ERROR_NONE != ret) {
      nns_edge_loge ("Failed to set information to custom connection.");
    }
  }

  return ret;
}

/**
 * @brief Internal function to get information from custom connection.
 */
int
nns_edge_custom_get_info (custom_connection_s * custom, const char *key,
    char **value)
{
  nns_edge_custom_s *custom_h;
  int ret = NNS_EDGE_ERROR_NOT_SUPPORTED;

  if (!custom || !custom->instance)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  custom_h = custom->instance;

  if (custom_h->nns_edge_custom_get_info) {
    ret = custom_h->nns_edge_custom_get_info (custom->priv, key, value);
    if (NNS_EDGE_ERROR_NONE != ret) {
      nns_edge_loge ("Failed to get information from custom connection.");
    }
  }

  return ret;
}
