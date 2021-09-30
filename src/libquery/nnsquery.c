/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2021 Sangjung Woo <sangjung.woo@samsung.com>
 */
/**
 * @file    nnsquery.c
 * @date    24 Aug 2021
 * @brief   Implementation of Common library to support MQTT-hybrid protocol
 * @see     https://github.com/nnstreamer/nnstreamer-edge
 * @author  Sangjung Woo <sangjung.woo@samsung.com>
 * @bug     No known bugs except for NYI items
 */
#include "nnsquery.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <MQTTAsync.h>

#define debug_print(fmt, ...) \
        do { if (DEBUG) fprintf(stderr, "%s:%d:%s(): " fmt "\n", __FILE__, \
                                __LINE__, __func__, ##__VA_ARGS__); } while (0)

typedef struct _QueryInfo
{
  char *mqtt_host_address;
  char *mqtt_host_port;
  char *mqtt_subscribe_topic;
  char *mqtt_client_id;

  query_mqtt_state_t mqtt_state;
  query_state_change_cb state_change_cb;
  void *user_data;

  query_msg_received_cb msg_received_cb;
  void *msg_user_data;

  MQTTAsync mqtt_client;
  MQTTAsync_connectOptions mqtt_conn_opts;
  MQTTAsync_disconnectOptions mqtt_disconn_opts;
} QueryInfo;

static uint8_t client_id_seq = 0;
static const char DEFAULT_MQTT_HOST_ADDRESS[] = "tcp://localhost";
static const char DEFAULT_MQTT_HOST_PORT[] = "1883";
static const int DEFAULT_MQTT_QOS = 1;
static const int BUFFER_SIZE = 1024;

/**
 * @brief Callback function to be called if MQTT connection is established.
 * @note If user register the state change callback, then the new state is set to users.
 */
static void
cb_mqtt_on_connect (void *context, MQTTAsync_successData * response)
{
  QueryInfo *info = (QueryInfo *) context;

  info->mqtt_state = MQTT_CONNECTED;
  debug_print ("Info: MQTT is connected");

  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief  Callback function to be called if the connect fails.
 * @note If user register the state change callback, then the new state is set to users.
 */
static void
cb_mqtt_on_connect_failure (void *context, MQTTAsync_failureData * response)
{
  QueryInfo *info = (QueryInfo *) context;

  info->mqtt_state = MQTT_CONNECT_FAILURE;
  debug_print ("Error: MQTT connection failure");

  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief Callback function to be called if the disconnect successfully completes.
 * @note If user register the state change callback, then the new state is set to users.
 */
static void
cb_mqtt_on_disconnect (void *context, MQTTAsync_successData * response)
{
  QueryInfo *info = (QueryInfo *) context;

  info->mqtt_state = MQTT_DISCONNECTED;
  debug_print ("Info: MQTT connection is closed");

  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief Callback function to be called if the disconnect fails.
 * @note If user register the state change callback, then the new state is set to users.
 */
static void
cb_mqtt_on_disconnect_failure (void *context, MQTTAsync_failureData * response)
{
  QueryInfo *info = (QueryInfo *) context;

  if (info->mqtt_state == MQTT_DISCONNECTED) {
    debug_print ("Info: current state: %d", info->mqtt_state);
    return;
  }

  info->mqtt_state = MQTT_DISCONNECT_FAILED;
  debug_print ("Info: MQTT connection is failed to close");
  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief Callback function to be called if the connection to the server is lost.
 * @note If user register the state change callback, then the new state is set to users.
 */
static void
cb_mqtt_on_connection_lost (void *context, char *cause)
{
  QueryInfo *info = (QueryInfo *) context;

  info->mqtt_state = MQTT_CONNECTION_LOST;
  debug_print ("Info: MQTT connection lost");

  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief Callback function to be called when subscribing the topic is success
 */
static void
cb_mqtt_on_subscribe_success (void *context, MQTTAsync_successData * response)
{
  QueryInfo *info = (QueryInfo *) context;
  debug_print ("Info: success to MQTT subscription: %s",
      info->mqtt_subscribe_topic);
}

/**
 * @brief Callback function when receiving the message for specific topic
 * @note If user register the msg_received_cb callback, then the payload with related information is sent to users.
 */
static int
cb_mqtt_on_message_received (void *context, char *topic_name,
    int topic_len, MQTTAsync_message * message)
{
  QueryInfo *info = (QueryInfo *) context;

  debug_print ("Info: Message arrived: %s (%d)", topic_name,
      message->payloadlen);

  if (info->msg_received_cb) {
    info->msg_received_cb (topic_name,
        message->payload, message->payloadlen, info->msg_user_data);
  }

  return 1;
}

static void
init_query_info (QueryInfo * info)
{
  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  MQTTAsync_disconnectOptions disconn_opts =
      MQTTAsync_disconnectOptions_initializer;

  /* initialize QueryInfo */
  info->mqtt_host_address = NULL;
  info->mqtt_host_port = NULL;
  info->mqtt_subscribe_topic = NULL;
  info->mqtt_client_id = NULL;

  info->mqtt_state = MQTT_INITIALIZING;

  info->state_change_cb = NULL;
  info->user_data = NULL;

  info->msg_received_cb = NULL;
  info->msg_user_data = NULL;

  /* MQTT connection option */
  info->mqtt_conn_opts = conn_opts;
  info->mqtt_conn_opts.cleansession = 1;
  info->mqtt_conn_opts.keepAliveInterval = 6;
  info->mqtt_conn_opts.onSuccess = cb_mqtt_on_connect;
  info->mqtt_conn_opts.onFailure = cb_mqtt_on_connect_failure;
  info->mqtt_conn_opts.context = info;

  /* MQTT disconnect option */
  info->mqtt_disconn_opts = disconn_opts;
  info->mqtt_disconn_opts.context = info;
  info->mqtt_disconn_opts.onSuccess = cb_mqtt_on_disconnect;
  info->mqtt_disconn_opts.onFailure = cb_mqtt_on_disconnect_failure;
}

static void
finalize_query_info (QueryInfo * info)
{
  if (info->mqtt_host_address) {
    free (info->mqtt_host_address);
    info->mqtt_host_address = NULL;
  }

  if (info->mqtt_host_port) {
    free (info->mqtt_host_port);
    info->mqtt_host_port = NULL;
  }

  if (info->mqtt_subscribe_topic) {
    free (info->mqtt_subscribe_topic);
    info->mqtt_subscribe_topic = NULL;
  }

  if (info->mqtt_client_id) {
    free (info->mqtt_client_id);
    info->mqtt_client_id = NULL;
  }
}

/**
 * @brief Open the connection of the MQTT broker with specific options
 */
int
query_open_connection (query_h * handle,
    char *host_addr, char *host_port,
    query_state_change_cb callback, void *user_data)
{
  QueryInfo *info = NULL;
  char server_url[BUFFER_SIZE];
  char client_id[BUFFER_SIZE];
  int ret = 0;

  /* check parameters */
  if (!handle) {
    debug_print ("Error: Invalid Param: handle is NULL");
    return -EINVAL;
  }

  info = (QueryInfo *) malloc (sizeof (QueryInfo));
  if (!info) {
    debug_print ("Error: failed to malloc()");
    return -ENOMEM;
  }
  init_query_info (info);

  /* set parameters */
  info->mqtt_host_address =
      host_addr ? strdup (host_addr) : strdup (DEFAULT_MQTT_HOST_ADDRESS);
  info->mqtt_host_port =
      host_port ? strdup (host_port) : strdup (DEFAULT_MQTT_HOST_PORT);

  snprintf (client_id, BUFFER_SIZE, "nns_query_%u_%u", getpid (),
      client_id_seq++);
  info->mqtt_client_id = strdup (client_id);

  /* set state change callback */
  if (callback) {
    info->state_change_cb = callback;
    info->user_data = user_data;
  }

  /* create MQTT client object */
  snprintf (server_url, BUFFER_SIZE, "%s:%s", info->mqtt_host_address,
      info->mqtt_host_port);
  ret =
      MQTTAsync_create (&info->mqtt_client, server_url, info->mqtt_client_id,
      MQTTCLIENT_PERSISTENCE_NONE, NULL);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_create(): %d", ret);
    goto error_handling;
  }

  /* connect MQTT Server */
  MQTTAsync_setCallbacks (info->mqtt_client, info,
      cb_mqtt_on_connection_lost, cb_mqtt_on_message_received, NULL);
  ret = MQTTAsync_connect (info->mqtt_client, &info->mqtt_conn_opts);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_connect()");
    goto error_handling;
  }

  *handle = info;
  return 0;

error_handling:
  finalize_query_info (info);
  free (info);
  return ret;
}

/**
 * @brief Close the connection of the MQTT broker and release the allocated memory space.
 */
int
query_close_connection (query_h handle)
{
  QueryInfo *info = (QueryInfo *) handle;
  int ret;

  /* check the current state */
  if (info->mqtt_state != MQTT_CONNECTED) {
    debug_print ("Error: mqtt_state is not connected!: %d", info->mqtt_state);
    return MQTTASYNC_DISCONNECTED;
  }

  /* update the state to block other requests */
  info->mqtt_state = MQTT_REQUEST_STOP;

  /* try to disconnect */
  while (MQTTAsync_isConnected (info->mqtt_client)) {
    ret = MQTTAsync_disconnect (info->mqtt_client, &info->mqtt_disconn_opts);
    if (ret != MQTTASYNC_SUCCESS) {
      debug_print ("Error: failed to MQTTAsync_disconnect()");
      return ret;
    }
  }

  /* cleanup resources */
  MQTTAsync_destroy (&info->mqtt_client);
  finalize_query_info (info);
  free (info);

  return 0;
}

/**
 * @brief Publish the raw message data from the edge server
 */
int
query_publish_raw_data (query_h handle,
    char *topic_name, void *payload, int payload_size, bool retained)
{
  QueryInfo *info = (QueryInfo *) handle;
  msg_data buf;
  int ret = 0;
  int total_size = 0;

  /* check the input parameters */
  if (!handle) {
    debug_print ("Error: Invalid Param: handle is NULL");
    return -EINVAL;
  }

  if (!topic_name) {
    debug_print ("Error: Invalid Param: topic name is NULL");
    return -EINVAL;
  }

  if (!payload) {
    debug_print ("Error: Invalid Param: payload is NULL");
    return -EINVAL;
  }

  if (payload_size <= 0) {
    debug_print ("Error: Invalid Param: payload_size should be bigger than 0.");
    return -EINVAL;
  }

  /* check the current state */
  if (info->mqtt_state != MQTT_CONNECTED) {
    debug_print ("Error: mqtt_state is not connected!: %d", info->mqtt_state);
    return MQTTASYNC_DISCONNECTED;
  }

  /* construct msg_data */
  buf.type = MSG_RAW;
  memcpy (buf.payload, payload, payload_size);
  total_size = sizeof (msg_type_t) + payload_size;

  /* send message to the MQTT broker */
  ret = MQTTAsync_send (info->mqtt_client,
      topic_name, total_size, &buf, DEFAULT_MQTT_QOS, (retained ? 1 : 0), NULL);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_send()");
    return ret;
  }

  return 0;
}

/**
 * @brief Publish the TCP message data from the edge server
 */
int
query_publish_tcp_data (query_h handle, char *topic_name,
    const char *hostname, uint16_t port, tcp_server_state_t state,
    void *custom, int custom_size, bool retained)
{
  QueryInfo *info = (QueryInfo *) handle;
  msg_data buf = { 0 };
  int ret;
  int total_size = 0;

  /* check the input parameters */
  if (!handle) {
    debug_print ("Error: Invalid Param: handle is NULL");
    return -EINVAL;
  }

  if (!topic_name) {
    debug_print ("Error: Invalid Param: topic name is NULL");
    return -EINVAL;
  }

  if (!hostname) {
    debug_print ("Error: Invalid Param: topic name is NULL");
    return -EINVAL;
  }

  /* check the current state */
  if (info->mqtt_state != MQTT_CONNECTED) {
    debug_print ("Error: mqtt_state is not connected!: %d", info->mqtt_state);
    return MQTTASYNC_DISCONNECTED;
  }

  /* construct msg_data */
  buf.type = MSG_TCP;
  memcpy (buf.srv_host, hostname, strlen (hostname));
  buf.state = state;
  buf.port = port;
  total_size = TCP_BASIC_SIZE;

  if (custom && custom_size > 0) {
    memcpy (buf.custom, custom, custom_size);
    total_size += custom_size;
  }

  /* send message to the MQTT broker */
  ret = MQTTAsync_send (info->mqtt_client,
      topic_name, total_size, &buf, DEFAULT_MQTT_QOS, (retained ? 1 : 0), NULL);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_send()");
    return ret;
  }

  return 0;
}

/**
 * @brief Subscribe the specific topic and register the callback function for message data.
 */
int
query_subscribe_topic (query_h handle, const char *topic_name,
    query_msg_received_cb callback, void *user_data)
{
  QueryInfo *info = (QueryInfo *) handle;
  MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
  int ret = 0;

  /* check the input parameters */
  if (!handle) {
    debug_print ("Error: Invalid Param: handle is NULL");
    return -EINVAL;
  }

  if (!topic_name) {
    debug_print ("Error: Invalid Param: topic name is NULL");
    return -EINVAL;
  }

  /* check the current state */
  if (info->mqtt_state != MQTT_CONNECTED) {
    debug_print ("Error: mqtt_state is not connected!: %d", info->mqtt_state);
    return MQTTASYNC_DISCONNECTED;
  }

  if (info->mqtt_subscribe_topic) {
    debug_print ("Error: Already subscribe! Topic: %s",
        info->mqtt_subscribe_topic);
    return MQTTASYNC_FAILURE;
  }

  /* set the parameters */
  info->mqtt_subscribe_topic = strdup (topic_name);
  if (callback) {
    info->msg_received_cb = callback;
    info->msg_user_data = user_data;
  }

  opts.onSuccess = cb_mqtt_on_subscribe_success;
  opts.onFailure = NULL;
  opts.context = info;

  ret = MQTTAsync_subscribe (info->mqtt_client, topic_name,
      DEFAULT_MQTT_QOS, &opts);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_subscribe()");
    free (info->mqtt_subscribe_topic);
    info->mqtt_subscribe_topic = NULL;
    return ret;
  }

  return 0;
}

/**
 * @brief Clear the retained topic in the MQTT broker
 */
int
query_clear_retained_topic (query_h handle, char *topic_name)
{
  QueryInfo *info = (QueryInfo *) handle;
  int ret = 0;

  /* check the input parameters */
  if (!handle) {
    debug_print ("Error: Invalid Param: handle is NULL");
    return -EINVAL;
  }

  if (!topic_name) {
    debug_print ("Error: Invalid Param: topic name is NULL");
    return -EINVAL;
  }

  /* check the current state */
  if (info->mqtt_state != MQTT_CONNECTED) {
    debug_print ("Error: mqtt_state is not connected!: %d", info->mqtt_state);
    return MQTTASYNC_DISCONNECTED;
  }

  /* send message to the MQTT broker */
  ret = MQTTAsync_send (info->mqtt_client,
      topic_name, 0, NULL, DEFAULT_MQTT_QOS, 1, NULL);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_send()");
    return ret;
  }

  return 0;
}
