/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-mqtt-mosquitto.c
 * @date   14 Oct 2022
 * @brief  Internal functions to support MQTT protocol (mosquitto Library).
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#if !defined(ENABLE_MQTT)
#error "This file can be built with mosquitto library."
#endif

#include <mosquitto.h>
#include "nnstreamer-edge-internal.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge-queue.h"

/**
 * @brief Data structure for mqtt broker handle.
 */
typedef struct
{
  void *mqtt_h;
  nns_edge_queue_h server_list;
  char *topic;
  bool connected;
} nns_edge_broker_s;

/**
 * @brief Callback function to be called when a message is arrived.
 */
static void
on_message_callback (struct mosquitto *client, void *data,
    const struct mosquitto_message *message)
{
  nns_edge_broker_s *bh = (nns_edge_broker_s *) data;
  char *msg = NULL;

  if (!bh) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return;
  }

  if (0 >= message->payloadlen) {
    nns_edge_logw ("Invalid payload length: %d", message->payloadlen);
    return;
  }

  nns_edge_logd ("MQTT message is arrived (ID:%d, Topic:%s).",
      message->mid, message->topic);

  msg = nns_edge_memdup (message->payload, message->payloadlen);
  if (msg)
    nns_edge_queue_push (bh->server_list, msg, nns_edge_free);

  return;
}

/**
 * @brief Initializes MQTT object.
 */
static int
_nns_edge_mqtt_init_client (nns_edge_handle_s * eh, const char *topic)
{
  nns_edge_broker_s *bh;
  int mret;
  char *client_id;
  struct mosquitto *handle;
  int ver = MQTT_PROTOCOL_V311; /** @todo check mqtt version (TizenRT repo) */

  nns_edge_logi ("Trying to connect MQTT (ID:%s, URL:%s:%d).",
      eh->id, eh->dest_host, eh->dest_port);

  bh = (nns_edge_broker_s *) calloc (1, sizeof (nns_edge_broker_s));
  if (!bh) {
    nns_edge_loge ("Failed to allocate memory for broker handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  mosquitto_lib_init ();
  client_id = nns_edge_strdup_printf ("nns_edge_%s_%u", eh->id, getpid ());

  handle = mosquitto_new (client_id, TRUE, NULL);
  SAFE_FREE (client_id);

  if (!handle) {
    nns_edge_loge ("Failed to create mosquitto client instance.");
    goto error;
  }

  mosquitto_user_data_set (handle, bh);

  mret = mosquitto_opts_set (handle, MOSQ_OPT_PROTOCOL_VERSION, &ver);
  if (MOSQ_ERR_SUCCESS != mret) {
    nns_edge_loge ("Failed to set MQTT protocol version 3.1.1.");
    goto error;
  }

  mosquitto_message_callback_set (handle, on_message_callback);

  mret = mosquitto_loop_start (handle);
  if (mret != MOSQ_ERR_SUCCESS) {
    nns_edge_loge ("Failed to start mosquitto loop.");
    goto error;
  }

  mret = mosquitto_connect (handle, eh->dest_host, eh->dest_port, 60);
  if (mret != MOSQ_ERR_SUCCESS) {
    nns_edge_loge ("Failed to connect MQTT.");
    goto error;
  }

  nns_edge_queue_create (&bh->server_list);
  bh->mqtt_h = handle;
  bh->topic = nns_edge_strdup (topic);
  bh->connected = true;

  eh->broker_h = bh;
  return NNS_EDGE_ERROR_NONE;

error:
  SAFE_FREE (bh);
  if (handle)
    mosquitto_destroy (handle);
  mosquitto_lib_cleanup ();
  return NNS_EDGE_ERROR_CONNECTION_FAILURE;
}

/**
 * @brief Connect to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_connect (nns_edge_h edge_h, const char *topic)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  if (!STR_IS_VALID (topic)) {
    nns_edge_loge ("Invalid param, given topic is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = _nns_edge_mqtt_init_client (eh, topic);
  if (NNS_EDGE_ERROR_NONE != ret)
    nns_edge_loge ("Failed to initialize the MQTT client object.");

  return ret;
}

/**
 * @brief Close the connection to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_close (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  nns_edge_broker_s *bh;
  struct mosquitto *handle;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) eh->broker_h;
  handle = bh->mqtt_h;

  if (handle) {
    nns_edge_logi ("Trying to disconnect MQTT (ID:%s, URL:%s:%d).",
        eh->id, eh->dest_host, eh->dest_port);

    /* Clear retained message */
    mosquitto_publish (handle, NULL, bh->topic, 0, NULL, 1, true);

    mosquitto_disconnect (handle);
    mosquitto_destroy (handle);
    mosquitto_lib_cleanup ();
  }

  nns_edge_queue_destroy (bh->server_list);
  bh->server_list = NULL;
  SAFE_FREE (bh->topic);
  SAFE_FREE (bh);
  eh->broker_h = NULL;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Publish raw data.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_publish (nns_edge_h edge_h, const void *data, const int length)
{
  nns_edge_handle_s *eh;
  nns_edge_broker_s *bh;
  struct mosquitto *handle;
  int ret;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data || length <= 0) {
    nns_edge_loge ("Invalid param, given data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) eh->broker_h;
  handle = bh->mqtt_h;

  if (!handle) {
    nns_edge_loge ("Invalid state, MQTT connection was not completed.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!bh->connected) {
    nns_edge_loge ("Failed to publish message, MQTT is not connected.");
    return NNS_EDGE_ERROR_IO;
  }

  /* Publish a message (default QoS 1 - at least once and retained true). */
  ret = mosquitto_publish (handle, NULL, bh->topic, length, data, 1, true);
  if (MOSQ_ERR_SUCCESS != ret) {
    nns_edge_loge ("Failed to publish a message (ID:%s, Topic:%s).",
        eh->id, eh->topic);
    return NNS_EDGE_ERROR_IO;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Subscribe a topic.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_subscribe (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  nns_edge_broker_s *bh;
  void *handle;
  int ret;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) eh->broker_h;
  handle = bh->mqtt_h;

  if (!handle) {
    nns_edge_loge ("Invalid state, MQTT connection was not completed.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!bh->connected) {
    nns_edge_loge ("Failed to subscribe, MQTT is not connected.");
    return NNS_EDGE_ERROR_IO;
  }

  /* Subscribe a topic (default QoS 1 - at least once). */
  ret = mosquitto_subscribe (handle, NULL, bh->topic, 1);
  if (MOSQ_ERR_SUCCESS != ret) {
    nns_edge_loge ("Failed to subscribe a topic (ID:%s, Topic:%s).",
        eh->id, eh->topic);
    return NNS_EDGE_ERROR_IO;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Get message from mqtt broker.
 */
int
nns_edge_mqtt_get_message (nns_edge_h edge_h, char **msg)
{
  nns_edge_handle_s *eh;
  nns_edge_broker_s *bh;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!msg) {
    nns_edge_loge ("Invalid param, given msg param is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) eh->broker_h;

  /* Wait for 1 second */
  if (!nns_edge_queue_wait_pop (bh->server_list, 1000U, (void **) msg)) {
    nns_edge_loge ("Failed to get message from mqtt broker within timeout.");
    return NNS_EDGE_ERROR_UNKNOWN;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Check mqtt connection
 */
bool
nns_edge_mqtt_is_connected (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  nns_edge_broker_s *bh;

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return false;
  }

  bh = (nns_edge_broker_s *) eh->broker_h;

  return bh->connected;
}
