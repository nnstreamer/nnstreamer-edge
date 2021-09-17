/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2021 Sangjung Woo <sangjung.woo@samsung.com>
 */
/**
 * @file    edge_sensor.c
 * @date    05 July 2021
 * @brief   Implementation of Edge APIs for publishing the any type of data as a MQTT topic
 * @see     https://github.com/nnstreamer/nnstreamer-edge
 * @author  Sangjung Woo <sangjung.woo@samsung.com>
 * @bug     No known bugs except for NYI items
 */
#include "mqttcommon.h"
#include "edge_sensor.h"

#include <MQTTAsync.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#define debug_print(fmt, ...) \
        do { if (DEBUG) fprintf(stderr, "%s:%d:%s(): " fmt "\n", __FILE__, \
                                __LINE__, __func__, ##__VA_ARGS__); } while (0)

#define GST_CLOCK_TIME_NONE           ((uint64_t) -1)

/**
 * @brief Structures for edge info.
 */
typedef struct _EdgeInfo
{
  char *mqtt_host_address;
  char *mqtt_host_port;
  char *mqtt_client_id;
  char *mqtt_topic;

  edge_mqtt_state_t mqtt_state;

  GstMQTTMessageHdr mqtt_msg_hdr;
  uint64_t mqtt_msg_buf_size;

  edge_state_change_cb state_change_cb;
  void *user_data;

  MQTTAsync mqtt_client;
  MQTTAsync_connectOptions mqtt_conn_opts;
  MQTTAsync_responseOptions mqtt_respn_opts;
  MQTTAsync_disconnectOptions mqtt_disconn_opts;
  MQTTAsync_token mqtt_last_sent_token;

  void *mqtt_msg_buf;
} EdgeInfo;

static uint8_t sink_client_id = 0;
static const char DEFAULT_MQTT_HOST_ADDRESS[] = "tcp://localhost";
static const char DEFAULT_MQTT_HOST_PORT[] = "1883";
static const int DEFAULT_MQTT_QOS = 1;
static const char DEFAULT_MQTT_PUB_TOPIC_FORMAT[] = "%s/topic";
static const int BUFFER_SIZE = 1024;
static const unsigned long DEFAULT_MQTT_DISCONNECT_TIMEOUT = 3 * 1000;  /* 3 secs */

/**
 * @brief Callback function to be called if MQTT connection is established.
 * @note If user register the state change callback, then the new state is set to users.
 */
static void
cb_mqtt_on_connect (void *context, MQTTAsync_successData * response)
{
  EdgeInfo *info = (EdgeInfo *) context;

  info->mqtt_state = MQTT_CONNECTED;
  debug_print ("Info: MQTT connectioned");

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
  EdgeInfo *info = (EdgeInfo *) context;

  info->mqtt_state = MQTT_CONNECT_FAILURE;
  debug_print ("Error: MQTT connection failure");

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
  EdgeInfo *info = (EdgeInfo *) context;

  info->mqtt_state = MQTT_CONNECTION_LOST;
  debug_print ("Error: MQTT connection lost");

  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief Callback function to be called the message is completed to deliver to the server.
 */
static void
cb_mqtt_on_delivery_complete (void *context, MQTTAsync_token token)
{
  EdgeInfo *info = (EdgeInfo *) context;
  debug_print ("Info: Message token %d is completed", token);

  if (info->mqtt_last_sent_token >= token) {
    info->mqtt_state = MQTT_DELIVERY_COMPLETE;
  }
}

/**
 * @brief Callback function to be called the message is sent successfully.
 */
static void
cb_mqtt_on_send_success (void *context, MQTTAsync_successData * response)
{
  EdgeInfo *info = (EdgeInfo *) context;

  info->mqtt_state = MQTT_SENDING;
  info->mqtt_last_sent_token = response->token;
  debug_print ("Info: Message token %d is sent", response->token);
}

/**
 * @brief Callback function to be called the message is failed to send.
 */
static void
cb_mqtt_on_send_failure (void *context, MQTTAsync_failureData * response)
{
  EdgeInfo *info = (EdgeInfo *) context;

  info->mqtt_state = MQTT_SEND_ERROR;
  debug_print ("Error: Message token %d is failed to send", response->token);
}

/**
 * @brief Callback function to be called if the disconnect successfully completes.
 * @note If user register the state change callback, then the new state is set to users.
 */
static void
cb_mqtt_on_disconnect (void *context, MQTTAsync_successData * response)
{
  EdgeInfo *info = (EdgeInfo *) context;

  info->mqtt_state = MQTT_DISCONNECTED;
  debug_print ("Info: MQTT connection is closed");

  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief Callback function to be called if the disconnect fails.
 * @note If user register the state change callback, then the new state is set to users.
 *       If the state is MQTT_SENDING or MQTT_DISCONNECTED, then MQTT_DISCONNECT_FAILED is not sent to users.
 */
static void
cb_mqtt_on_disconnect_failure (void *context, MQTTAsync_failureData * response)
{
  EdgeInfo *info = (EdgeInfo *) context;

  if ((info->mqtt_state == MQTT_SENDING) ||
      (info->mqtt_state == MQTT_DISCONNECTED)) {
    debug_print ("state: %d", info->mqtt_state);
    return;
  }

  info->mqtt_state = MQTT_DISCONNECT_FAILED;
  debug_print ("Info: MQTT connection is failed to close");
  if (info->state_change_cb) {
    info->state_change_cb (info->user_data, info->mqtt_state);
  }
}

/**
 * @brief Get the system time in microseconds format.
 * @return the number of microseconds since January 1, 1970 UTC.
 */
static int64_t
get_time_stamp ()
{
  const int64_t GST_SEC_TO_US_MULTIPLIER = 1000000;
  struct timeval tv;
  gettimeofday (&tv, NULL);
  return tv.tv_sec * (int64_t) GST_SEC_TO_US_MULTIPLIER + tv.tv_usec;
}

/**
 * @brief Initialize the EdgeInfo structure.
 * @param[in,out] info The pointer of EdgeInfo structure.
 */
static void
init_edge_info (EdgeInfo * info)
{
  MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
  MQTTAsync_responseOptions respn_opts = MQTTAsync_responseOptions_initializer;
  MQTTAsync_disconnectOptions disconn_opts =
      MQTTAsync_disconnectOptions_initializer;

  /* initialize GstMQTTMessageHdr */
  info->mqtt_msg_hdr.num_mems = 0;
  info->mqtt_msg_hdr.base_time_epoch = 0;
  info->mqtt_msg_hdr.sent_time_epoch = 0;
  info->mqtt_msg_hdr.duration = GST_CLOCK_TIME_NONE;
  info->mqtt_msg_hdr.dts = GST_CLOCK_TIME_NONE;
  info->mqtt_msg_hdr.pts = GST_CLOCK_TIME_NONE;
  memset (info->mqtt_msg_hdr.gst_caps_str, 0x00,
      sizeof (info->mqtt_msg_hdr.gst_caps_str));

  /* initialize EdgeInfo */
  info->mqtt_host_address = NULL;
  info->mqtt_host_port = NULL;
  info->mqtt_client_id = NULL;
  info->mqtt_topic = NULL;

  info->mqtt_state = MQTT_INITIALIZING;
  info->mqtt_msg_buf = NULL;
  info->mqtt_msg_buf_size = 0;
  info->mqtt_last_sent_token = -1;

  info->state_change_cb = NULL;
  info->user_data = NULL;

  /* MQTT connection option */
  info->mqtt_conn_opts = conn_opts;
  info->mqtt_conn_opts.cleansession = 1;
  info->mqtt_conn_opts.keepAliveInterval = 6;
  info->mqtt_conn_opts.onSuccess = cb_mqtt_on_connect;
  info->mqtt_conn_opts.onFailure = cb_mqtt_on_connect_failure;
  info->mqtt_conn_opts.context = info;

  /* MQTT response option */
  info->mqtt_respn_opts = respn_opts;
  info->mqtt_respn_opts.context = info;
  info->mqtt_respn_opts.onSuccess = cb_mqtt_on_send_success;
  info->mqtt_respn_opts.onFailure = cb_mqtt_on_send_failure;

  /* MQTT disconnect option */
  info->mqtt_disconn_opts = disconn_opts;
  info->mqtt_disconn_opts.context = info;
  info->mqtt_disconn_opts.onSuccess = cb_mqtt_on_disconnect;
  info->mqtt_disconn_opts.onFailure = cb_mqtt_on_disconnect_failure;
}

/**
 * @brief Release the allocated memory in the EdgeInfo structure.
 * @param[in] info The pointer of EdgeInfo structure.
 */
static void
finalize_edge_info (EdgeInfo * info)
{
  if (info->mqtt_host_address) {
    free (info->mqtt_host_address);
    info->mqtt_host_address = NULL;
  }

  if (info->mqtt_host_port) {
    free (info->mqtt_host_port);
    info->mqtt_host_port = NULL;
  }

  if (info->mqtt_client_id) {
    free (info->mqtt_client_id);
    info->mqtt_client_id = NULL;
  }

  if (info->mqtt_topic) {
    free (info->mqtt_topic);
    info->mqtt_topic = NULL;
  }

  if (info->mqtt_msg_buf) {
    free (info->mqtt_msg_buf);
    info->mqtt_msg_buf = NULL;
  }
}

/**
 * @brief Close the MQTT connection and release the allocated memory space.
 */
int
edge_close_connection (edge_h handle)
{
  EdgeInfo *info = (EdgeInfo *) handle;
  int ret;

  if (!handle) {
    debug_print("Error: Invalid Param: handle is NULL");
    return -1;
  }

  /* check the current state */
  if ((info->mqtt_state != MQTT_CONNECTED) &&
      (info->mqtt_state != MQTT_DELIVERY_COMPLETE) &&
      (info->mqtt_state != MQTT_SENDING)) {
    debug_print ("Error: mqtt_state is invalid!: %d", info->mqtt_state);
    return -1;
  }

  /* support QoS */
  if (info->mqtt_state == MQTT_SENDING) {
    debug_print ("Info: MQTTAsync_waitForCompletion(): %d",
        info->mqtt_last_sent_token);
    MQTTAsync_waitForCompletion (info->mqtt_client, info->mqtt_last_sent_token,
        DEFAULT_MQTT_DISCONNECT_TIMEOUT);
  }
  /* In case of MQTT_REQUEST_STOP state, publishing new message is denied. */
  info->mqtt_state = MQTT_REQUEST_STOP;

  /* try to disconnect */
  while (MQTTAsync_isConnected (info->mqtt_client)) {
    ret = MQTTAsync_disconnect (info->mqtt_client, &info->mqtt_disconn_opts);
    if (ret != MQTTASYNC_SUCCESS) {
      debug_print ("Error: failed to MQTTAsync_disconnect()");
      return -1;
    }
  }

  /* cleanup resources */
  MQTTAsync_destroy (&info->mqtt_client);
  finalize_edge_info (info);
  free (info);

  return 0;
}

/**
 * @brief Open the MQTT connection with specific options
 */
int
edge_open_connection (edge_h * handle,
    char *host_address, char *host_port, char *topic_name,
    int64_t base_time_stamp, uint64_t duration, char *gst_caps_string,
    edge_state_change_cb callback, void *user_data)
{
  EdgeInfo *info = NULL;
  char server_url[BUFFER_SIZE];
  char client_id[BUFFER_SIZE];
  char topic[BUFFER_SIZE];
  int ret = 0;

  if (!handle) {
    debug_print ("Error: Invalid Param: handle is NULL");
    return -1;
  }

  info = (EdgeInfo *) malloc (sizeof (EdgeInfo));
  if (!info) {
    debug_print ("Error: failed to malloc()");
    return -1;
  }
  init_edge_info (info);

  /* Set parameter */
  info->mqtt_host_address =
      host_address ? strdup (host_address) : strdup (DEFAULT_MQTT_HOST_ADDRESS);
  info->mqtt_host_port =
      host_port ? strdup (host_port) : strdup (DEFAULT_MQTT_HOST_PORT);

  snprintf (client_id, BUFFER_SIZE, "edge_sensor_%u_%u", getpid (),
      sink_client_id++);
  info->mqtt_client_id = strdup (client_id);

  if (topic_name) {
    info->mqtt_topic = strdup (topic_name);
  } else {
    snprintf (topic, BUFFER_SIZE, DEFAULT_MQTT_PUB_TOPIC_FORMAT,
        info->mqtt_client_id);
    info->mqtt_topic = strdup (topic);
  }

  /* Set base time stamp if user is provided. If not, current time stamp is used */
  if (base_time_stamp == 0) {
    info->mqtt_msg_hdr.base_time_epoch =
        get_time_stamp () * GST_US_TO_NS_MULTIPLIER;
  } else {
    info->mqtt_msg_hdr.base_time_epoch = base_time_stamp;
  }

  /* Set state change callback */
  if (callback) {
    info->state_change_cb = callback;
    info->user_data = user_data;
  }

  /* Create MQTT Client object */
  snprintf (server_url, BUFFER_SIZE, "%s:%s", info->mqtt_host_address,
      info->mqtt_host_port);
  ret =
      MQTTAsync_create (&info->mqtt_client, server_url, info->mqtt_client_id,
      MQTTCLIENT_PERSISTENCE_NONE, NULL);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_create(): %d", ret);
    goto error_handling;
  }

  /* Attempt to connect MQTT Server */
  MQTTAsync_setCallbacks (info->mqtt_client, info,
      cb_mqtt_on_connection_lost, NULL, cb_mqtt_on_delivery_complete);
  ret = MQTTAsync_connect (info->mqtt_client, &info->mqtt_conn_opts);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_connect()");
    goto error_handling;
  }
  *handle = info;
  return 0;

error_handling:
  finalize_edge_info (info);
  free (info);
  return -1;
}

/**
 * @brief Publish the single message that contains only one record.
 *
 */
int
edge_publish_single_msg (edge_h handle, void *buffer, uint64_t payload_size)
{
  EdgeInfo *info = (EdgeInfo *) handle;
  int ret = 0;
  uint8_t *msg_pub;

  /* check the input parameters */
  if (!handle) {
    debug_print ("Error: Invalid Param: handle is NULL");
    return -1;
  }

  if (!buffer) {
    debug_print ("Error: Invalid Param: buffer is NULL");
    return -1;
  }

  if (payload_size == 0) {
    debug_print ("Error: Invalid Param: payload_size should be bigger than 0.");
    return -1;
  }

  /* check the current state */
  if ((info->mqtt_state != MQTT_CONNECTED) &&
      (info->mqtt_state != MQTT_DELIVERY_COMPLETE) &&
      (info->mqtt_state != MQTT_SENDING)) {
    debug_print ("Error: mqtt_state is invalid!: %d", info->mqtt_state);
    return -1;
  }

  /* alocate the MQTT buffer memory */
  if (payload_size > info->mqtt_msg_buf_size) {
    info->mqtt_msg_buf_size = payload_size + GST_MQTT_LEN_MSG_HDR;;
    free (info->mqtt_msg_buf);
    info->mqtt_msg_buf = NULL;
  }
  if (!info->mqtt_msg_buf) {
    info->mqtt_msg_buf = malloc (info->mqtt_msg_buf_size);
    if (!info->mqtt_msg_buf) {
      debug_print ("Error: failed malloc()");
      return -1;
    }
  }

  /* setup header info */
  info->mqtt_msg_hdr.num_mems = 1;
  info->mqtt_msg_hdr.size_mems[0] = payload_size;

  /* set timestamp */
  info->mqtt_msg_hdr.sent_time_epoch =
      get_time_stamp () * GST_US_TO_NS_MULTIPLIER;
  info->mqtt_msg_hdr.pts =
      info->mqtt_msg_hdr.base_time_epoch - info->mqtt_msg_hdr.sent_time_epoch;
  info->mqtt_msg_hdr.dts = GST_CLOCK_TIME_NONE;
  info->mqtt_msg_hdr.duration = GST_CLOCK_TIME_NONE;

  /* copy header and payload */
  msg_pub = info->mqtt_msg_buf;
  memcpy (msg_pub, &info->mqtt_msg_hdr, sizeof (info->mqtt_msg_hdr));
  memcpy (&msg_pub[sizeof (info->mqtt_msg_hdr)], buffer, payload_size);

  /* send message to the MQTT server */
  ret = MQTTAsync_send (info->mqtt_client, info->mqtt_topic,
      info->mqtt_msg_buf_size, info->mqtt_msg_buf,
      DEFAULT_MQTT_QOS, 1, &info->mqtt_respn_opts);
  if (ret != MQTTASYNC_SUCCESS) {
    debug_print ("Error: failed MQTTAsync_send()");
    return -1;
  }
  return ret;
}
