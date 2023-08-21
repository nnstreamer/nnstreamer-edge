/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-mqtt-paho.c
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
#include "nnstreamer-edge-mqtt.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge-queue.h"
#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"

/**
 * @brief Data structure for mqtt broker handle.
 */
typedef struct
{
  void *mqtt_h;
  nns_edge_queue_h message_queue;
  char *id;
  char *topic;
  char *host;
  int port;

  /* event callback for new message */
  nns_edge_event_cb event_cb;
  void *user_data;
} nns_edge_broker_s;

/**
 * @brief Callback function to be called when a message is arrived.
 * @return Return TRUE to prevent delivering the message again.
 */
static int
mqtt_cb_message_arrived (void *context, char *topic, int topic_len,
    MQTTAsync_message * message)
{
  nns_edge_broker_s *bh;
  char *msg = NULL;
  nns_size_t msg_len;
  int ret;

  UNUSED (topic);
  UNUSED (topic_len);
  bh = (nns_edge_broker_s *) context;

  if (!bh) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return TRUE;
  }

  if (0 >= message->payloadlen) {
    nns_edge_logw ("Invalid payload length: %d", message->payloadlen);
    return TRUE;
  }

  nns_edge_logd ("MQTT message is arrived (ID:%s, Topic:%s).",
      bh->id, bh->topic);

  msg_len = (nns_size_t) message->payloadlen;
  msg = nns_edge_memdup (message->payload, msg_len);

  if (msg) {
    if (bh->event_cb) {
      nns_edge_data_h data_h;

      if (nns_edge_data_create (&data_h) != NNS_EDGE_ERROR_NONE) {
        nns_edge_loge ("Failed to create data handle in msg thread.");
        SAFE_FREE (msg);
        return TRUE;
      }

      nns_edge_data_deserialize (data_h, (void *) msg, (nns_size_t) msg_len);

      ret = nns_edge_event_invoke_callback (bh->event_cb, bh->user_data,
          NNS_EDGE_EVENT_NEW_DATA_RECEIVED, data_h, sizeof (nns_edge_data_h),
          NULL);
      if (ret != NNS_EDGE_ERROR_NONE)
        nns_edge_loge ("Failed to send an event for received message.");

      nns_edge_data_destroy (data_h);
      SAFE_FREE (msg);
    } else {
      /* Push received message into msg queue. DO NOT free msg here. */
      nns_edge_queue_push (bh->message_queue, msg, msg_len, nns_edge_free);
    }
  }

  return TRUE;
}

/**
 * @brief Connect to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_connect (const char *id, const char *topic, const char *host,
    const int port, nns_edge_broker_h * broker_h)
{
  nns_edge_broker_s *bh;
  MQTTAsync_connectOptions options = MQTTAsync_connectOptions_initializer;
  int ret = NNS_EDGE_ERROR_NONE;
  MQTTAsync handle;
  char *url;
  char *client_id;
  unsigned int wait_count;

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

  nns_edge_logd ("Trying to connect MQTT (ID:%s, URL:%s:%d).", id, host, port);

  bh = (nns_edge_broker_s *) calloc (1, sizeof (nns_edge_broker_s));
  if (!bh) {
    nns_edge_loge ("Failed to allocate memory for broker handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  url = nns_edge_get_host_string (host, port);
  client_id = nns_edge_strdup_printf ("nns_edge_%s_%u", id, getpid ());

  ret = MQTTAsync_create (&handle, url, client_id,
      MQTTCLIENT_PERSISTENCE_NONE, NULL);
  SAFE_FREE (url);
  SAFE_FREE (client_id);

  if (MQTTASYNC_SUCCESS != ret) {
    nns_edge_loge ("Failed to create MQTT handle.");
    ret = NNS_EDGE_ERROR_CONNECTION_FAILURE;
    goto error;
  }

  bh->id = nns_edge_strdup (id);
  bh->topic = nns_edge_strdup (topic);
  bh->host = nns_edge_strdup (host);
  bh->port = port;
  bh->mqtt_h = handle;
  bh->event_cb = NULL;
  bh->user_data = NULL;
  nns_edge_queue_create (&bh->message_queue);

  MQTTAsync_setCallbacks (handle, bh, NULL, mqtt_cb_message_arrived, NULL);

  options.cleansession = 1;
  options.keepAliveInterval = 6;
  options.context = bh;

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

  *broker_h = bh;
  return NNS_EDGE_ERROR_NONE;

error:
  nns_edge_mqtt_close (bh);
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
  MQTTAsync handle;
  MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;
  unsigned int wait_count;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) broker_h;
  handle = bh->mqtt_h;

  if (handle) {
    nns_edge_logd ("Trying to disconnect MQTT (ID:%s, URL:%s:%d).",
        bh->id, bh->host, bh->port);

    options.context = bh;

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

  nns_edge_queue_destroy (bh->message_queue);
  bh->message_queue = NULL;

  SAFE_FREE (bh->id);
  SAFE_FREE (bh->topic);
  SAFE_FREE (bh->host);
  SAFE_FREE (bh);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal util function to send edge-data via MQTT connection.
 */
