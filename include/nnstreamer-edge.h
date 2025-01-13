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
 */

#ifndef __NNSTREAMER_EDGE_H__
#define __NNSTREAMER_EDGE_H__

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef void *nns_edge_h;

/**
 * @brief Enumeration for the error codes of nnstreamer-edge (linux standard error, sync with tizen error code).
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

/**
 * @brief Enumeration for the connection type of nnstreamer-edge.
 */
typedef enum {
  NNS_EDGE_CONNECT_TYPE_TCP = 0,
  NNS_EDGE_CONNECT_TYPE_MQTT,
  NNS_EDGE_CONNECT_TYPE_HYBRID,
  NNS_EDGE_CONNECT_TYPE_CUSTOM,

  NNS_EDGE_CONNECT_TYPE_UNKNOWN
} nns_edge_connect_type_e;

/**
 * @brief Enumeration for the node type of nnstreamer-edge.
 */
typedef enum {
  NNS_EDGE_NODE_TYPE_QUERY_CLIENT = 0,
  NNS_EDGE_NODE_TYPE_QUERY_SERVER,
  NNS_EDGE_NODE_TYPE_PUB,
  NNS_EDGE_NODE_TYPE_SUB,

  NNS_EDGE_NODE_TYPE_UNKNOWN,
} nns_edge_node_type_e;

/**
 * @brief Create a handle representing an instance of edge-AI connection between a server and client (query) or a data publisher and scriber.
 * @remarks If the function succeeds, @a edge_h should be released using nns_edge_release_handle().
 * @param[in] id Unique id in local network
 * @param[in] connect_type Value of @a nns_edge_connect_type_e. Connection type between edge nodes.
 * @param[in] node_type Value of @a nns_edge_node_type_e. The node type of edge connection.
 * @param[out] edge_h The edge handle.
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
 * // Create edge handle. Select the connection type and node type. In this example, TCP and query-client are used.
 * ret = nns_edge_create_handle ("TEMP_ID", NNS_EDGE_CONNECT_TYPE_TCP, NNS_EDGE_NODE_QUERY_CLIENT, &edge_h);
 * if (NNS_EDGE_ERROR_NONE != ret)
 *   // Handle error case. After this, handling error case is repeated, so it is omitted.
 *
 * // Information that needs to be set according to the node and connection type.
 * // 1. Query-server-src:
 * //  - All connection type: HOST, PORT, CAPS
 * //  - Hybrid and MQTT: DEST_HOST, DEST_PORT, TOPIC
 * // 2. Query-server-sink:
 * //  - All connection type: CAPS
 * // 3. Query-client
 * //  - All connection type: HOST, PORT, CAPS
 * //  - Hybrid and MQTT: TOPIC
 * // 4. Stream-sink
 * //  - All connection type: HOST, PORT
 * //  - Hybrid and MQTT: DEST_HOST, DEST_PORT, TOPIC
 * // 5. Stream-src
 * //  - All connection type: HOST, PORT
 * //  - Hybrid and MQTT: DEST_HOST, DEST_PORT, TOPIC
 *
 * // Set information for query-client of TCP connection.
 * ret = nns_edge_set_info (edge_h, "HOST", "127.0.0.1");
 * ret = nns_edge_set_info (edge_h, "PORT", "1234");
 * ret = nns_edge_set_info (edge_h, "CAPS", "CAPABILITY_STRING");
 *
 * // Set event callback to receive the new data or capability.
 * // Query-server, query-client and stream-src need to set event callback.
 * nns_edge_set_event_callback (src->edge_h, _nns_edge_event_cb, src);
 *
 * // Start edge handle.
 * ret = nns_edge_start (edge_h);
 *
 * // Do something...
 *
 * // Release edge handle.
 * nns_edge_release_handle (edge_h);
 */
int nns_edge_create_handle (const char *id, nns_edge_connect_type_e connect_type, nns_edge_node_type_e node_type, nns_edge_h *edge_h);

