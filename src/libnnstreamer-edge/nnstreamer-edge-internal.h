/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-internal.h
 * @date   11 May 2022
 * @brief  Internal functions and definition to support communication among devices.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 * @note   This file is internal header for nnstreamer edge. DO NOT export this file.
 */

#ifndef __NNSTREAMER_EDGE_INTERNAL_H__
#define __NNSTREAMER_EDGE_INTERNAL_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "nnstreamer-edge.h"
#include "nnstreamer-edge-queue.h"

typedef void *nns_edge_broker_h;

/**
 * @brief Internal data structure for raw data.
 */
typedef struct {
  void *data;
  size_t data_len;
  nns_edge_data_destroy_cb destroy_cb;
} nns_edge_raw_data_s;

/**
 * @brief Internal data structure for metadata.
 */
typedef struct _nns_edge_metadata_node_s nns_edge_metadata_node_s;

/**
 * @brief Internal data structure for metadata.
 */
struct _nns_edge_metadata_node_s {
  char *key;
  char *value;
  nns_edge_metadata_node_s *next;
};

/**
 * @brief Internal data structure to handle metadata. This struct should be managed in the handle.
 */
typedef struct {
  unsigned int list_len;
  nns_edge_metadata_node_s *list;
} nns_edge_metadata_s;

/**
 * @brief Internal data structure for edge event.
 */
typedef struct {
  unsigned int magic;
  nns_edge_event_e event;
  nns_edge_raw_data_s data;
} nns_edge_event_s;

/**
 * @brief Data structure for edge handle.
 */
typedef struct {
  unsigned int magic;
  pthread_mutex_t lock;
  char *id;
  char *topic;
  nns_edge_connect_type_e connect_type;
  char *host; /**< host name or IP address */
  int port; /**< port number (0~65535, default 0 to get available port.) */
  char *dest_host; /**< destination IP address (broker or target device) */
  int dest_port; /**< destination port number (broker or target device) */
  int flags;
  nns_edge_metadata_s meta;

  /* Edge event callback and user data */
  nns_edge_event_cb event_cb;
  void *user_data;

  int64_t client_id;
  char *caps_str;

  /* list of connection data */
  void *connections;

  /* socket listener */
  bool listening;
  int listener_fd;
  pthread_t listener_thread;

  /* thread and queue to send data */
  nns_edge_queue_h send_queue;
  pthread_t send_thread;

  /* MQTT */
  nns_edge_broker_h broker_h;
} nns_edge_handle_s;

#if defined(ENABLE_MQTT)
/**
 * @brief Connect to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_connect (nns_edge_h edge_h, const char *topic);

/**
 * @brief Close the connection to MQTT.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_close (nns_edge_h edge_h);

/**
 * @brief Publish raw data.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_publish (nns_edge_h edge_h, const void *data, const int length);

/**
 * @brief Subscribe a topic.
 * @note This is internal function for MQTT broker. You should call this with edge-handle lock.
 */
int nns_edge_mqtt_subscribe (nns_edge_h edge_h);

/**
 * @brief Check mqtt connection
 */
bool nns_edge_mqtt_is_connected (nns_edge_h edge_h);

/**
 * @brief Get message from mqtt broker.
 */
int nns_edge_mqtt_get_message (nns_edge_h edge_h, char **msg);

#else
/**
 * @todo consider to change code style later.
 * If MQTT is disabled, nnstreamer does not include nnstreamer_edge_mqtt.c, and changing code style will make error as it is not used function now.
 *
 * static int nns_edge_mqtt_publish (nns_edge_h edge_h, const void *data, const int length)
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
#endif

#if defined(ENABLE_AITT)
/**
 * @brief Create AITT handle and connect to AITT.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_connect (nns_edge_h edge_h);

/**
 * @brief Release the AITT handle.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_close (nns_edge_h edge_h);

/**
 * @brief Publish raw data.
 * @note This is internal function forAITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_publish (nns_edge_h edge_h, const void *data, const int length);

/**
 * @brief Subscribe a topic.
 * @note This is internal function for AITT. You should call this with edge-handle lock.
 */
int nns_edge_aitt_subscribe (nns_edge_h edge_h);
#else
#define nns_edge_aitt_connect(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_close(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_publish(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#define nns_edge_aitt_subscribe(...) (NNS_EDGE_ERROR_NOT_SUPPORTED)
#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_INTERNAL_H__ */
