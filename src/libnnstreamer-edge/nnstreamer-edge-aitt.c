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

#if !defined(ENABLE_AITT)
#error "This file can be built with AITT library."
#endif

#include <stdbool.h>
#include <aitt_c.h>

#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"
#include "nnstreamer-edge-internal.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge-log.h"

/**
 * @brief Data structure for aitt handle.
 */
typedef struct
{
  aitt_h aitt_handle;
  aitt_sub_h sub_handle;
  char *id;
  char *topic;
  char *host;
  int port;

  /* event callback for new message */
  nns_edge_event_cb event_cb;
  void *user_data;
} nns_edge_aitt_handle_s;

/**
 * @brief Create AITT handle and connect to AITT.
 */
int
nns_edge_aitt_connect (const char *id, const char *topic, const char *host,
    const int port, nns_edge_aitt_h * handle)
{
  nns_edge_aitt_handle_s *ah;
  aitt_option_h option;

  if (!STR_IS_VALID (id)) {
    nns_edge_loge ("Invalid param, given id is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (topic)) {
    nns_edge_loge ("Invalid param, given topic is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (host)) {
    nns_edge_loge ("Invalid param, given host is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!PORT_IS_VALID (port)) {
    nns_edge_loge ("Invalid param, given port is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!handle) {
    nns_edge_loge ("Invalid param, handle should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_logd ("Create AITT instance: broker address: %s:%d", host, port);

  ah = (nns_edge_aitt_handle_s *) calloc (1, sizeof (nns_edge_aitt_handle_s));
  if (!ah) {
    nns_edge_loge ("Failed to allocate memory for AITT handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  option = aitt_option_new ();
  aitt_option_set (option, AITT_OPT_MY_IP, "localhost");

  ah->aitt_handle = aitt_new (id, option);
  aitt_option_destroy (option);

  if (!ah->aitt_handle) {
    nns_edge_loge ("Failed to create AITT handle. AITT internal error.");
    SAFE_FREE (ah);
    return NNS_EDGE_ERROR_UNKNOWN;
  }

  if (AITT_ERROR_NONE != aitt_connect (ah->aitt_handle, host, port)) {
    nns_edge_loge ("Failed to connect to AITT. IP:port = %s:%d", host, port);
    aitt_destroy (ah->aitt_handle);
    SAFE_FREE (ah);
    return NNS_EDGE_ERROR_UNKNOWN;
  }

  ah->id = nns_edge_strdup (id);
  ah->topic = nns_edge_strdup (topic);
  ah->host = nns_edge_strdup (host);
  ah->port = port;

  *handle = ah;
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Release the AITT handle.
 */
int
nns_edge_aitt_close (nns_edge_aitt_h handle)
{
  nns_edge_aitt_handle_s *ah;

  if (!handle) {
    nns_edge_loge ("Invalid param, given AITT handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) handle;

  /* clear event callback */
  ah->event_cb = NULL;
  ah->user_data = NULL;

  if (ah->aitt_handle) {
    if (AITT_ERROR_NONE != aitt_disconnect (ah->aitt_handle)) {
      nns_edge_loge ("Failed to close AITT handle.");
      return NNS_EDGE_ERROR_UNKNOWN;
    }

    aitt_destroy (ah->aitt_handle);
    ah->aitt_handle = NULL;
  }

  SAFE_FREE (ah->id);
  SAFE_FREE (ah->topic);
  SAFE_FREE (ah->host);
  SAFE_FREE (ah);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Check whether aitt handle exists or not.
 */
int
nns_edge_aitt_is_connected (nns_edge_aitt_h handle)
{
  nns_edge_aitt_handle_s *ah;

  if (!handle) {
    nns_edge_loge ("Invalid param, given AITT handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) handle;
  if (!ah->aitt_handle) {
    nns_edge_loge ("AITT handle is not yet connected.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Publish raw data.
 */
int
nns_edge_aitt_publish (nns_edge_aitt_h handle, const void *data,
    const int length)
{
  nns_edge_aitt_handle_s *ah;
  int ret;

  if (!handle) {
    nns_edge_loge ("Invalid param, given AITT handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data || length <= 0) {
    nns_edge_loge ("Invalid param, given data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) handle;

  ret = aitt_publish (ah->aitt_handle, ah->topic, data, length);
  if (AITT_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to publish the message. topic: %s", ah->topic);
    return NNS_EDGE_ERROR_IO;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Callback function to be called when a message is arrived.
 */
static void
aitt_cb_message_arrived (aitt_msg_h msg_handle, const void *msg,
    int msg_len, void *user_data)
{
  nns_edge_aitt_handle_s *ah;
  nns_edge_data_h data_h;
  int ret;

  ah = (nns_edge_aitt_handle_s *) user_data;

  if (!ah || !ah->event_cb) {
    nns_edge_logw ("The event callback is null, cannot handle new message.");
    return;
  }

  if (nns_edge_data_create (&data_h) != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create data handle in msg thread.");
    return;
  }

  nns_edge_data_deserialize (data_h, (void *) msg, (nns_size_t) msg_len);

  ret = nns_edge_event_invoke_callback (ah->event_cb, ah->user_data,
      NNS_EDGE_EVENT_NEW_DATA_RECEIVED, data_h, sizeof (nns_edge_data_h), NULL);
  if (ret != NNS_EDGE_ERROR_NONE)
    nns_edge_loge ("Failed to send an event for received message.");

  nns_edge_data_destroy (data_h);
}

/**
 * @brief Subscribe a topic.
 */
int
nns_edge_aitt_subscribe (nns_edge_aitt_h handle)
{
  nns_edge_aitt_handle_s *ah;

  if (!handle) {
    nns_edge_loge ("Invalid param, given AITT handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) handle;

  if (AITT_ERROR_NONE != aitt_subscribe (ah->aitt_handle, ah->topic,
          aitt_cb_message_arrived, ah, &ah->sub_handle)) {
    nns_edge_loge ("Failed to subscribe the topic: %s", ah->topic);
    return NNS_EDGE_ERROR_UNKNOWN;
  }
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Set event callback for new message.
 */
int
nns_edge_aitt_set_event_callback (nns_edge_aitt_h handle, nns_edge_event_cb cb,
    void *user_data)
{
  nns_edge_aitt_handle_s *ah;

  if (!handle) {
    nns_edge_loge ("Invalid param, given AITT handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ah = (nns_edge_aitt_handle_s *) handle;

  ah->event_cb = cb;
  ah->user_data = user_data;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal util function to send edge-data.
 */
int
nns_edge_aitt_send_data (nns_edge_aitt_h handle, nns_edge_data_h data_h)
{
  int ret;
  void *data = NULL;
  nns_size_t size;

  ret = nns_edge_data_serialize (data_h, &data, &size);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to serialize the edge data.");
    return ret;
  }

  ret = nns_edge_aitt_publish (handle, data, size);
  if (NNS_EDGE_ERROR_NONE != ret)
    nns_edge_loge ("Failed to send data to destination.");

  nns_edge_free (data);
  return ret;
}
