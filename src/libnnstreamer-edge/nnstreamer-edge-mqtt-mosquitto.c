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
  bool connected;

  /* event callback for new message */
  nns_edge_event_cb event_cb;
  void *user_data;

  pthread_mutex_t lock;
  pthread_cond_t cond;
  bool cleared;
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
  int ret;

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

  if (msg) {
    if (bh->event_cb) {
      nns_edge_data_h data_h;

      if (nns_edge_data_create (&data_h) != NNS_EDGE_ERROR_NONE) {
        nns_edge_loge ("Failed to create data handle in msg thread.");
        SAFE_FREE (msg);
        return;
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

  mret = nns_edge_queue_create (&bh->message_queue);
  if (NNS_EDGE_ERROR_NONE != mret) {
    nns_edge_loge ("Failed to create message queue.");
    goto error;
  }
  bh->mqtt_h = handle;
  bh->id = nns_edge_strdup (id);
  bh->topic = nns_edge_strdup (topic);
  bh->host = nns_edge_strdup (host);
  bh->port = port;
  bh->connected = true;
  bh->event_cb = NULL;
  bh->user_data = NULL;
  bh->cleared = false;
  nns_edge_lock_init (bh);
  nns_edge_cond_init (bh);

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
    nns_edge_loge ("Invalid param, broker_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = _nns_edge_mqtt_init_client (id, topic, host, port, broker_h);
  if (NNS_EDGE_ERROR_NONE != ret)
    nns_edge_loge ("Failed to initialize the MQTT client object.");

  return ret;
}

/**
 * @brief Publish callback for clearing retained message.
 * @note This callback is called both if the message is sent successfully or if the broker responded with an error.
 */
static void
_clear_retained_cb (struct mosquitto *mosq, void *obj, int mid)
{
  nns_edge_broker_s *bh = NULL;

  bh = (nns_edge_broker_s *) mosquitto_userdata (mosq);

  if (!bh || bh->cleared)
    return;

  nns_edge_lock (bh);
  bh->cleared = true;
  nns_edge_cond_signal (bh);
  nns_edge_unlock (bh);
}

/**
 * @brief Clear retained message.
 */
static void
_nns_edge_clear_retained (nns_edge_broker_s * bh)
{
  struct mosquitto *handle;
  unsigned int wait = 0U;

  if (!bh)
    return;

  handle = bh->mqtt_h;
  if (handle) {
    nns_edge_lock (bh);
    bh->cleared = false;

    mosquitto_publish_callback_set (handle, _clear_retained_cb);
    mosquitto_publish (handle, NULL, bh->topic, 0, NULL, 1, true);

    /* Wait up to 10 seconds. */
    while (!bh->cleared && ++wait < 1000U)
      nns_edge_cond_wait_until (bh, 10);

    mosquitto_publish_callback_set (handle, NULL);
    bh->cleared = true;
    nns_edge_unlock (bh);
  }
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

    _nns_edge_clear_retained (bh);

    mosquitto_disconnect (handle);
    mosquitto_destroy (handle);
    mosquitto_lib_cleanup ();
  }

  bh->mqtt_h = NULL;
  bh->connected = false;

  nns_edge_queue_destroy (bh->message_queue);
  bh->message_queue = NULL;
  nns_edge_lock_destroy (bh);
  nns_edge_cond_destroy (bh);
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
 * @brief Get message from mqtt broker within timeout (0 for infinite timeout).
 */
int
nns_edge_mqtt_get_message (nns_edge_broker_h broker_h, void **msg,
    nns_size_t * msg_len, unsigned int timeout)
{
  int ret;
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
  ret = nns_edge_queue_wait_pop (bh->message_queue, timeout, msg, msg_len);
  if (NNS_EDGE_ERROR_NONE != ret)
    nns_edge_loge ("Failed to get message from mqtt broker within timeout.");

  return ret;
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
