/**
 * @file        unittest_nnstreamer-edge-custom.cc
 * @date        20 Aug 2024
 * @brief       Unittest for nnstreamer-edge custom connection.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Gichan Jang <gichan2.jang@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
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

  ret = nns_edge_custom_create_handle ("temp_id", "libnnstreamer-edge-custom-test.so",
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &edge_h);
  ASSERT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, NULL);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_set_info (edge_h, "PEER_ADDRESS", "TE:MP:AD:DR:ES:SS");
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_get_info (edge_h, "PEER_ADDRESS", &ret_str);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  EXPECT_STREQ ("TE:MP:AD:DR:ES:SS", ret_str);
  SAFE_FREE (ret_str);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_is_connected (edge_h);
  EXPECT_EQ (NNS_EDGE_ERROR_CONNECTION_FAILURE, ret);

  ret = nns_edge_connect (edge_h, "temp", 3000);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  ret = nns_edge_is_connected (edge_h);
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
