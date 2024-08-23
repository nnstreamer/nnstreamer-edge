/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-queue.h
 * @date   24 August 2022
 * @brief  Thread-safe queue.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Jaeyun Jung <jy1210.jung@samsung.com>
 * @note   This file is internal header for nnstreamer-edge. DO NOT export this file.
 * @bug No known bugs except for NYI items.
 */

#ifndef __NNSTREAMER_EDGE_QUEUE_H__
#define __NNSTREAMER_EDGE_QUEUE_H__

#include "nnstreamer-edge-data.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_queue_h;

/**
 * @brief Enumeration for the queue leaky option.
 */
typedef enum {
  NNS_EDGE_QUEUE_LEAK_UNKNOWN = 0,
  NNS_EDGE_QUEUE_LEAK_NEW,
  NNS_EDGE_QUEUE_LEAK_OLD
} nns_edge_queue_leak_e;

/**
 * @brief Create queue. Default length limit is 0 (unlimited).
 * @remarks If the function succeeds, @a handle should be released using nns_edge_queue_destroy().
 * @param[out] handle Newly created handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_queue_create (nns_edge_queue_h *handle);

/**
 * @brief Destroy queue.
 * @param[in] handle The queue handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_queue_destroy (nns_edge_queue_h handle);

/**
 * @brief Get the length of the queue.
 * @param[in] handle The queue handle.
 * @return The number of data in the queue.
 */
unsigned int nns_edge_queue_get_length (nns_edge_queue_h handle);

/**
 * @brief Set the max length of the queue.
 * @param[in] handle The queue handle.
 * @param[in] limit The max data in queue. Default 0 means unlimited.
 * @param[in] leaky The queue leaky option.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_queue_set_limit (nns_edge_queue_h handle, unsigned int limit, nns_edge_queue_leak_e leaky);

/**
 * @brief Add new data into queue.
 * @param[in] handle The queue handle.
 * @param[in] data The data to be added.
 * @param[in] size The size of pushed data.
 * @param[in] destroy Nullable, the callback function to release data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_queue_push (nns_edge_queue_h handle, void *data, nns_size_t size, nns_edge_data_destroy_cb destroy);

/**
 * @brief Remove and return the first data in queue.
 * @param[in] handle The queue handle.
 * @param[out] data The data in the queue.
 * @param[out] size The size of data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_IO Failed to get data.
 */
int nns_edge_queue_pop (nns_edge_queue_h handle, void **data, nns_size_t *size);

/**
 * @brief Remove and return the first data in queue. If queue is empty, wait until new data is added in the queue.
 * @param[in] handle The queue handle.
 * @param[in] timeout The time to wait for new data, in milliseconds. (0 for infinite timeout)
 * @param[out] data The data in the queue.
 * @param[out] size The size of data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_IO Failed to get data.
 */
int nns_edge_queue_wait_pop (nns_edge_queue_h handle, unsigned int timeout, void **data, nns_size_t *size);

/**
 * @brief Stop waiting for new data and clear all data in the queue.
 * @details When this function is called, nns_edge_queue_wait_pop() will stop the waiting.
 * @param[in] handle The queue handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_queue_clear (nns_edge_queue_h handle);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_QUEUE_H__ */