int
nns_edge_mqtt_publish_data (nns_edge_broker_h broker_h, nns_edge_data_h data_h)
{
  int ret;
  void *data = NULL;
  nns_size_t size;

  ret = nns_edge_data_serialize (data_h, &data, &size);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to serialize the edge data.");
    return ret;
  }

  ret = nns_edge_mqtt_publish (broker_h, data, size);
  if (NNS_EDGE_ERROR_NONE != ret)
    nns_edge_loge ("Failed to send data to destination.");

  SAFE_FREE (data);
  return ret;
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
  MQTTAsync handle;
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

  if (!MQTTAsync_isConnected (handle)) {
    nns_edge_loge ("Failed to publish message, MQTT is not connected.");
    return NNS_EDGE_ERROR_IO;
  }

  /* Publish a message (default QoS 1 - at least once and retained true). */
  ret = MQTTAsync_send (handle, bh->topic, length, data, 1, 1, NULL);
  if (ret != MQTTASYNC_SUCCESS) {
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
  MQTTAsync handle;
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

  if (!MQTTAsync_isConnected (handle)) {
    nns_edge_loge ("Failed to subscribe, MQTT is not connected.");
    return NNS_EDGE_ERROR_IO;
  }

  /* Subscribe a topic (default QoS 1 - at least once). */
  ret = MQTTAsync_subscribe (handle, bh->topic, 1, NULL);
  if (ret != MQTTASYNC_SUCCESS) {
    nns_edge_loge ("Failed to subscribe a topic (ID:%s, Topic:%s).",
        bh->id, bh->topic);
    return NNS_EDGE_ERROR_IO;
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
  MQTTAsync handle;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given broker handle is invalid.");
    return false;
  }

  bh = (nns_edge_broker_s *) broker_h;
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
 * @brief Get message from mqtt broker within timeout (0 for infinite timeout).
 */
int
nns_edge_mqtt_get_message (nns_edge_broker_h broker_h, void **msg,
    nns_size_t * msg_len, unsigned int timeout)
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

  /**
   * The time to wait for new data, in milliseconds.
   * (Default: 0 for infinite timeout)
   */
  if (!nns_edge_queue_wait_pop (bh->message_queue, timeout, msg, msg_len)) {
    nns_edge_loge ("Failed to get message from mqtt broker within timeout.");
    return NNS_EDGE_ERROR_UNKNOWN;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Set event callback for new message.
 */
int
nns_edge_mqtt_set_event_callback (nns_edge_broker_h broker_h,
    nns_edge_event_cb cb, void *user_data)
{
  nns_edge_broker_s *bh;

  if (!broker_h) {
    nns_edge_loge ("Invalid param, given MQTT handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  bh = (nns_edge_broker_s *) broker_h;

  bh->event_cb = cb;
  bh->user_data = user_data;

  return NNS_EDGE_ERROR_NONE;
}
