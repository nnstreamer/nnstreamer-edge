/**
 * @file        unittest_nnstreamer-edge-custom.cc
 * @date        20 Aug 2024
 * @brief       Unittest for nnstreamer-edge custom connection.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Gichan Jang <gichan2.jang@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include "nnstreamer-edge-custom-impl.h"
#include "nnstreamer-edge-custom.h"
#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge.h"

/**
 * @brief Create edge custom handle.
 */
TEST (edgeCustom, createHandle)
{
  int ret;
  nns_edge_h edge_h = NULL;
  ret = nns_edge_custom_create_handle ("temp_id", "libnnstreamer-edge-custom-test.so",
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &edge_h);
  ASSERT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Create edge custom handle - invalid param.
 */
TEST (edgeCustom, createHandleInvalidParam01_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_custom_create_handle ("temp-id",
      "libnnstreamer-edge-custom-test.so", NNS_EDGE_NODE_TYPE_UNKNOWN, &edge_h);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Create edge custom handle - invalid param.
 */
TEST (edgeCustom, createHandleInvalidParam02_n)
{
  int ret;

  ret = nns_edge_custom_create_handle ("temp-id",
      "libnnstreamer-edge-custom-test.so", NNS_EDGE_NODE_TYPE_QUERY_SERVER, NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Create edge custom handle - invalid param.
 */
TEST (edgeCustom, createHandleInvalidParam03_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_custom_create_handle (
      "temp-id", NULL, NNS_EDGE_NODE_TYPE_QUERY_SERVER, &edge_h);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Edge event callback for test.
 */
static int
_test_edge_event_cb (nns_edge_event_h event_h, void *user_data)
{
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  int ret;
  int *device_found = (int *) user_data;

  ret = nns_edge_event_get_type (event_h, &event);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  switch (event) {
    case NNS_EDGE_EVENT_DEVICE_FOUND:
      (*device_found)++;
      break;
    default:
      break;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Check the return value of custom connection.
 */
TEST (edgeCustom, expectedReturn)
{
  int ret;
  nns_edge_h edge_h = NULL;
  nns_edge_data_h data_h = NULL;
  char *ret_str = NULL;
  int device_found = 0;

  ret = nns_edge_custom_create_handle ("temp_id", "libnnstreamer-edge-custom-test.so",
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &edge_h);
  ASSERT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, &device_found);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_set_info (edge_h, "PEER_ADDRESS", "TE:MP:AD:DR:ES:SS");
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_get_info (edge_h, "PEER_ADDRESS", &ret_str);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  EXPECT_STREQ ("TE:MP:AD:DR:ES:SS", ret_str);
  SAFE_FREE (ret_str);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_start_discovery (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  EXPECT_EQ (1, device_found);

  ret = nns_edge_stop_discovery (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_is_connected (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_CONNECTION_FAILURE, ret);

  ret = nns_edge_connect (edge_h, "temp", 3000);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_is_connected (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_disconnect (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_is_connected (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_CONNECTION_FAILURE, ret);

  ret = nns_edge_connect (edge_h, "temp", 3000);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_send (edge_h, data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_stop (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_is_connected (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_CONNECTION_FAILURE, ret);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Load edge custom - invalid param.
 */
TEST (edgeCustom, loadInvalidParam01_n)
{
  int ret;
  nns_edge_custom_connection_h handle;

  ret = nns_edge_custom_load (NULL, &handle);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Load edge custom - invalid param.
 */
TEST (edgeCustom, loadInvalidParam02_n)
{
  int ret;
  nns_edge_custom_connection_h handle;

  ret = nns_edge_custom_load ("", &handle);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Load edge custom - invalid param.
 */
TEST (edgeCustom, loadInvalidParam03_n)
{
  int ret;

  ret = nns_edge_custom_load ("libnnstreamer-edge-custom-test.so", NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Release edge custom - invalid param.
 */
TEST (edgeCustom, releaseInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_release (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Start edge custom - invalid param.
 */
TEST (edgeCustom, startInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_start (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Stop edge custom - invalid param.
 */
TEST (edgeCustom, stopInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_stop (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Set event callback of edge custom - invalid param.
 */
TEST (edgeCustom, startDiscoveryInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_start_discovery (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Set event callback of edge custom - invalid param.
 */
TEST (edgeCustom, stopDiscoveryInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_stop_discovery (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Set event callback of edge custom - invalid param.
 */
TEST (edgeCustom, setEventCbInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_set_event_callback (NULL, NULL, NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Connect edge custom - invalid param.
 */
TEST (edgeCustom, connectInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_connect (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Disconnect edge custom - invalid param.
 */
TEST (edgeCustom, disconnectInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_disconnect (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Check connection of edge custom - invalid param.
 */
TEST (edgeCustom, isConnectedInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_is_connected (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Send data using edge custom - invalid param.
 */
TEST (edgeCustom, sendDataInvalidParam01_n)
{
  int ret;
  nns_edge_data_h data_h;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_send_data (NULL, data_h);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Send data using edge custom - invalid param.
 */
TEST (edgeCustom, sendDataInvalidParam02_n)
{
  int ret;
  nns_edge_custom_connection_h handle;

  ret = nns_edge_custom_load ("libnnstreamer-edge-custom-test.so", &handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_send_data (handle, NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_release (handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Set info using edge custom - invalid param.
 */
TEST (edgeCustom, setInfoInvalidParam01_n)
{
  int ret;

  ret = nns_edge_custom_set_info (NULL, "test-key", "test-value");
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Set info using edge custom - invalid param.
 */
TEST (edgeCustom, setInfoInvalidParam02_n)
{
  int ret;
  nns_edge_custom_connection_h handle;

  ret = nns_edge_custom_load ("libnnstreamer-edge-custom-test.so", &handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_set_info (handle, NULL, "test-value");
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_custom_set_info (handle, "", "test-value");
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_release (handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Set info using edge custom - invalid param.
 */
TEST (edgeCustom, setInfoInvalidParam03_n)
{
  int ret;
  nns_edge_custom_connection_h handle;

  ret = nns_edge_custom_load ("libnnstreamer-edge-custom-test.so", &handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_set_info (handle, "test-key", NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_custom_set_info (handle, "test-key", "");
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_release (handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Get info using edge custom - invalid param.
 */
TEST (edgeCustom, getInfoInvalidParam01_n)
{
  int ret;
  char *value;

  ret = nns_edge_custom_get_info (NULL, "test-key", &value);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Get info using edge custom - invalid param.
 */
TEST (edgeCustom, getInfoInvalidParam02_n)
{
  int ret;
  char *value;
  nns_edge_custom_connection_h handle;

  ret = nns_edge_custom_load ("libnnstreamer-edge-custom-test.so", &handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_get_info (handle, NULL, &value);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_custom_get_info (handle, "", &value);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_release (handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Get info using edge custom - invalid param.
 */
TEST (edgeCustom, getInfoInvalidParam03_n)
{
  int ret;
  nns_edge_custom_connection_h handle;

  ret = nns_edge_custom_load ("libnnstreamer-edge-custom-test.so", &handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_get_info (handle, "test-key", NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_custom_release (handle);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Main gtest
 */
int
main (int argc, char **argv)
{
  int result = -1;

  try {
    testing::InitGoogleTest (&argc, argv);
  } catch (...) {
    nns_edge_loge ("Catch exception, failed to init google test.");
  }

  try {
    result = RUN_ALL_TESTS ();
  } catch (...) {
    nns_edge_loge ("Catch exception, failed to run the unittest.");
  }

  return result;
}
