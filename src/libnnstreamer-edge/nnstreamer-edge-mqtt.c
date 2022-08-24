/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-mqtt.c
 * @date   11 May 2022
 * @brief  Internal functions to support MQTT protocol (Paho Asynchronous MQTT C Client Library).
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Sangjung Woo <sangjung.woo@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#if !defined(ENABLE_MQTT)
#error "This file can be built with Paho MQTT library."
#endif

#include <MQTTAsync.h>
#include "nnstreamer-edge-internal.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"

/**
 * @brief Data structure for mqtt broker handle.
 */
typedef struct
{
  void *mqtt_h;
  GAsyncQueue *server_list;
  char *topic;
} nns_edge_broker_s;

/**
 * @brief Callback function to be called when a message is arrived.
 * @return Return TRUE to prevent delivering the message again.
 */
static int
mqtt_cb_message_arrived (void *context, char *topic, int topic_len,
    MQTTAsync_message * message)
{
  nns_edge_handle_s *eh;
  nns_edge_broker_s *bh;
  char *msg = NULL;

  UNUSED (topic);
  UNUSED (topic_len);
  eh = (nns_edge_handle_s *) context;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return TRUE;
  }

  if (0 >= message->payloadlen) {
    nns_edge_logw ("Invalid payload lenth: %d", message->payloadlen);
    return TRUE;
  }

  bh = (nns_edge_broker_s *) eh->broker_h;

  nns_edge_logd ("MQTT message is arrived (ID:%s, Topic:%s).",
      eh->id, eh->topic);

  msg = nns_edge_memdup (message->payload, message->payloadlen);
  if (msg)
    g_async_queue_push (bh->server_list, msg);

  return TRUE;
}

/**
 * @brief Connect to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_connect (nns_edge_h edge_h, const char *topic)
{
  nns_edge_handle_s *eh;
  nns_edge_broker_s *bh;
  MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
  int ret = NNS_EDGE_ERROR_NONE;
  MQTTAsync handle;
  char *url;
  char *client_id;
  unsigned int wait_count;

  if (!STR_IS_VALID (topic)) {
    nns_edge_loge ("Invalid param, given topic is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_logi ("Trying to connect MQTT (ID:%s, URL:%s:%d).",
      eh->id, eh->dest_host, eh->dest_port);

  bh = (nns_edge_broker_s *) calloc (1, sizeof (nns_edge_broker_s));
  if (!bh) {
    nns_edge_loge ("Failed to allocate memory for broker handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  url = nns_edge_get_host_string (eh->dest_host, eh->dest_port);
  client_id = nns_edge_strdup_printf ("nns_edge_%s_%u", eh->id, getpid ());

  ret = MQTTAsync_create (&handle, url, client_id,
      MQTTCLIENT_PERSISTENCE_NONE, NULL);
  SAFE_FREE (url);
  SAFE_FREE (client_id);

  if (MQTTASYNC_SUCCESS != ret) {
    nns_edge_loge ("Failed to create MQTT handle.");
    ret = NNS_EDGE_ERROR_CONNECTION_FAILURE;
    goto error;
  }

  bh->topic = nns_edge_strdup (topic);
  bh->mqtt_h = handle;
  bh->server_list = g_async_queue_new ();
  eh->broker_h = bh;

  MQTTAsync_setCallbacks (handle, edge_h, NULL, mqtt_cb_message_arrived, NULL);

  options.cleansession = 1;
  options.keepAliveInterval = 6;
  options.context = edge_h;

  if (MQTTAsync_connect (handle, &options) != MQTTASYNC_SUCCESS) {
    nns_edge_loge ("Failed to connect MQTT.");
    ret = NNS_EDGE_ERROR_CONNECTION_FAILURE;
    goto error;
  }

  /* Waiting for the connection */
  wait_count = 0U;
  do {
    if (wait_count > 500U) {
      ret = NNS_EDGE_ERROR_CONNECTION_FAILURE;
      goto error;
    }

    usleep (10000);
    wait_count++;
  } while (!MQTTAsync_isConnected (handle));

  return NNS_EDGE_ERROR_NONE;

error:
  nns_edge_mqtt_close (eh);
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
  MQTTAsync handle;
  MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;
  char *msg;
  unsigned int wait_count;

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

    options.context = edge_h;

    /* Clear retained message */
    MQTTAsync_send (handle, bh->topic, 0, NULL, 1, 1, NULL);

    wait_count = 0U;
    do {
      if (MQTTAsync_disconnect (handle, &options) != MQTTASYNC_SUCCESS) {
        nns_edge_loge ("Failed to disconnect MQTT.");
        break;
      }

      if (wait_count > 500U) {
        nns_edge_loge ("Failed to disconnect MQTT, timed out.");
        break;
      }

      usleep (10000);
      wait_count++;
    } while (MQTTAsync_isConnected (handle));

    MQTTAsync_destroy (&handle);
  }

  while ((msg = g_async_queue_try_pop (bh->server_list))) {
    SAFE_FREE (msg);
  }
  g_async_queue_unref (bh->server_list);
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
  MQTTAsync handle;
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

  if (!MQTTAsync_isConnected (handle)) {
    nns_edge_loge ("Failed to publish message, MQTT is not connected.");
    return NNS_EDGE_ERROR_IO;
  }

  /* Publish a message (default QoS 1 - at least once and retained true). */
  ret = MQTTAsync_send (handle, bh->topic, length, data, 1, 1, NULL);
  if (ret != MQTTASYNC_SUCCESS) {
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
  MQTTAsync handle;
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

  if (!MQTTAsync_isConnected (handle)) {
    nns_edge_loge ("Failed to subscribe, MQTT is not connected.");
    return NNS_EDGE_ERROR_IO;
  }

  /* Subscribe a topic (default QoS 1 - at least once). */
  ret = MQTTAsync_subscribe (handle, bh->topic, 1, NULL);
  if (ret != MQTTASYNC_SUCCESS) {
    nns_edge_loge ("Failed to subscribe a topic (ID:%s, Topic:%s).",
        eh->id, eh->topic);
    return NNS_EDGE_ERROR_IO;
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
  MQTTAsync handle;
  eh = (nns_edge_handle_s *) edge_h;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh) || !eh->broker_h) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return false;
  }

  bh = (nns_edge_broker_s *) eh->broker_h;
  handle = bh->mqtt_h;

  if (!handle) {
    nns_edge_loge ("Invalid state, MQTT connection was not completed.");
    return false;
  }

  if (MQTTAsync_isConnected (handle)) {
    return true;
  }

  return false;
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
  *msg = g_async_queue_timeout_pop (bh->server_list, 1000000U);
  if (!*msg) {
    nns_edge_loge ("Failed to get message from mqtt broker within timeout.");
    return NNS_EDGE_ERROR_UNKNOWN;
  }

  return NNS_EDGE_ERROR_NONE;
}
