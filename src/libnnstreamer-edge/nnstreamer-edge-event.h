/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-event.h
 * @date   7 Sep 2022
 * @brief  Util functions for nnstreamer edge event.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 * @note   This file is internal header for nnstreamer edge utils. DO NOT export this file.
 */

#ifndef __NNSTREAMER_EDGE_EVENT_H__
#define __NNSTREAMER_EDGE_EVENT_H__

#include "nnstreamer-edge.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Internal util function to invoke event callback.
 */
int nns_edge_event_invoke_callback (nns_edge_event_cb event_cb, void *user_data, nns_edge_event_e event, void *data, nns_size_t data_len, nns_edge_data_destroy_cb destroy_cb);

/**
 * @brief Create nnstreamer edge event.
 * @note This is internal function for edge event.
 */
int nns_edge_event_create (nns_edge_event_e event, nns_edge_event_h *event_h);

/**
 * @brief Destroy nnstreamer edge event.
 * @note This is internal function for edge event.
 */
int nns_edge_event_destroy (nns_edge_event_h event_h);

/**
 * @brief Set event data.
 * @note This is internal function for edge event.
 */
int nns_edge_event_set_data (nns_edge_event_h event_h, void *data, nns_size_t data_len, nns_edge_data_destroy_cb destroy_cb);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_EVENT_H__ */
