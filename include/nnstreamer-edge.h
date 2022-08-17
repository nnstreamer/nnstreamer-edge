/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Gichan Jang <gichan2.jang@samsung.com>
 *
 * @file   nnstreamer-edge.h
 * @date   25 Mar 2022
 * @brief  Common library to support communication among devices.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 *
 * @note This file will be moved to nnstreamer-edge repo. (https://github.com/nnstreamer/nnstreamer-edge)
 *
 * @todo Update document and sample code.
 * 1. Add sample code when the 1st API set is complete - connection, pub/sub, request, ...
 * 2. Update license when migrating this into edge repo. (Apache-2.0)
 */

#ifndef __NNSTREAMER_EDGE_H__
#define __NNSTREAMER_EDGE_H__

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_h;
typedef void *nns_edge_event_h;
typedef void *nns_edge_data_h;

/**
 * @brief The maximum number of data instances that nnstreamer-edge data may have.
 */
#define NNS_EDGE_DATA_LIMIT (16)

/**
 * @brief Enumeration for the error codes of nnstreamer-edge.
 * @todo define detailed error code later (linux standard error, sync with tizen error code)
 */
typedef enum {
  NNS_EDGE_ERROR_NONE = 0,
  NNS_EDGE_ERROR_INVALID_PARAMETER = -EINVAL,
  NNS_EDGE_ERROR_OUT_OF_MEMORY = -ENOMEM,
  NNS_EDGE_ERROR_IO = -EIO,
  NNS_EDGE_ERROR_CONNECTION_FAILURE = -ECONNREFUSED,
  NNS_EDGE_ERROR_UNKNOWN = (-1073741824LL),
  NNS_EDGE_ERROR_NOT_SUPPORTED = (NNS_EDGE_ERROR_UNKNOWN + 2),
} nns_edge_error_e;

typedef enum {
  NNS_EDGE_EVENT_UNKNOWN = 0,
  NNS_EDGE_EVENT_CAPABILITY,
  NNS_EDGE_EVENT_NEW_DATA_RECEIVED,
  NNS_EDGE_EVENT_CALLBACK_RELEASED,

  NNS_EDGE_EVENT_CUSTOM = 0x01000000
} nns_edge_event_e;

typedef enum {
  NNS_EDGE_CONNECT_TYPE_TCP = 0,
  NNS_EDGE_CONNECT_TYPE_UDP,
  NNS_EDGE_CONNECT_TYPE_MQTT,
  NNS_EDGE_CONNECT_TYPE_HYBRID,

  NNS_EDGE_CONNECT_TYPE_UNKNOWN
} nns_edge_connect_type_e;

typedef enum {
  NNS_EDGE_FLAG_NONE = 0,
  NNS_EDGE_FLAG_RECV = (1 << 0),
  NNS_EDGE_FLAG_SEND = (1 << 1),
  NNS_EDGE_FLAG_SERVER = (1 << 2),

  NNS_EDGE_FLAG_ALL = (NNS_EDGE_FLAG_RECV | NNS_EDGE_FLAG_SEND | NNS_EDGE_FLAG_SERVER)
} nns_edge_flag_e;

/**
 * @brief Callback for the nnstreamer edge event.
 * @note This callback will suspend data stream. Do not spend too much time in the callback.
 * @return User should return NNS_EDGE_ERROR_NONE if an event is successfully handled.
 */
typedef int (*nns_edge_event_cb) (nns_edge_event_h event_h, void *user_data);

/**
 * @brief Callback called when nnstreamer-edge data is released.
 */
typedef void (*nns_edge_data_destroy_cb) (void *data);

/**
 * @brief Create a handle representing an instance of edge-AI connection between a server and client (query) or a data publisher and scriber.
 * @param[in] id Unique id in local network
 * @param[in] connect_type value of @a nns_edge_connect_type_e. Connection type between edge nodes.
 * @param[in] flags value of @a nns_edge_flag_e. The value can be set by using bitwise-or operation.
 * @param[out] edge_h The edge handle. edge handle should be released using nns_edge_release_handle().
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 *
 * Here is an example of the usage:
 * @code
 * // Define edge event callback function.
 * static int _nns_edge_event_cb (nns_edge_event_h event_h, void *user_data) {
 *   nns_edge_event_e event_type;
 *   nns_edge_event_get_type (event_h, &event_type);
 *
 *   switch (event_type) {
 *     case NNS_EDGE_EVENT_CAPABILITY:
 *     {
 *       char *capability;
 *       nns_edge_event_parse_capability (event_h, &capability);
 *
 *       // Check capability string and return NNS_EDGE_ERROR_NONE if capability is acceptable.
 *       // Returning error code will deny the connection.
 *
 *       free (capability);
 *       break;
 *     }
 *     case NNS_EDGE_EVENT_NEW_DATA_RECEIVED:
 *     {
 *       nns_edge_data_h data_h;
 *       nns_edge_event_parse_new_data (event_h, &data_h);
 *
 *       // You should release returned data_h using nns_edge_data_destroy().
 *       // Recommend not to spend too much time in event callback.
 *       nns_edge_data_destroy (data_h);
 *       break;
 *     }
 *     default:
 *       break;
 *   }
 *   return NNS_EDGE_ERROR_NONE;
 * }
 *
 * // Main function
 * nns_edge_h edge_h;
 * int ret;
 *
 * // create edge handle. Select the connection type and node type. In this example, TCP and Query-client are used.
 * ret = nns_edge_create_handle ("TEMP_ID", NNS_EDGE_CONNECT_TYPE_TCP, NNS_EDGE_NODE_QUERY_CLIENT, &edge_h);
 * if (NNS_EDGE_ERROR_NONE != ret)
 *   // Handle error case. After this, handling error case is repeated, so it is omitted.
 *
 * // Information that needs to be set according to the node and connection type.
 * // 1. query-server-src:
 * //  - All connection type: HOST, PORT, CAPS
 * //  - Hybrid and AITT: DEST_HOST, DEST_PORT, TOPIC
 * // 2. query-server-sink:
 * //  - All connection type: CAPS
 * // 3. query-client
 * //  - All connection type: HOST, PORT, CAPS
 * //  - Hybrid and AITT: TOPIC
 * // 4. Stream-sink
 * //  - All connection type: HOST, PORT
 * //  - Hybrid and AITT: DEST_HOST, DEST_PORT, TOPIC
 * // 5. Stream-src
 * //  - All connection type: HOST, PORT
 * //  - Hybrid and AITT: DEST_HOST, DEST_PORT, TOPIC
 *
 * // Setting infomation for query client of TCP connection.
 * ret = nns_edge_set_info (edge_h, "HOST", "127.0.0.1");
 * ret = nns_edge_set_info (edge_h, "PORT", "1234");
 * ret = nns_edge_set_info (edge_h, "CAPS", "CAPABILITY_STRING");
 *
 * // Set event callback to receive the new data or capability.
 * // Query-server, query-client and stream-src need to set event callback.
 * nns_edge_set_event_callback (src->edge_h, _nns_edge_event_cb, src);
 *
 * // start edge handle.
 * ret = nns_edge_start (edge_h);
 *
 * // Do something...
 *
 * // Release edge handle.
 * nns_edge_release_handle (edge_h);
 */
int nns_edge_create_handle (const char *id, nns_edge_connect_type_e connect_type, int flags, nns_edge_h *edge_h);

/**
 * @brief Start the nnstreamer edge. After the start, the edge can accept a new connection or request a connection.
 * @param[in] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_CONNECTION_FAILURE Failed to get socket address.
 */
int nns_edge_start (nns_edge_h edge_h);

/**
 * @brief Release the given edge handle. All the connections are disconnected.
 * @param[in] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_release_handle (nns_edge_h edge_h);

/**
 * @brief Set the event callback to receive the new data or check the capability.
 * @param[in] edge_h The edge handle.
 * @param[in] cb The edge callbacks for event handling.
 * @param[in] user_data The user's custom data given to callbacks.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_set_event_callback (nns_edge_h edge_h, nns_edge_event_cb cb, void *user_data);

/**
 * @brief Connect to the destination node. In the case of Hybrid and AITT, the TOPIC, DEST_HOST and DEST_PORT must be set before connection using nns_edge_set_info().
 * @param[in] edge_h The edge handle.
 * @param[in] dest_host IP address to connect. In case of TCP connection, it is the ip address of the destination node, and in the case of Hybrid or AITT connection, it is the ip of the broker.
 * @param[in] dest_port The network port to connect. In case of TCP connection, it is the port of the destination node, and in the case of Hybrid or AITT connection, it is the port of the broker.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_CONNECTION_FAILURE Failed to connect to destination.
 *
 * Here is an example of the usage:
 * @code
 * // Assume edge handle is created and started.
 * nns_edge_data_h data_h;
 * char *val;
 *
 * // Query-client and subscriber may connect to query-server and publisher.
 * ret = nns_edge_connect (edge_h, "127.0.0.1", 3000);
 *
 * // Create data handle.
 * ret = nns_edge_data_create (&data_h);
 *
 * // Set raw data to be transferred.
 * ret = nns_edge_data_add (data_h, data, data_size, NULL);
 *
 * // [Optional] Set client ID to data handle. If client_id is not given, send data to all connected edeg nodes.
 * ret = nns_edge_get_info (edge_h, "client_id", &val);
 * ret = nns_edge_data_set_info (data_h, "client_id", val);
 *
 * // Transfer the data to connected edge node.
 * ret = nns_edge_send (edge_h, data_h);
 *
 * // Destroy data handle.
 * ret = nns_edge_data_destroy (data_h);
 *
 * // [Optional] Disconnect from the query-server or publisher. Or the connection is closed when edge handle is released.
 * nns_edge_disconnect (edge_h);
 */
int nns_edge_connect (nns_edge_h edge_h, const char *dest_host, int dest_port);

/**
 * @brief Disconnect from the destination node.
 * @param[in] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_disconnect (nns_edge_h edge_h);

/**
 * @brief Send data to desination (broker or connected node), asynchronously. If client_id is not set in data_h, send data to all connected edeg nodes. To set client_id in data_h, use nns_edge_data_set_info().
 * @param[in] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_IO Failed to transfer the data.
 */
int nns_edge_send (nns_edge_h edge_h, nns_edge_data_h data_h);

/**
 * @brief Deprecated, use nns_edge_send() instead.
 * @todo Remove this macro later.
 */
#define nns_edge_publish(h,d) nns_edge_send(h,d)

/**
 * @brief Set nnstreamer edge info.
 * @note The param key is case-insensitive. If same key string already exists, it will replace the old value.
 * @param[in] edge_h The edge handle.
 * @param[in] key Identifiers to determine which value to set.
 * @param[in] value The value to be set.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @section table_sec Edge info table
 * key                  | value
 * ---------------------|--------------------------------------------------------------
 * CAPS or CAPABILITY   | capapility strings.
 * IP or HOST           | IP address of the node to accept connection from other node.
 * PORT                 | Port of the node to accept connection from other node. The value should be 0 or higher.
 * DEST_IP or DEST_HOST | IP address of the destination node. In case of TCP connection, it is the ip address of the destination node, and in the case of Hybrid or AITT connection, it is the ip address of the broker.
 * DEST_PORT            | Port of the destination node. In case of TCP connection, it is the port number of the destination node, and in the case of Hybrid or AITT connection, it is the port number of the broker. The value should be 0 or higher.
 * TOPIC                | Topic used to publish/subscribe to/from the broker.
 * ID or CLIENT_ID      | Unique indentifier of the edge handle or client ID. (Read-only)
 */
int nns_edge_set_info (nns_edge_h edge_h, const char *key, const char *value);

/**
 * @brief Get nnstreamer edge info.
 * @note The param key is case-insensitive. Caller should release returned string using free().
 * @param[in] edge_h The edge handle.
 * @param[in] key Identifiers to determine which value to get.
 * @param[out] value The values that match the key.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_get_info (nns_edge_h edge_h, const char *key, char **value);

/**
 * @brief Get the nnstreamer edge event type.
 * @param[in] event_h The edge event handle.
 * @param[out] event event type. value of @a nns_edge_event_e.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_event_get_type (nns_edge_event_h event_h, nns_edge_event_e *event);

/**
 * @brief Parse edge event (NNS_EDGE_EVENT_NEW_DATA_RECEIVED) and get received data.
 * @note Caller should release returned edge data using nns_edge_data_destroy().
 * @param[in] event_h The edge event handle.
 * @param[out] data_h Handle of received data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid
 */
int nns_edge_event_parse_new_data (nns_edge_event_h event_h, nns_edge_data_h *data_h);

/**
 * @brief Parse edge event (NNS_EDGE_EVENT_CAPABILITY) and get capability string.
 * @note Caller should release returned string using free().
 * @param[in] event_h The edge event handle.
 * @param[out] capability Capability string.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid
 */
int nns_edge_event_parse_capability (nns_edge_event_h event_h, char **capability);

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
 * @note See NNS_EDGE_DATA_LIMIT, the maximum number of edge data in handle.
 * @param[in] data_h The edge data handle.
 * @param[in] data The raw data.
 * @param[in] data_len data length.
 * @param[in] destroy_cb The callback for destroying the added data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_add (nns_edge_data_h data_h, void *data, size_t data_len, nns_edge_data_destroy_cb destroy_cb);

/**
 * @brief Get the n'th edge data.
 * @note DO NOT release returned data. You should copy the data to another buffer if the returned data is necessary.
 * @param[in] data_h The edge data handle.
 * @param[in] index The index of the data to get.
 * @param[out] data The data in the data handle.
 * @param[out] data_len Byte size of the data.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_data_get (nns_edge_data_h data_h, unsigned int index, void **data, size_t *data_len);

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
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
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

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_H__ */
