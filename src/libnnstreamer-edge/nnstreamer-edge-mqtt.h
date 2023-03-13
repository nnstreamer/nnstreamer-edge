/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-mqtt.h
 * @date   11 May 2022
 * @brief  Internal functions to support MQTT protocol (Paho Asynchronous MQTT C Client Library).
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Sangjung Woo <sangjung.woo@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#ifndef __NNSTREAMER_EDGE_MQTT_H__
#define __NNSTREAMER_EDGE_MQTT_H__

#include <stdbool.h>
#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_broker_h;

#if defined(ENABLE_MQTT)
/**
 * @brief Connect to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_connect (const char *id, const char *topic, const char *host, const int port, nns_edge_broker_h *broker_h);

/**
 * @brief Close the connection to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_close (nns_edge_broker_h broker_h);

/**
 * @brief Publish raw data.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_publish (nns_edge_broker_h broker_h, const void *data, const int length);

/**
 * @brief Subscribe a topic.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_subscribe (nns_edge_broker_h broker_h);

/**
 * @brief Check mqtt connection
 */
bool nns_edge_mqtt_is_connected (nns_edge_broker_h broker_h);

/**
 * @brief Get message from mqtt broker. If no message in the queue, it waits up to 1 second for new message.
 */
int nns_edge_mqtt_get_message (nns_edge_broker_h broker_h, void **msg, nns_size_t *msg_len);

/**
 * @brief Internal util function to send edge-data via MQTT connection.
 */
int nns_edge_mqtt_publish_data (nns_edge_broker_h handle, nns_edge_data_h data_h);

/**
 * @brief Set event callback for new message.
 */
int nns_edge_mqtt_set_event_callback (nns_edge_broker_h broker_h, nns_edge_event_cb cb, void *user_data);

#else
/**
 * @todo consider to change code style later.
 * If MQTT is disabled, nnstreamer does not include nnstreamer_edge_mqtt.c, and changing code style will make error as it is not used function now.
 *
 * static int nns_edge_mqtt_publish (nns_edge_broker_h broker_h, const void *data, const int length)
 * {
 *   return NNS_EDGE_ERROR_NOT_SUPPORTED;
 * }
 */
#define nns_edge_mqtt_connect(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mqtt_close(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mqtt_publish(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mqtt_subscribe(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mqtt_is_connected(...) (false)
#define nns_edge_mqtt_get_message(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mqtt_publish_data(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_mqtt_set_event_callback(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#endif /* ENABLE_MQTT */

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_MQTT_H__ */
