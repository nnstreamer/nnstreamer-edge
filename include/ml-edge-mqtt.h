/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2021 Sangjung Woo <sangjung.woo@samsung.com>
 */
/**
 * @file    ml-edge-mqtt.h
 * @date    05 July 2021
 * @brief   ML Edge APIs for publishing the any type of data as a MQTT topic
 * @see     https://github.com/nnstreamer/nnstreamer-edge
 * @author  Sangjung Woo <sangjung.woo@samsung.com>
 * @bug     No known bugs except for NYI items
 */
#ifndef __ML_EDGE_MQTT_H__
#define __ML_EDGE_MQTT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum _edge_mqtt_state_t {
  MQTT_CONNECTION_LOST = -3,    /**< MQTT connection is lost */
  MQTT_CONNECT_FAILURE = -2,    /**< MQTT connection is failed */
  MQTT_INITIALIZING = -1,       /**< Not connected yet */
  MQTT_CONNECTED,               /**< Connected to server successfully */
  MQTT_REQUEST_STOP,            /**< User request to disconnect */
  MQTT_SENDING,                 /**< MQTT message is sent but ACK is not received */
  MQTT_DELIVERY_COMPLETE,       /**< All message is successfully delivered */
  MQTT_SEND_ERROR,              /**< Failed to sent the message */
  MQTT_DISCONNECTED,            /**< MQTT connection is closed */
  MQTT_DISCONNECT_FAILED,       /**< Disconnect is failed */
} edge_mqtt_state_t;

typedef void *edge_h;

/**
 * @brief Callback function for MQTT state change
 * @param[in,out] user_data User data that provided when calling edge_open_connection()
 * @param[in] state The new state of MQTT
 */
typedef void (*edge_state_change_cb) (void *user_data, edge_mqtt_state_t state);

/**
 * @brief Open the MQTT connection with specific options
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in,out] handle MQTT handle to connect
 * @param[in] host_address Host address to connect to. If NULL, then 'tcp://localhost' is used.
 * @param[in] host_port Network port of host to connect to. If NULL, then '1883' is used.
 * @param[in] base_time_stamp Base time stamp in usec. If 0, then current time stamp is set.
 * @param[in] callback State change callback for MQTT. If NULL, callback event is not used.
 * @param[in] user_data User data for callback function. If @callback is NULL, then user_data is ignored.
 */
int
edge_open_connection (edge_h *handle,
    char *host_address, char *host_port,
    int64_t base_time_stamp,
    edge_state_change_cb callback, void *user_data);

/**
 * @brief Publish the single message that contains only one record.
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in] handle MQTT handle to publish message.
 * @param[in] topic_name The topic name to publish message data.
 * @param[in] payload Payload to publish.
 * @param[in] payload_size The payload size to publish.
 */
int
edge_publish_msg_for_mqttsrc (edge_h handle,
    char *topic_name,
    void *payload, uint64_t payload_size);

/**
 * @brief Close the MQTT connection and release the allocated memory space.
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in] handle MQTT handle to close.
 */
int
edge_close_connection (edge_h handle);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __ML_EDGE_MQTT_H__ */