/**
 * @brief Create edge custom handle.
 * @remarks If the function succeeds, @a edge_h should be released using nns_edge_release_handle().
 * @param[in] id Unique id in local network
 * @param[in] lib_path The library path implementing the custom connection.
 * @param[in] node_type Value of @a nns_edge_node_type_e. The node type of edge connection.
 * @param[out] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_OUT_OF_MEMORY Failed to allocate required memory.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_custom_create_handle (const char *id, const char *lib_path, nns_edge_node_type_e node_type, nns_edge_h *edge_h);

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
 * @brief Stop the nnstreamer edges.
 * @param[in] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_stop (nns_edge_h edge_h);

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
 * @brief Discovery connectable devices within the network.
 * @param[in] edge_h The edge handle.
 * @param[in] user_data The user's custom data passed to discovery callback.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 */
int nns_edge_discovery (nns_edge_h edge_h, void *user_data);

/**
 * @brief Connect to the destination node. In the case of Hybrid and MQTT, the TOPIC, DEST_HOST and DEST_PORT must be set before connection using nns_edge_set_info().
 * @param[in] edge_h The edge handle.
 * @param[in] dest_host IP address to connect. In case of TCP connection, it is the IP address of the destination node, and in the case of Hybrid or MQTT connection, it is the IP of the broker.
 * @param[in] dest_port The network port to connect. In case of TCP connection, it is the port of the destination node, and in the case of Hybrid or MQTT connection, it is the port of the broker.
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
 * // [Optional] Set client ID to data handle. If client_id is not given, send data to all connected edge nodes.
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
 * @brief Send data to destination (broker or connected node), asynchronously. If client_id is not set in data_h, send data to all connected edge nodes. To set client_id in data_h, use nns_edge_data_set_info().
 * @param[in] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_NOT_SUPPORTED Not supported.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_IO Failed to transfer the data.
 */
int nns_edge_send (nns_edge_h edge_h, nns_edge_data_h data_h);

/**
 * @brief Check whether edge is connected or not.
 * @param[in] edge_h The edge handle.
 * @return 0 on success. Otherwise a negative error value.
 * @retval #NNS_EDGE_ERROR_NONE Successful.
 * @retval #NNS_EDGE_ERROR_INVALID_PARAMETER Given parameter is invalid.
 * @retval #NNS_EDGE_ERROR_IO No available connection.
 */
int nns_edge_is_connected (nns_edge_h edge_h);

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
 * CAPS or CAPABILITY   | capability strings.
 * IP or HOST           | IP address of the node to accept connection from other node.
 * PORT                 | Port of the node to accept connection from other node. The value should be 0 or higher, if the port is set to 0 then the available port is allocated.
 * DEST_IP or DEST_HOST | IP address of the destination node. In case of TCP connection, it is the IP address of the destination node, and in the case of Hybrid or MQTT connection, it is the IP address of the broker.
 * DEST_PORT            | Port of the destination node. In case of TCP connection, it is the port number of the destination node, and in the case of Hybrid or MQTT connection, it is the port number of the broker. The value should be 0 or higher.
 * TOPIC                | Topic used to publish/subscribe to/from the broker.
 * QUEUE_SIZE           | Max number of data in the queue, when sending edge data to other node. Default 0 means unlimited. N:<leaky [NEW, OLD]> where leaky 'OLD' drops old buffer (default NEW). (e.g., QUEUE_SIZE=5:OLD drops old buffer and pushes new data when queue size reaches 5.)
 * ID or CLIENT_ID      | Unique identifier of the edge handle or client ID. (Read-only)
 */
int nns_edge_set_info (nns_edge_h edge_h, const char *key, const char *value);

/**
 * @brief Get nnstreamer edge info.
 * @remarks If the function succeeds, @a value should be released using free().
 * @note The param key is case-insensitive.
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
 * @brief Get the version of nnstreamer-edge.
 * @param[out] major MAJOR.minor.micro, won't set if it's null.
 * @param[out] minor major.MINOR.micro, won't set if it's null.
 * @param[out] micro major.minor.MICRO, won't set if it's null.
 */
void nns_edge_get_version (unsigned int *major, unsigned int *minor, unsigned int *micro);

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* __NNSTREAMER_EDGE_H__ */
