/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2024 Gichan Jang <gichan2.jang@samsung.com>
 *
 * @file   nnstreamer-edge-custom-test.c
 * @date   16 Aug 2024
 * @brief  NNStreamer-edge custom connection for test.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#include "nnstreamer-edge-custom.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge.h"

typedef struct
{
  int is_connected;
  char *peer_address;
  nns_edge_event_cb event_cb;
  void *user_data;
} nns_edge_custom_test_s;

static int
nns_edge_custom_close (void *priv)
{
  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;

  SAFE_FREE (custom_h->peer_address);
  SAFE_FREE (custom_h);

  return NNS_EDGE_ERROR_NONE;
}

static const char *
nns_edge_custom_get_description ()
{
  return "custom";
}

static int
nns_edge_custom_create (void **priv)
{
  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) calloc (1, sizeof (nns_edge_custom_test_s));
  if (!custom_h) {
    nns_edge_loge ("Failed to allocate memory for edge custom handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  *priv = custom_h;

  return NNS_EDGE_ERROR_NONE;
}

static int
nns_edge_custom_start (void *priv)
{
  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;
  custom_h->is_connected = 0;

  return NNS_EDGE_ERROR_NONE;
}

static int
nns_edge_custom_stop (void *priv)
{
  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;
  custom_h->is_connected = 0;

  return NNS_EDGE_ERROR_NONE;
}

static int
nns_edge_custom_connect (void *priv)
{
  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;
  custom_h->is_connected = 1;

  return NNS_EDGE_ERROR_NONE;
}

static int
nns_edge_custom_subscribe (void *priv)
{
  return NNS_EDGE_ERROR_NOT_SUPPORTED;
}


static int
nns_edge_custom_discover (void *priv)
{
  int ret = NNS_EDGE_ERROR_NONE;

  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;
  ret = nns_edge_event_invoke_callback (custom_h->event_cb, custom_h->user_data,
      NNS_EDGE_EVENT_DEVICE_FOUND, NULL, 0, NULL);

  return ret;
}

static int
nns_edge_custom_is_connected (void *priv)
{
  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;

  if (custom_h->is_connected == 1)
    return NNS_EDGE_ERROR_NONE;

  return NNS_EDGE_ERROR_CONNECTION_FAILURE;
}

static int
nns_edge_custom_set_event_cb (void *priv, nns_edge_event_cb cb, void *user_data)
{
  if (!priv) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;

  custom_h->event_cb = cb;
  custom_h->user_data = user_data;

  return NNS_EDGE_ERROR_NONE;
}

static int
nns_edge_custom_send_data (void *priv, nns_edge_data_h data_h)
{
  if (!priv || !data_h) {
    nns_edge_loge ("Invalid param, handle or data should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  return NNS_EDGE_ERROR_NONE;
}

static int
nns_edge_custom_set_info (void *priv, const char *key, const char *value)
{
  if (!priv || !key || !value) {
    nns_edge_loge ("Invalid param, handle, key or value should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;

  if (strcasecmp (key, "PEER_ADDRESS") == 0) {
    SAFE_FREE (custom_h->peer_address);
    custom_h->peer_address = nns_edge_strdup (value);
    return NNS_EDGE_ERROR_NONE;
  }

  nns_edge_loge ("The key '%s' is not supported.", key);
  return NNS_EDGE_ERROR_INVALID_PARAMETER;
}

static int
nns_edge_custom_get_info (void *priv, const char *key, char **value)
{
  if (!priv || !key || !value) {
    nns_edge_loge ("Invalid param, handle, key or value should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }
  nns_edge_custom_test_s *custom_h = (nns_edge_custom_test_s *) priv;

  if (strcasecmp (key, "PEER_ADDRESS") == 0) {
    *value = nns_edge_strdup (custom_h->peer_address);
    return NNS_EDGE_ERROR_NONE;
  }

  nns_edge_loge ("The key '%s' is not supported.", key);
  return NNS_EDGE_ERROR_INVALID_PARAMETER;
}

nns_edge_custom_s edge_custom_h = {
  .nns_edge_custom_get_description = nns_edge_custom_get_description,
  .nns_edge_custom_create = nns_edge_custom_create,
  .nns_edge_custom_close = nns_edge_custom_close,
  .nns_edge_custom_start = nns_edge_custom_start,
  .nns_edge_custom_discover = nns_edge_custom_discover,
  .nns_edge_custom_stop = nns_edge_custom_stop,
  .nns_edge_custom_connect = nns_edge_custom_connect,
  .nns_edge_custom_subscribe = nns_edge_custom_subscribe,
  .nns_edge_custom_is_connected = nns_edge_custom_is_connected,
  .nns_edge_custom_set_event_cb = nns_edge_custom_set_event_cb,
  .nns_edge_custom_send_data = nns_edge_custom_send_data,
  .nns_edge_custom_set_info = nns_edge_custom_set_info,
  .nns_edge_custom_get_info = nns_edge_custom_get_info
};

const nns_edge_custom_s *
nns_edge_custom_get_instance ()
{
  return &edge_custom_h;
}
