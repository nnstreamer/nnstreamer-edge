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
#include "nnstreamer-edge-metadata.h"
#include "nnstreamer-edge-aitt.h"
#include "nnstreamer-edge-mqtt.h"
#include "nnstreamer-edge-queue.h"

/**
 * @brief Internal data structure for raw data.
 */
typedef struct {
  void *data;
  size_t data_len;
  nns_edge_data_destroy_cb destroy_cb;
} nns_edge_raw_data_s;

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
  nns_edge_node_type_e node_type;
  nns_edge_metadata_h metadata;

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

  /* MQTT or AITT handle */
  void *broker_h;
} nns_edge_handle_s;

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_INTERNAL_H__ */
