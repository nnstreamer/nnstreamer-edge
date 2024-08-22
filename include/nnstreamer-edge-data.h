/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-data.h
 * @date   7 Sep 2022
 * @brief  Util functions for edge data.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#ifndef __NNSTREAMER_EDGE_DATA_H__
#define __NNSTREAMER_EDGE_DATA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_data_h;
typedef uint64_t nns_size_t;
typedef int64_t nns_ssize_t;

/**
 * @brief The maximum number of data instances that nnstreamer-edge data may have.
 */
#define NNS_EDGE_DATA_LIMIT (256)
/**
 * @brief Callback called when nnstreamer-edge data is released.
 */
typedef void (*nns_edge_data_destroy_cb) (void *data);


/**
 * @brief Create a handle used for data transmission.
 * @note Caller should release returned edge data using nns_edge_data_destroy().
 * @param[out] data_h Handle of edge data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_create (nns_edge_data_h *data_h);

/**
 * @brief Destroy nnstreamer edge data handle.
 * @param[in] data_h Handle of edge data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_destroy (nns_edge_data_h data_h);

/**
 * @brief Copy edge data and return new handle.
 * @note Caller should release returned new edge data using nns_edge_data_destroy().
 * @param[in] data_h The edge data to be copied.
 * @param[out] new_data_h A destination handle of edge data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_copy (nns_edge_data_h data_h, nns_edge_data_h *new_data_h);

/**
 * @brief Add raw data into nnstreamer edge data.
 * @note See NNS_EDGE_DATA_LIMIT, the maximum number of raw data in handle.
 * @param[in] data_h The edge data handle.
 * @param[in] data The raw data.
 * @param[in] data_len The byte size of the data.
 * @param[in] destroy_cb The callback for destroying the added data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_add (nns_edge_data_h data_h, void *data, nns_size_t data_len, nns_edge_data_destroy_cb destroy_cb);

/**
 * @brief Remove raw data in edge data.
 * @param[in] data_h The edge data handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_clear (nns_edge_data_h data_h);

/**
 * @brief Get the n'th edge data.
 * @note DO NOT release returned data. You should copy the data to another buffer if the returned data is necessary.
 * @param[in] data_h The edge data handle.
 * @param[in] index The index of the data to get.
 * @param[out] data The data in the data handle.
 * @param[out] data_len The byte size of the data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_get (nns_edge_data_h data_h, unsigned int index, void **data, nns_size_t *data_len);

/**
 * @brief Get the number of edge data in handle.
 * @param[in] data_h The edge data handle.
 * @param[out] count The number of the data in the data handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_get_count (nns_edge_data_h data_h, unsigned int *count);

/**
 * @brief Set the information of edge data.
 * @note The param key is case-insensitive. If same key string already exists, it will replace old value.
 * @param[in] data_h The edge data handle.
 * @param[in] key A key of the information.
 * @param[in] value The information to be set.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_set_info (nns_edge_data_h data_h, const char *key, const char *value);

/**
 * @brief Get the information of edge data.
 * @note The param key is case-insensitive. Caller should release the returned value using free().
 * @param[in] data_h The edge data handle.
 * @param[in] key A key of the information.
 * @param[in] value The information to get.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_get_info (nns_edge_data_h data_h, const char *key, char **value);

/**
 * @brief Clear information of edge data.
 * @param[in] data_h The edge data handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_clear_info (nns_edge_data_h data_h);

/**
 * @brief Release the edge data handle. This function releases the memory allocated for the edge data handle.
 * @param[in] data The pointer to the edge data handle to be released.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
void nns_edge_data_release_handle (void *data);

/**
 * @brief Validate edge data handle.
 * @param[in] data_h The edge data handle.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_is_valid (nns_edge_data_h data_h);

/**
 * @brief Serialize metadata in edge data.
 * @param[in] data_h The handle to the edge data.
 * @param[out] data A pointer to store the serialized meta data.
 * @param[out] data_len A pointer to store the length of the serialized meta data.
 * @return 0 on success, otherwise an error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 */
int nns_edge_data_serialize_meta (nns_edge_data_h data_h, void **data, nns_size_t *data_len);

/**
 * @brief Deserialize metadata in edge data.
 * @param[in] data_h The handle to the edge data.
 * @param[out] data A pointer to deserialized meta data.
 * @param[out] data_len Length of the deserialized meta data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_deserialize_meta (nns_edge_data_h data_h, const void *data, const nns_size_t data_len);

/**
 * @brief Serialize entire edge data (meta data + raw data).
 * @param[in] data_h The handle to the edge data.
 * @param[out] data A pointer to store the serialized edge data.
 * @param[out] data_len A pointer to store the length of the serialized edge data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 */
int nns_edge_data_serialize (nns_edge_data_h data_h, void **data, nns_size_t *data_len);

/**
 * @brief Deserialize entire edge data (meta data + raw data).
 * @param[in] data_h The handle to the edge data.
 * @param[out] data A pointer to deserialized edge data.
 * @param[out] data_len Length of the deserialized edge data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_deserialize (nns_edge_data_h data_h, const void *data, const nns_size_t data_len);

/**
 * @brief Check given data is serialized buffer.
 * @param[in] data A serialized edge data to check.
 * @param[in] data_len Length of the serialized edge data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid. */
int nns_edge_data_is_serialized (const void *data, const nns_size_t data_len);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_DATA_H__ */
