/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2021 Sangjung Woo <sangjung.woo@samsung.com>
 */
/**
 * @file    nnsquery.h
 * @date    24 Aug 2021
 * @brief   Common library to support MQTT-hybrid protocol
 * @see     https://github.com/nnstreamer/nnstreamer-edge
 * @author  Sangjung Woo <sangjung.woo@samsung.com>
 * @bug     No known bugs except for NYI items
 */
#ifndef __NNSQUERY_H__
#define __NNSQUERY_H__

#include <stdint.h>
#include <limits.h>
#include <stdbool.h>

#define MAX_MSG_SIZE            1020
#define MAX_TCP_CUSTOM_SIZE     950

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum _query_mqtt_state_t {
  MQTT_CONNECTION_LOST = -3,    /**< MQTT connection is lost */
  MQTT_CONNECT_FAILURE = -2,    /**< MQTT connection is failed */
  MQTT_INITIALIZING = -1,       /**< Not connected yet */
  MQTT_CONNECTED,               /**< Connected to server successfully */
  MQTT_REQUEST_STOP,            /**< User request to disconnect */
  MQTT_DISCONNECTED,            /**< MQTT connection is closed */
  MQTT_DISCONNECT_FAILED,       /**< Disconnect is failed */
} query_mqtt_state_t;

typedef enum _msg_type_t {
    MSG_RAW = 0,      /**< Raw message type */
    MSG_TCP,          /**< TCP message type */
    MSG_UDP,          /**< UDP message type */
} msg_type_t;

typedef enum _tcp_server_state_t {
  TCP_CLOSED = -2,
  TCP_CLOSING = -1,
  TCP_READY,
  TCP_BUSY,
} tcp_server_state_t;

typedef struct _msg_data {
  msg_type_t type;                          /**< 4 byte: message type */
  union {
    /* for raw data */
    struct {
      uint8_t payload[MAX_MSG_SIZE];        /* 1020 byte: payload */
    };

    /* for TCP data */
    struct {
      uint8_t srv_host[HOST_NAME_MAX];      /**< 64 byte: server host name */
      uint16_t port;                        /**< 2 byte: port number */
      tcp_server_state_t state;             /**< 4 byte: tcp server state */
      uint8_t custom[MAX_TCP_CUSTOM_SIZE];  /**< 950 byte: custom data (optional) */
    } __attribute__((aligned(1), packed));;
  };
} msg_data;

/**
 * @brief The basic size of TCP message.
 */
#define TCP_BASIC_SIZE (sizeof(msg_type_t) + HOST_NAME_MAX + sizeof(uint16_t) + sizeof(tcp_server_state_t))

typedef void *query_h;

/**
 * @brief Callback function for MQTT broker state change
 * @param[in,out] user_data User data that provided when calling query_open_connection()
 * @param[in] state The new state of MQTT broker
 */
typedef void (*query_state_change_cb) (void *user_data, query_mqtt_state_t state);

/**
 * @brief Callback function for receiving the message data
 * @param[in] topic_name The subscribed topic name
 * @param[in] msg Message data that received from MQTT broker
 * @param[in] msg_len Message length that received from MQTT broker
 * @param[in,out] user_data User data that provided when calling query_open_connection()
 */
typedef void (*query_msg_received_cb) (const char *topic_name,
    msg_data *msg, int msg_len, void *user_data);

/**
 * @brief Open the connection of the MQTT broker with specific options
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in,out] handle MQTT query handle to connect
 * @param[in] host_addr Host address to connect to. If NULL, then 'tcp://localhost' is used.
 * @param[in] host_port Network port of host to connect to. If NULL, then '1883' is used.
 * @param[in] callback State change callback for MQTT broker. If NULL, callback event is not used.
 * @param[in] user_data User data for callback function. If @callback is NULL, then user_data is ignored.
 */
int
query_open_connection (query_h *handle,
    char *host_addr, char *host_port,
    query_state_change_cb callback, void *user_data);

/**
 * @brief Close the connection of the MQTT broker and release the allocated memory space.
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in] handle MQTT handle to close.
 */
int
query_close_connection (query_h handle);

/**
 * @brief Publish the raw message data from the edge server
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in] handle MQTT broker handle
 * @param[in] topic_name The topic name to publish message data.
 * @param[in] payload The payload to publish message data.
 * @param[in] payload_size The payload size of the message data.
 * @param[in] retained If true, the broker stores the last message and publish it when starting to subscribe.
 */
int
query_publish_raw_data (query_h handle, 
    char *topic_name,
    void *payload, int payload_size, bool retained);

/**
 * @brief Publish the TCP message data from the edge server
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in] handle MQTT broker handle
 * @param[in] topic_name The topic name to publish message data.
 * @param[in] hostname The hostname of the edge server
 * @param[in] port The port number of the edge server
 * @param[in] state The state information of the edge server
 * @param[in] custom The custom data to publish (optional)
 * @param[in] custom_size The size of the custom data. If 0, custom is ignored.
 * @param[in] retained If true, the broker stores the last message and publish it when starting to subscribe.
 */
int
query_publish_tcp_data (query_h handle, char *topic_name,
    const char *hostname, uint16_t port, tcp_server_state_t state,
    void *custom, int custom_size,
    bool retained);

/**
 * @brief Subscribe the specific topic and register the callback function for message data.
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in] handle MQTT broker handle
 * @param[in] topic_name The topic name to subscribe.
 * @param[in] callback Callback function for receiving the message data
 * @param[in] msg_user_data User data for callback function. If @callback is NULL, then user_data is ignored.
 */
int
query_subscribe_topic (query_h handle, const char *topic_name,
    query_msg_received_cb callback, void* msg_user_data);

/**
 * @brief Clear the retained topic in the MQTT broker
 * @return @c 0 on success. Otherwise a negative error value.
 * @param[in] handle MQTT broker handle.
 * @param[in] topic_name The topic name to clear.
 */
int
query_clear_retained_topic (query_h handle, char *topic_name);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSQUERY_H__ */
