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
} nns_edge_broker_s;

/**
 * @brief Connect to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_connect (nns_edge_h edge_h, const char *topic)
{
  UNUSED (edge_h);
  UNUSED (topic);
  return NNS_EDGE_ERROR_NOT_SUPPORTED;
}

/**
 * @brief Close the connection to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_close (nns_edge_h edge_h)
{
  UNUSED (edge_h);
  return NNS_EDGE_ERROR_NOT_SUPPORTED;
}

/**
 * @brief Publish raw data.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_publish (nns_edge_h edge_h, const void *data, const int length)
{
  UNUSED (edge_h);
  UNUSED (data);
  UNUSED (length);
  return NNS_EDGE_ERROR_NOT_SUPPORTED;
}

/**
 * @brief Subscribe a topic.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int
nns_edge_mqtt_subscribe (nns_edge_h edge_h)
{
  UNUSED (edge_h);
  return NNS_EDGE_ERROR_NOT_SUPPORTED;
}

/**
 * @brief Check mqtt connection
 */
bool
nns_edge_mqtt_is_connected (nns_edge_h edge_h)
{
  UNUSED (edge_h);
  return false;
}

/**
 * @brief Get message from mqtt broker.
 */
int
nns_edge_mqtt_get_message (nns_edge_h edge_h, char **msg)
{
  UNUSED (edge_h);
  UNUSED (msg);
  return NNS_EDGE_ERROR_NOT_SUPPORTED;
}
