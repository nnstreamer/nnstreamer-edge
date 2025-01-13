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
 */

#ifndef __NNSTREAMER_EDGE_EVENT_H__
#define __NNSTREAMER_EDGE_EVENT_H__

#include "nnstreamer-edge-data.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_event_h;

/**
 * @brief Enumeration for the event type of nnstreamer-edge.
 */
typedef enum {
  NNS_EDGE_EVENT_UNKNOWN = 0,
  NNS_EDGE_EVENT_CAPABILITY,
  NNS_EDGE_EVENT_NEW_DATA_RECEIVED,
  NNS_EDGE_EVENT_CALLBACK_RELEASED,
  NNS_EDGE_EVENT_CONNECTION_CLOSED,
  NNS_EDGE_EVENT_CONNECTION_COMPLETED,
  NNS_EDGE_EVENT_CONNECTION_FAILURE,
  NNS_EDGE_EVENT_DEVICE_FOUND,

  NNS_EDGE_EVENT_CUSTOM = 0x01000000
} nns_edge_event_e;

/**
 * @brief Callback for the nnstreamer edge event.
 * @note This callback will suspend data stream. Do not spend too much time in the callback.
 * @param[in] event_h The edge event handle.
 * @param[in] user_data The user's custom data given to callbacks.
 * @return 0 on success. Otherwise a negative error value.
 */
typedef int (*nns_edge_event_cb) (nns_edge_event_h event_h, void *user_data);

/**
 * @brief Get the nnstreamer edge event type.
 * @param[in] event_h The edge event handle.
 * @param[out] event The event type, value of @a nns_edge_event_e.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_event_get_type (nns_edge_event_h event_h, nns_edge_event_e *event);

/**
 * @brief Parse edge event (NNS_EDGE_EVENT_NEW_DATA_RECEIVED) and get received data.
 * @remarks If the function succeeds, @a data_h should be released using nns_edge_data_destroy().
 * @param[in] event_h The edge event handle.
 * @param[out] data_h Handle of received data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid
 */
int nns_edge_event_parse_new_data (nns_edge_event_h event_h, nns_edge_data_h *data_h);

/**
 * @brief Parse edge event (NNS_EDGE_EVENT_CAPABILITY) and get capability string.
 * @remarks If the function succeeds, @a capability should be released using free().
 * @param[in] event_h The edge event handle.
 * @param[out] capability Capability string.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid
 */
int nns_edge_event_parse_capability (nns_edge_event_h event_h, char **capability);

/**
 * @brief Util function to invoke event callback.
 * @param[in] event_cb Callback for the nnstreamer edge event.
 * @param[in] user_data The user's custom data given to callbacks.
 * @param[in] data A pointer to event data.
 * @param[in] data_len Length of the event data.
 * @param[in] destroy_cb A callback function to free the event data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 */
int nns_edge_event_invoke_callback (nns_edge_event_cb event_cb, void *user_data, nns_edge_event_e event, void *data, nns_size_t data_len, nns_edge_data_destroy_cb destroy_cb);

/**
 * @brief Create nnstreamer edge event.
 * @remarks If the function succeeds, @a event_h should be released using nns_edge_event_destroy().
 * @param[in] event Edge event type.
 * @param[out] event_h The handle of the created edge event.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 */
int nns_edge_event_create (nns_edge_event_e event, nns_edge_event_h *event_h);

/**
 * @brief Destroy nnstreamer edge event.
 * @param[in] event_h The handle of the event to be destroyed.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_event_destroy (nns_edge_event_h event_h);

/**
 * @brief Set event data.
 * @param[in] event_h The handle of edge event.
 * @param[in] data A pointer to event data.
 * @param[in] data_len The byte size of the event data.
 * @param[in] destroy_cb A callback function to destroy the event data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_event_set_data (nns_edge_event_h event_h, void *data, nns_size_t data_len, nns_edge_data_destroy_cb destroy_cb);

/**
 * @brief Get event data.
 * @note DO NOT release returned data. You should copy the data to another buffer if the returned data is necessary.
 * @param[in] event_h The handle of edge event.
 * @param[out] data The data in the event handle.
 * @param[out] data_len The byte size of the event data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_event_get_data (nns_edge_event_h event_h, void **data, nns_size_t *data_len);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_EVENT_H__ */
