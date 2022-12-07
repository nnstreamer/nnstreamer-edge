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
  char *id;
  char *topic;
  char *host;
  int port;
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
  nns_size_t msg_len;

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

  msg_len = (nns_size_t) message->payloadlen;
  msg = nns_edge_memdup (message->payload, msg_len);
  if (msg)
    nns_edge_queue_push (bh->server_list, msg, msg_len, nns_edge_free);

  return;
}

/**
 * @brief Initializes MQTT object.
 */
static int
_nns_edge_mqtt_init_client (const char *id, const char *topic, const char *host,
    const int port, nns_edge_broker_h * broker_h)
{
  nns_edge_broker_s *bh;
  int mret;
  char *client_id;
  struct mosquitto *handle;
  int ver = MQTT_PROTOCOL_V311; /** @todo check mqtt version (TizenRT repo) */

  nns_edge_logd ("Trying to connect MQTT (ID:%s, URL:%s:%d).", id, host, port);

  bh = (nns_edge_broker_s *) calloc (1, sizeof (nns_edge_broker_s));
  if (!bh) {
    nns_edge_loge ("Failed to allocate memory for broker handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  mosquitto_lib_init ();
  client_id = nns_edge_strdup_printf ("nns_edge_%s_%u", id, getpid ());

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

  mret = mosquitto_connect (handle, host, port, 60);
  if (mret != MOSQ_ERR_SUCCESS) {
    nns_edge_loge ("Failed to connect MQTT.");
    goto error;
  }

  nns_edge_queue_create (&bh->server_list);
  bh->mqtt_h = handle;
  bh->id = nns_edge_strdup (id);
  bh->topic = nns_edge_strdup (topic);
  bh->host = nns_edge_strdup (host);
  bh->port = port;
  bh->connected = true;

  *broker_h = bh;
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
nns_edge_mqtt_connect (const char *id, const char *topic, const char *host,
    const int port, nns_edge_broker_h * broker_h)
{
  int ret = NNS_EDGE_ERROR_NONE;

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

  if (!broker_h) {
    nns_edge_loge ("Invalid param, mqtt_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = _nns_edge_mqtt_init_client (id, topic, host, port, broker_h);
  if (NNS_EDGE_ERROR_NONE != ret)
    nns_edge_loge ("Failed to initialize the MQTT client object.");

  return ret;
}

/**
 * @brief Close the connection to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_close (nns_edge_broker_h broker_h)
{
  nns_edge_broker_s *bh;
  struct mosquitto *handle;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) broker_h;
  handle = bh->mqtt_h;

  if (handle) {
    nns_edge_logd ("Trying to disconnect MQTT (ID:%s, URL:%s:%d).",
        bh->id, bh->host, bh->port);

    /* Clear retained message */
    mosquitto_publish (handle, NULL, bh->topic, 0, NULL, 1, true);

    mosquitto_disconnect (handle);
    mosquitto_destroy (handle);
    mosquitto_lib_cleanup ();
  }

  nns_edge_queue_destroy (bh->server_list);
  bh->server_list = NULL;
  SAFE_FREE (bh->id);
  SAFE_FREE (bh->topic);
  SAFE_FREE (bh->host);
  SAFE_FREE (bh);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Publish raw data.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_publish (nns_edge_broker_h broker_h, const void *data,
    const int length)
{
  nns_edge_broker_s *bh;
  struct mosquitto *handle;
  int ret;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!data || length <= 0) {
    nns_edge_loge ("Invalid param, given data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) broker_h;
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
        bh->id, bh->topic);
    return NNS_EDGE_ERROR_IO;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Subscribe a topic.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_subscribe (nns_edge_broker_h broker_h)
{
  nns_edge_broker_s *bh;
  void *handle;
  int ret;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) broker_h;
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
        bh->id, bh->topic);
    return NNS_EDGE_ERROR_IO;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Get message from mqtt broker.
 */
int
nns_edge_mqtt_get_message (nns_edge_broker_h broker_h, void **msg,
    nns_size_t * msg_len)
{
  nns_edge_broker_s *bh;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!msg) {
    nns_edge_loge ("Invalid param, given msg param is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) broker_h;

  /* Wait for 1 second */
  if (!nns_edge_queue_wait_pop (bh->server_list, 1000U, msg, msg_len)) {
    nns_edge_loge ("Failed to get message from mqtt broker within timeout.");
    return NNS_EDGE_ERROR_UNKNOWN;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Check mqtt connection
 */
bool
nns_edge_mqtt_is_connected (nns_edge_broker_h broker_h)
{
  nns_edge_broker_s *bh;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return false;
  }

  bh = (nns_edge_broker_s *) broker_h;

  return bh->connected;
}
