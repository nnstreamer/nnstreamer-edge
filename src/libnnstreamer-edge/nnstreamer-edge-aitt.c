/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Gichan Jang <gichan2.jang@samsung.com>
 *
 * @file   nnstreamer-edge-aitt.c
 * @date   28 Mar 2022
 * @brief  Common library to support communication among devices using aitt.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */


#include <stdbool.h>
#include <aitt_c.h>

#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-common.h"
#include "nnstreamer-edge-internal.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge-log.h"

typedef void *nns_edge_aitt_h;
typedef void *nns_edge_aitt_msg_h;
typedef void *nns_edge_aitt_sub_h;

/**
 * @brief Data structure for aitt handle.
 */
typedef struct
{
  nns_edge_aitt_h aitt_h;
  nns_edge_aitt_msg_h msg_h;
  nns_edge_aitt_sub_h sub_h;
} nns_edge_aitt_handle_s;

/**
 * @brief Create AITT handle and connect to AITT.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int
nns_edge_aitt_connect (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  nns_edge_aitt_handle_s *ah;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_logd ("Create AITT instance: My address: %s:%d", eh->host, eh->port);

  ah = (nns_edge_aitt_handle_s *) calloc (1, sizeof (nns_edge_aitt_handle_s));
  if (!ah) {
    nns_edge_loge ("Failed to allocate memory for AITT handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  ah->aitt_h = aitt_new (eh->id, eh->host);
  if (!ah->aitt_h) {
    nns_edge_loge ("Failed to create AITT handle. AITT internal error.");
    SAFE_FREE (ah);
    return NNS_EDGE_ERROR_UNKNOWN;
  }

  if (AITT_ERROR_NONE != aitt_connect (ah->aitt_h, eh->dest_host,
          eh->dest_port)) {
    nns_edge_loge ("Failed to connect to AITT. IP:port = %s:%d", eh->dest_host,
        eh->dest_port);
    aitt_destroy (ah->aitt_h);
    SAFE_FREE (ah);
    return NNS_EDGE_ERROR_UNKNOWN;
  }
  eh->broker_h = ah;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Release the AITT handle.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int
nns_edge_aitt_close (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  nns_edge_aitt_handle_s *ah;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) eh->broker_h;
  if (AITT_ERROR_NONE != aitt_disconnect (ah->aitt_h)) {
    nns_edge_loge ("Failed to close AITT handle.");
    return NNS_EDGE_ERROR_UNKNOWN;
  }
  aitt_destroy (ah->aitt_h);
  ah->aitt_h = NULL;
  SAFE_FREE (eh->broker_h);
  eh->broker_h = NULL;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Check whether aitt handle exists or not.
 */
int
nns_edge_aitt_is_connected (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  nns_edge_aitt_handle_s *ah;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) eh->broker_h;
  if (!ah || !ah->aitt_h) {
    nns_edge_loge ("AITT handle is not yet connected.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Publish raw data.
 * @note This is internal function forAITT. You should call this with edge-handle lock.
 */
int
nns_edge_aitt_publish (nns_edge_h edge_h, const void *data, const int length)
{
  nns_edge_handle_s *eh;
  nns_edge_aitt_handle_s *ah;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data || length <= 0) {
    nns_edge_loge ("Invalid param, given data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) eh->broker_h;

  if (AITT_ERROR_NONE != aitt_publish (ah->aitt_h, eh->topic, data, length)) {
    nns_edge_loge ("Failed to publish the message. topic: %s", eh->topic);
    return NNS_EDGE_ERROR_IO;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to invoke event callback.
 * @note This function should be called with handle lock.
 */
static int
_nns_edge_invoke_event_cb (nns_edge_handle_s * eh, nns_edge_event_e event,
    void *data, size_t data_len, nns_edge_data_destroy_cb destroy_cb)
{
  nns_edge_event_h event_h;
  int ret;

  /* If event callback is null, return ok. */
  if (!eh->event_cb) {
    nns_edge_logw ("AITT: The event callback is null, do nothing!");
    return NNS_EDGE_ERROR_NONE;
  }

  ret = nns_edge_event_create (event, &event_h);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create new edge event.");
    return ret;
  }

  if (data) {
    ret = nns_edge_event_set_data (event_h, data, data_len, destroy_cb);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to handle edge event due to invalid event data.");
      goto error;
    }
  }

  ret = eh->event_cb (event_h, eh->user_data);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("The event callback returns error.");
  }

error:
  nns_edge_event_destroy (event_h);
  return ret;
}

/**
 * @brief Callback function to be called when a message is arrived.
 */
static void
aitt_cb_message_arrived (aitt_msg_h msg_handle, const void *msg,
    size_t msg_len, void *user_data)
{
  nns_edge_handle_s *eh;
  nns_edge_data_h data_h;

  eh = (nns_edge_handle_s *) user_data;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return;
  }

  if (nns_edge_data_create (&data_h) != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create data handle in msg thread.");
    return;
  }

  nns_edge_data_deserialize (data_h, (void *) msg);

  _nns_edge_invoke_event_cb (eh, NNS_EDGE_EVENT_NEW_DATA_RECEIVED, data_h,
      sizeof (nns_edge_data_h), NULL);

  nns_edge_data_destroy (data_h);
}

/**
 * @brief Subscribe a topic.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int
nns_edge_aitt_subscribe (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  nns_edge_aitt_handle_s *ah;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!eh->topic) {
    nns_edge_loge ("Invalid param, topic cannot be NULL for AITT connection. "
        "Please set topic using nns_edge_set_info()");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) eh->broker_h;

  if (AITT_ERROR_NONE != aitt_subscribe (ah->aitt_h, eh->topic,
          aitt_cb_message_arrived, eh, &ah->msg_h)) {
    nns_edge_loge ("Failed to subscribe the topoc: %s", eh->topic);
    return NNS_EDGE_ERROR_UNKNOWN;
  }
  return NNS_EDGE_ERROR_NONE;
}
