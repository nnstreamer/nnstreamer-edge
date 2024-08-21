/**
 * @file        unittest_nnstreamer-edge.cc
 * @date        27 June 2022
 * @brief       Unittest for nnstreamer-edge library.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Jaeyun Jung <jy1210.jung@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include "nnstreamer-edge.h"
#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"
#include "nnstreamer-edge-metadata.h"
#include "nnstreamer-edge-mqtt.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge-queue.h"

/**
 * @brief Data struct for unittest.
 */
typedef struct
{
  nns_edge_h handle;
  bool running;
  bool is_server;
  bool event_cb_released;
  unsigned int received;
} ne_test_data_s;

/**
 * @brief Allocate and initialize test data.
 */
static ne_test_data_s *
_get_test_data (bool is_server)
{
  ne_test_data_s *_td;

  _td = (ne_test_data_s *) calloc (1, sizeof (ne_test_data_s));

  if (_td) {
    _td->is_server = is_server;
  }

  return _td;
}

/**
 * @brief Release test data.
 */
static void
_free_test_data (ne_test_data_s *_td)
{
  if (!_td)
    return;

  SAFE_FREE (_td);
}

/**
 * @brief Edge event callback for test.
 */
static int
_test_edge_event_cb (nns_edge_event_h event_h, void *user_data)
{
  ne_test_data_s *_td = (ne_test_data_s *) user_data;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  char *val;
  unsigned int i, count;
  int ret;

  if (!_td) {
    /* Cannot update event status. */
    return NNS_EDGE_ERROR_NONE;
  }

  ret = nns_edge_event_get_type (event_h, &event);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  switch (event) {
    case NNS_EDGE_EVENT_CALLBACK_RELEASED:
      _td->event_cb_released = true;
      break;
    case NNS_EDGE_EVENT_NEW_DATA_RECEIVED:
      _td->received++;

      ret = nns_edge_event_parse_new_data (event_h, &data_h);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

      /* Compare metadata */
      ret = nns_edge_data_get_info (data_h, "test-key1", &val);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      EXPECT_STREQ (val, "test-value1");
      SAFE_FREE (val);
      ret = nns_edge_data_get_info (data_h, "test-key2", &val);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      EXPECT_STREQ (val, "test-value2");
      SAFE_FREE (val);

      if (_td->is_server) {
        /**
         * @note This is test code, responding to client.
         * Recommend not to call edge API in event callback.
         */
        ret = nns_edge_send (_td->handle, data_h);
        EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      } else {
        /* Compare received data */
        ret = nns_edge_data_get_count (data_h, &count);
        EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
        ret = nns_edge_data_get (data_h, 0, &data, &data_len);
        EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

        EXPECT_EQ (count, 1U);
        for (i = 0; i < 10U; i++)
          EXPECT_EQ (((unsigned int *) data)[i], i);
      }

      ret = nns_edge_data_destroy (data_h);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      break;
    default:
      break;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Connect to local host, multiple clients.
 */
TEST(edge, connectLocal)
{
  nns_edge_h server_h, client1_h, client2_h;
  ne_test_data_s *_td_server, *_td_client1, *_td_client2;
  nns_edge_data_h data_h;
  nns_size_t data_len;
  void *data;
  unsigned int i, retry;
  int ret, port;
  char *val, *client1_id, *client2_id;

  _td_server = _get_test_data (true);
  _td_client1 = _get_test_data (false);
  _td_client2 = _get_test_data (false);
  ASSERT_TRUE (_td_server != NULL && _td_client1 != NULL && _td_client2 != NULL);
  port = nns_edge_get_available_port ();

  /* Prepare server (127.0.0.1:port) */
  val = nns_edge_strdup_printf ("%d", port);
  nns_edge_create_handle ("temp-server", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &server_h);
  nns_edge_set_event_callback (server_h, _test_edge_event_cb, _td_server);
  nns_edge_set_info (server_h, "IP", "127.0.0.1");
  nns_edge_set_info (server_h, "PORT", val);
  nns_edge_set_info (server_h, "CAPS", "test server");
  nns_edge_set_info (server_h, "QUEUE_SIZE", "10:OLD");
  _td_server->handle = server_h;
  SAFE_FREE (val);

  /* Prepare client */
  nns_edge_create_handle ("temp-client1", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client1_h);
  nns_edge_set_event_callback (client1_h, _test_edge_event_cb, _td_client1);
  nns_edge_set_info (client1_h, "CAPS", "test client1");
  _td_client1->handle = client1_h;

  nns_edge_create_handle ("temp-client2", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client2_h);
  nns_edge_set_event_callback (client2_h, _test_edge_event_cb, _td_client2);
  nns_edge_set_info (client2_h, "CAPS", "test client2");
  _td_client2->handle = client2_h;

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client1_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client2_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_connect (client1_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  usleep (10000);
  ret = nns_edge_connect (client2_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  sleep (2);

  /* Send request to server */
  data_len = 10U * sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  client1_id = client2_id = NULL;
  ret = nns_edge_get_info (client1_h, "client_id", &client1_id);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_get_info (client2_h, "client_id", &client2_id);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 10U; i++)
    ((unsigned int *) data)[i] = i;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  /* For metadata test */
  ret = nns_edge_data_set_info (data_h, "test-key1", "test-value1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_set_info (data_h, "test-key2", "test-value2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 5U; i++) {
    ret = nns_edge_data_set_info (data_h, "client_id", client1_id);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    ret = nns_edge_send (client1_h, data_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    usleep (10000);
    ret = nns_edge_data_set_info (data_h, "client_id", client2_id);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    ret = nns_edge_send (client2_h, data_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

    usleep (100000);
  }

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Wait for responding data (20 seconds) */
  retry = 0U;
  do {
    usleep (100000);
    if (_td_client1->received > 0 && _td_client2->received > 0)
      break;
  } while (retry++ < 200U);

  ret = nns_edge_disconnect (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (client1_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (client2_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  EXPECT_TRUE (_td_server->received > 0);
  EXPECT_TRUE (_td_client1->received > 0);
  EXPECT_TRUE (_td_client2->received > 0);

  SAFE_FREE (client1_id);
  SAFE_FREE (client2_id);

  _free_test_data (_td_server);
  _free_test_data (_td_client1);
  _free_test_data (_td_client2);
}

/**
 * @brief Create edge handle - invalid param.
 */
TEST(edge, createHandleInvalidParam01_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_UNKNOWN,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Create edge handle - invalid param.
 */
TEST(edge, createHandleInvalidParam02_n)
{
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Create edge handle - invalid param.
 */
TEST(edge, createHandleInvalidParam03_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_HYBRID,
      NNS_EDGE_NODE_TYPE_UNKNOWN, &edge_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Start - invalid param.
 */
TEST(edge, startInvalidParam01_n)
{
  int ret;

  ret = nns_edge_start (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Start - invalid param.
 */
TEST(edge, startInvalidParam02_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_start (edge_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Release edge handle - invalid param.
 */
TEST(edge, releaseHandleInvalidParam01_n)
{
  int ret;

  ret = nns_edge_release_handle (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Release edge handle - invalid param.
 */
TEST(edge, releaseHandleInvalidParam02_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set event callback - null param to clear event callback.
 */
TEST(edge, setEventCbSetNullCallback)
{
  nns_edge_h edge_h;
  ne_test_data_s *_td;
  int ret;

  _td = _get_test_data (false);
  ASSERT_TRUE (_td != NULL);

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, _td);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Set null param to clear event callback. */
  ret = nns_edge_set_event_callback (edge_h, NULL, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  EXPECT_TRUE (_td->event_cb_released);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _free_test_data (_td);
}

/**
 * @brief Set event callback - invalid param.
 */
TEST(edge, setEventCbInvalidParam01_n)
{
  int ret;

  ret = nns_edge_set_event_callback (NULL, _test_edge_event_cb, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set event callback - invalid param.
 */
TEST(edge, setEventCbInvalidParam02_n)
{
  nns_edge_h edge_h;
  ne_test_data_s *_td;
  int ret;

  _td = _get_test_data (false);
  ASSERT_TRUE (_td != NULL);

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, _td);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _free_test_data (_td);
}

/**
 * @brief Connect - invalid param.
 */
TEST(edge, connectInvalidParam01_n)
{
  int ret;

  ret = nns_edge_connect (NULL, "127.0.0.1", 80);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect - invalid param.
 */
TEST(edge, connectInvalidParam02_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_connect (edge_h, "127.0.0.1", 80);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect - invalid param.
 */
TEST(edge, connectInvalidParam03_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_connect (edge_h, NULL, 80);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect - invalid param.
 */
TEST(edge, connectInvalidParam04_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_connect (edge_h, "", 80);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect - invalid param.
 */
TEST(edge, connectInvalidParam05_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Invalid port number */
  ret = nns_edge_connect (edge_h, "127.0.0.1", -1);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", 0);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", 77777);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect - invalid param.
 */
TEST(edge, connectInvalidParam06_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_event_callback (edge_h, _test_edge_event_cb, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* not started */
  ret = nns_edge_connect (edge_h, "127.0.0.1", 80);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Disconnect - invalid param.
 */
TEST(edge, disconnectInvalidParam01_n)
{
  int ret;

  ret = nns_edge_disconnect (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Disconnect - invalid param.
 */
TEST(edge, disconnectInvalidParam02_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_disconnect (edge_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Send - invalid param.
 */
TEST(edge, sendInvalidParam01_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "client_id", "10");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_send (NULL, data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Send - invalid param.
 */
TEST(edge, sendInvalidParam02_n)
{
  nns_edge_h edge_h;
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "client_id", "10");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_send (edge_h, data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Send - invalid param.
 */
TEST(edge, sendInvalidParam03_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_send (edge_h, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam01_n)
{
  int ret;

  ret = nns_edge_set_info (NULL, "caps", "temp-caps");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam02_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_set_info (edge_h, "caps", "temp-caps");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam03_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (edge_h, NULL, "temp-caps");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam04_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (edge_h, "", "temp-caps");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam05_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (edge_h, "caps", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam06_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (edge_h, "caps", "");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam07_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Not allowed key */
  ret = nns_edge_set_info (edge_h, "id", "temp-id2");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "client_id", "temp-cid");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam08_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Invalid port number */
  ret = nns_edge_set_info (edge_h, "port", "-1");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "port", "77777");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST(edge, setInfoInvalidParam09_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Invalid option */
  ret = nns_edge_set_info (edge_h, "QUEUE_SIZE", "15:INVALID_LEAKY");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info.
 */
TEST(edge, getInfo)
{
  nns_edge_h edge_h;
  char *value = NULL;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (edge_h, "capability", "capa-for-test");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "topic", "topic-for-test");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "ip", "165.213.201.100");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "port", "2000");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "dest_ip", "165.213.201.101");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "dest_port", "2001");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "temp-key1", "temp-value1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "temp-key2", "temp-value2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_get_info (edge_h, "ID", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-id");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "capability", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "capa-for-test");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "topic", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "topic-for-test");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "ip", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "165.213.201.100");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "port", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "2000");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "dest_ip", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "165.213.201.101");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "dest_port", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "2001");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "temp-key1", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value1");
  SAFE_FREE (value);

  ret = nns_edge_get_info (edge_h, "temp-key2", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value2");
  SAFE_FREE (value);

  /* Replace old value */
  ret = nns_edge_set_info (edge_h, "temp-key2", "temp-value2-replaced");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_get_info (edge_h, "temp-key2", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value2-replaced");
  SAFE_FREE (value);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info - invalid param.
 */
TEST(edge, getInfoInvalidParam01_n)
{
  char *value = NULL;
  int ret;

  ret = nns_edge_get_info (NULL, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info - invalid param.
 */
TEST(edge, getInfoInvalidParam02_n)
{
  nns_edge_h edge_h;
  char *value;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_get_info (edge_h, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (edge_h, NNS_EDGE_MAGIC);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info - invalid param.
 */
TEST(edge, getInfoInvalidParam03_n)
{
  nns_edge_h edge_h;
  char *value = NULL;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_get_info (edge_h, NULL, &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info - invalid param.
 */
TEST(edge, getInfoInvalidParam04_n)
{
  nns_edge_h edge_h;
  char *value = NULL;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_get_info (edge_h, "", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info - invalid param.
 */
TEST(edge, getInfoInvalidParam05_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_get_info (edge_h, "temp-key", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info - invalid param.
 */
TEST(edge, getInfoInvalidParam06_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Cannot get the client ID if handle is server */
  ret = nns_edge_get_info (edge_h, "client_id", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Clear info of edge-data - invalid param.
 */
TEST(edgeData, clearInfoInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_clear_info (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Clear info of edge-data - invalid param.
 */
TEST(edgeData, clearInfoInvalidParam02_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_clear_info (data_h);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Clear info of edge data.
 */
TEST(edgeData, clearInfo)
{
  nns_edge_data_h data_h;
  char *value = NULL;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_get_info (data_h, "temp-key", &value);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  EXPECT_STREQ ("temp-value", value);
  SAFE_FREE (value);

  ret = nns_edge_data_clear_info (data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_get_info (data_h, "temp-key", &value);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_get_info (data_h, "temp-key", &value);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  EXPECT_STREQ ("temp-value", value);
  SAFE_FREE (value);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Create edge-data - invalid param.
 */
TEST(edgeData, createInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_create (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge-data - invalid param.
 */
TEST(edgeData, destroyInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_destroy (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge-data - invalid param.
 */
TEST(edgeData, destroyInvalidParam02_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Validate edge-data.
 */
TEST(edgeData, validate)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_is_valid (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Validate edge-data - invalid param.
 */
TEST(edgeData, validateInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_is_valid (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Validate edge-data - invalid param.
 */
TEST(edgeData, validateInvalidParam02_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_is_valid (data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge-data.
 */
TEST(edgeData, copy)
{
  nns_edge_data_h src_h, desc_h;
  void *data, *result;
  nns_size_t data_len, result_len;
  char *result_value;
  unsigned int i, result_count;
  int ret;

  data_len = 10U * sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  for (i = 0; i < 10U; i++)
    ((unsigned int *) data)[i] = i;

  ret = nns_edge_data_create (&src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (src_h, "temp-key1", "temp-data-val1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_set_info (src_h, "temp-key2", "temp-data-val2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (src_h, data, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_copy (src_h, &desc_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Compare data and info */
  ret = nns_edge_data_get_count (desc_h, &result_count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result_count, 1U);

  ret = nns_edge_data_get (desc_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  for (i = 0; i < 10U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i);

  ret = nns_edge_data_get_info (desc_h, "temp-key1", &result_value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (result_value, "temp-data-val1");
  SAFE_FREE (result_value);

  ret = nns_edge_data_get_info (desc_h, "temp-key2", &result_value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (result_value, "temp-data-val2");
  SAFE_FREE (result_value);

  ret = nns_edge_data_destroy (desc_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge-data - invalid param.
 */
TEST(edgeData, copyInvalidParam01_n)
{
  nns_edge_data_h desc_h;
  int ret;

  ret = nns_edge_data_copy (NULL, &desc_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge-data - invalid param.
 */
TEST(edgeData, copyInvalidParam02_n)
{
  nns_edge_data_h src_h, desc_h;
  int ret;

  ret = nns_edge_data_create (&src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (src_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_copy (src_h, &desc_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (src_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge-data - invalid param.
 */
TEST(edgeData, copyInvalidParam03_n)
{
  nns_edge_data_h src_h;
  int ret;

  ret = nns_edge_data_create (&src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_copy (src_h, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Add edge-data - max data limit.
 */
TEST(edgeData, addMaxData_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int i, ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  for (i = 0; i < NNS_EDGE_DATA_LIMIT; i++) {
    ret = nns_edge_data_add (data_h, data, data_len, NULL);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  }

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Add edge-data - invalid param.
 */
TEST(edgeData, addInvalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_add (NULL, data, data_len, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Add edge-data - invalid param.
 */
TEST(edgeData, addInvalidParam02_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Add edge-data - invalid param.
 */
TEST(edgeData, addInvalidParam03_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, NULL, data_len, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Add edge-data - invalid param.
 */
TEST(edgeData, addInvalidParam04_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, 0, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get edge-data.
 */
TEST(edgeData, get)
{
  nns_edge_data_h data_h;
  void *data, *result;
  nns_size_t data_len, result_len;
  unsigned int count;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (count, 1U);

  ret = nns_edge_data_get (data_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result, data);
  EXPECT_EQ (result_len, data_len);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get edge-data - invalid param.
 */
TEST(edgeData, getInvalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_get (NULL, 0, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge-data - invalid param.
 */
TEST(edgeData, getInvalidParam02_n)
{
  nns_edge_data_h data_h;
  void *data, *result;
  nns_size_t data_len, result_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_get (data_h, 0, &result, &result_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get edge-data - invalid param.
 */
TEST(edgeData, getInvalidParam03_n)
{
  nns_edge_data_h data_h;
  void *data, *result;
  nns_size_t data_len, result_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Invalid index */
  ret = nns_edge_data_get (data_h, 1, &result, &result_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get edge-data - invalid param.
 */
TEST(edgeData, getInvalidParam04_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len, result_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get (data_h, 0, NULL, &result_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get edge-data - invalid param.
 */
TEST(edgeData, getInvalidParam05_n)
{
  nns_edge_data_h data_h;
  void *data, *result;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get (data_h, 0, &result, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get count of edge-data - invalid param.
 */
TEST(edgeData, getCountInvalidParam01_n)
{
  unsigned int count;
  int ret;

  ret = nns_edge_data_get_count (NULL, &count);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get count of edge-data - invalid param.
 */
TEST(edgeData, getCountInvalidParam02_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  unsigned int count;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get count of edge-data - invalid param.
 */
TEST(edgeData, getCountInvalidParam03_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_count (data_h, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Set info of edge-data - invalid param.
 */
TEST(edgeData, setInfoInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_set_info (NULL, "temp-key", "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info of edge-data - invalid param.
 */
TEST(edgeData, setInfoInvalidParam02_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info of edge-data - invalid param.
 */
TEST(edgeData, setInfoInvalidParam03_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, NULL, "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info of edge-data - invalid param.
 */
TEST(edgeData, setInfoInvalidParam04_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info of edge-data - invalid param.
 */
TEST(edgeData, getInfoInvalidParam01_n)
{
  char *value = NULL;
  int ret;

  ret = nns_edge_data_get_info (NULL, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info of edge-data - invalid param.
 */
TEST(edgeData, getInfoInvalidParam02_n)
{
  nns_edge_data_h data_h;
  char *value = NULL;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_get_info (data_h, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info of edge-data - invalid param.
 */
TEST(edgeData, getInfoInvalidParam03_n)
{
  nns_edge_data_h data_h;
  char *value = NULL;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_info (data_h, NULL, &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info of edge-data - invalid param.
 */
TEST(edgeData, getInfoInvalidParam04_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_info (data_h, "temp-key", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info of edge-data - invalid param.
 */
TEST(edgeData, getInfoInvalidParam05_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_info (data_h, "", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize meta in edge-data - invalid param.
 */
TEST(edgeData, serializeInvalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_serialize_meta (NULL, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize meta in edge-data - invalid param.
 */
TEST(edgeData, serializeInvalidParam02_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_serialize_meta (data_h, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize meta in edge-data - invalid param.
 */
TEST(edgeData, serializeInvalidParam03_n)
{
  nns_edge_data_h data_h;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (data_h, NULL, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize meta in edge-data - invalid param.
 */
TEST(edgeData, serializeInvalidParam04_n)
{
  nns_edge_data_h data_h;
  void *data;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (data_h, &data, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Deserialize meta to edge-data - invalid param.
 */
TEST(edgeData, deserializeInvalidParam01_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize_meta (NULL, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize meta to edge-data - invalid param.
 */
TEST(edgeData, deserializeInvalidParam02_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_deserialize_meta (data_h, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize meta to edge-data - invalid param.
 */
TEST(edgeData, deserializeInvalidParam03_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize_meta (data_h, NULL, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize meta to edge-data - invalid param.
 */
TEST(edgeData, deserializeInvalidParam04_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize_meta (data_h, data, 0);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Serialize and deserialize the edge-data.
 */
TEST(edgeDataSerialize, normal)
{
  nns_edge_data_h src_h, dest_h;
  void *data1, *data2, *result, *serialized_data;
  nns_size_t data_len, result_len, serialized_len;
  char *result_value;
  unsigned int i, result_count;
  int ret;

  data_len = 10U * sizeof (unsigned int);
  data1 = malloc (data_len);
  ASSERT_TRUE (data1 != NULL);

  for (i = 0; i < 10U; i++)
    ((unsigned int *) data1)[i] = i;

  data2 = malloc (data_len * 2);
  ASSERT_TRUE (data2 != NULL);

  for (i = 0; i < 20U; i++)
    ((unsigned int *) data2)[i] = 20 - i;

  ret = nns_edge_data_create (&src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (src_h, "temp-key1", "temp-data-val1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_set_info (src_h, "temp-key2", "temp-data-val2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (src_h, data1, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (src_h, data2, data_len * 2, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (src_h, &serialized_data, &serialized_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_create (&dest_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (dest_h, serialized_data, serialized_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Compare data and info */
  ret = nns_edge_data_get_count (dest_h, &result_count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result_count, 2U);

  ret = nns_edge_data_get (dest_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  for (i = 0; i < 10U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i);

  ret = nns_edge_data_get (dest_h, 1, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  for (i = 0; i < 20U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], 20 - i);

  ret = nns_edge_data_get_info (dest_h, "temp-key1", &result_value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (result_value, "temp-data-val1");
  SAFE_FREE (result_value);

  ret = nns_edge_data_get_info (dest_h, "temp-key2", &result_value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (result_value, "temp-data-val2");
  SAFE_FREE (result_value);

  ret = nns_edge_data_destroy (dest_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (serialized_data);
}

/**
 * @brief Serialize edge-data - invalid param.
 */
TEST(edgeDataSerialize, invalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_serialize (NULL, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize edge-data - invalid param.
 */
TEST(edgeData, invalidParam02_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_serialize (data_h, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Clear raw data in edge-data - invalid param.
 */
TEST(edgeData, clearInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_clear (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Clear raw data in edge-data - invalid param.
 */
TEST(edgeData, clearInvalidParam02_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_clear (data_h);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Clear raw data in edge-data.
 */
TEST(edgeData, clear)
{
  nns_edge_data_h data_h;
  void *data, *result;
  nns_size_t data_len, result_len;
  unsigned int count;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_add (data_h, data, data_len, nns_edge_free);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE , ret);
  EXPECT_EQ (1U, count);

  ret = nns_edge_data_clear (data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
  EXPECT_EQ (0U, count);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Serialize edge-data - invalid param.
 */
TEST(edgeDataSerialize, invalidParam03_n)
{
  nns_edge_data_h data_h;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (data_h, NULL, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize edge-data - invalid param.
 */
TEST(edgeDataSerialize, invalidParam04_n)
{
  nns_edge_data_h data_h;
  void *data;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (data_h, &data, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Deserialize edge-data - invalid param.
 */
TEST(edgeDataDeserialize, invalidParam01_n)
{
  void *data = NULL;
  int ret;

  ret = nns_edge_data_deserialize (NULL, data, 10U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge-data - invalid param.
 */
TEST(edgeDataDeserialize, invalidParam02_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_data_deserialize (data_h, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (data_h, NNS_EDGE_MAGIC);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge-data - invalid param.
 */
TEST(edgeDataDeserialize, invalidParam03_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (data_h, NULL, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge-data - invalid param.
 */
TEST(edgeDataDeserialize, invalidParam04_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (data_h, data, 1U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - invalid param.
 */
TEST(edgeDataIsSerialized, invalidParam01_n)
{
  int ret;

  ret = nns_edge_data_is_serialized (NULL, 1U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Util to check serialized data - invalid param.
 */
TEST(edgeDataIsSerialized, invalidParam02_n)
{
  void *data;
  int ret;

  data = nns_edge_malloc (100U);
  ASSERT_TRUE (data != NULL);
  memset (data, 0, 100U);

  /* invalid data key */
  ret = nns_edge_data_is_serialized (data, 100U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - invalid param.
 */
TEST(edgeDataIsSerialized, invalidParam03_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* invalid data size */
  ret = nns_edge_data_is_serialized (data, 1U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Create edge event - invalid param.
 */
TEST(edgeEvent, createInvalidParam01_n)
{
  nns_edge_event_h event_h;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_UNKNOWN, &event_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Create edge event - invalid param.
 */
TEST(edgeEvent, createInvalidParam02_n)
{
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge event - invalid param.
 */
TEST(edgeEvent, destroyInvalidParam01_n)
{
  int ret;

  ret = nns_edge_event_destroy (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge event - invalid param.
 */
TEST(edgeEvent, destroyInvalidParam02_n)
{
  nns_edge_event_h event_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_set_data (event_h, data, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge event data - invalid param.
 */
TEST(edgeEvent, setDataInvalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_event_set_data (NULL, data, data_len, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Set edge event data - invalid param.
 */
TEST(edgeEvent, setDataInvalidParam02_n)
{
  nns_edge_event_h event_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_set_data (event_h, NULL, data_len, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Set edge event data - invalid param.
 */
TEST(edgeEvent, setDataInvalidParam03_n)
{
  nns_edge_event_h event_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_set_data (event_h, data, 0, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Set edge event data - invalid param.
 */
TEST(edgeEvent, setDataInvalidParam04_n)
{
  nns_edge_event_h event_h;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U * sizeof (int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_event_set_data (event_h, data, data_len, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Get edge event type.
 */
TEST(edgeEvent, getType)
{
  nns_edge_event_h event_h;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_get_type (event_h, &event);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (event, NNS_EDGE_EVENT_CUSTOM);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event type - invalid param.
 */
TEST(edgeEvent, getTypeInvalidParam01_n)
{
  nns_edge_event_e event;
  int ret;

  ret = nns_edge_event_get_type (NULL, &event);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event type - invalid param.
 */
TEST(edgeEvent, getTypeInvalidParam02_n)
{
  nns_edge_event_h event_h;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_get_type (event_h, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event type - invalid param.
 */
TEST(edgeEvent, getTypeInvalidParam03_n)
{
  nns_edge_event_h event_h;
  nns_edge_event_e event;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_event_get_type (event_h, &event);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse new data of edge event.
 */
TEST(edgeEvent, parseNewData)
{
  nns_edge_event_h event_h;
  nns_edge_data_h data_h, result_h;
  void *data, *result;
  nns_size_t data_len, result_len;
  char *result_value;
  unsigned int i, count;
  int ret;

  data_len = 10U * sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  for (i = 0; i < 10U; i++)
    ((unsigned int *) data)[i] = i;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (data_h, "temp-key1", "temp-data-val1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_set_info (data_h, "temp-key2", "temp-data-val2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_create (NNS_EDGE_EVENT_NEW_DATA_RECEIVED, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_set_data (event_h, data_h, sizeof (nns_edge_data_h), NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_parse_new_data (event_h, &result_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Compare data and info */
  ret = nns_edge_data_get_count (result_h, &count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (count, 1U);

  ret = nns_edge_data_get (result_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  for (i = 0; i < 10U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i);

  ret = nns_edge_data_get_info (result_h, "temp-key1", &result_value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (result_value, "temp-data-val1");
  SAFE_FREE (result_value);

  ret = nns_edge_data_get_info (result_h, "temp-key2", &result_value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (result_value, "temp-data-val2");
  SAFE_FREE (result_value);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (result_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse new data of edge event - invalid param.
 */
TEST(edgeEvent, parseNewDataInvalidParam01_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_event_parse_new_data (NULL, &data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse new data of edge event - invalid param.
 */
TEST(edgeEvent, parseNewDataInvalidParam02_n)
{
  nns_edge_event_h event_h;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_NEW_DATA_RECEIVED, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_parse_new_data (event_h, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse new data of edge event - invalid param.
 */
TEST(edgeEvent, parseNewDataInvalidParam03_n)
{
  nns_edge_event_h event_h;
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_NEW_DATA_RECEIVED, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_event_parse_new_data (event_h, &data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse new data of edge event - invalid param.
 */
TEST(edgeEvent, parseNewDataInvalidParam04_n)
{
  nns_edge_event_h event_h;
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_parse_new_data (event_h, &data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse capability of edge event.
 */
TEST(edgeEvent, parseCapability)
{
  const char capability[] = "temp-capability";
  nns_edge_event_h event_h;
  char *caps = NULL;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CAPABILITY, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_set_data (event_h, (void *) capability, strlen (capability), NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_parse_capability (event_h, &caps);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (caps, capability);
  SAFE_FREE (caps);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse capability of edge event - invalid param.
 */
TEST(edgeEvent, parseCapabilityInvalidParam01_n)
{
  char *caps = NULL;
  int ret;

  ret = nns_edge_event_parse_capability (NULL, &caps);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse capability of edge event - invalid param.
 */
TEST(edgeEvent, parseCapabilityInvalidParam02_n)
{
  nns_edge_event_h event_h;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CAPABILITY, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_parse_capability (event_h, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse capability of edge event - invalid param.
 */
TEST(edgeEvent, parseCapabilityInvalidParam03_n)
{
  nns_edge_event_h event_h;
  char *caps = NULL;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CAPABILITY, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_event_parse_capability (event_h, &caps);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse capability of edge event - invalid param.
 */
TEST(edgeEvent, parseCapabilityInvalidParam04_n)
{
  nns_edge_event_h event_h;
  char *caps = NULL;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_parse_capability (event_h, &caps);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Create edge metadata - invalid param.
 */
TEST(edgeMeta, createInvalidParam01_n)
{
  int ret;

  ret = nns_edge_metadata_create (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge metadata - invalid param.
 */
TEST(edgeMeta, destroyInvalidParam01_n)
{
  int ret;

  ret = nns_edge_metadata_destroy (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge metadata - invalid param.
 */
TEST(edgeMeta, setInvalidParam01_n)
{
  int ret;

  ret = nns_edge_metadata_set (NULL, "temp-key", "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge metadata - invalid param.
 */
TEST(edgeMeta, setInvalidParam02_n)
{
  nns_edge_metadata_h meta;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (meta, NULL, "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge metadata - invalid param.
 */
TEST(edgeMeta, setInvalidParam03_n)
{
  nns_edge_metadata_h meta;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (meta, "", "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge metadata - invalid param.
 */
TEST(edgeMeta, setInvalidParam04_n)
{
  nns_edge_metadata_h meta;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (meta, "temp-key", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge metadata - invalid param.
 */
TEST(edgeMeta, setInvalidParam05_n)
{
  nns_edge_metadata_h meta;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (meta, "temp-key", "");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge metadata - invalid param.
 */
TEST(edgeMeta, getInvalidParam01_n)
{
  char *value = NULL;
  int ret;

  ret = nns_edge_metadata_get (NULL, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge metadata - invalid param.
 */
TEST(edgeMeta, getInvalidParam02_n)
{
  nns_edge_metadata_h meta;
  char *value = NULL;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (meta, NULL, &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge metadata - invalid param.
 */
TEST(edgeMeta, getInvalidParam03_n)
{
  nns_edge_metadata_h meta;
  char *value = NULL;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (meta, "", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge metadata - invalid param.
 */
TEST(edgeMeta, getInvalidParam04_n)
{
  nns_edge_metadata_h meta;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (meta, "temp-key", NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge metadata.
 */
TEST(edgeMeta, copy)
{
  nns_edge_metadata_h src, desc;
  char *value = NULL;
  int ret;

  ret = nns_edge_metadata_create (&src);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_create (&desc);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (src, "temp-key1", "temp-value1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_set (src, "temp-key2", "temp-value2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Replace old value */
  ret = nns_edge_metadata_set (src, "temp-key2", "temp-value2-replaced");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_copy (desc, src);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (desc, "temp-key1", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value1");
  SAFE_FREE (value);

  ret = nns_edge_metadata_get (desc, "temp-key2", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value2-replaced");
  SAFE_FREE (value);

  ret = nns_edge_metadata_destroy (src);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_destroy (desc);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge metadata - invalid param.
 */
TEST(edgeMeta, copyInvalidParam01_n)
{
  nns_edge_metadata_h src;
  int ret;

  ret = nns_edge_metadata_create (&src);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_copy (NULL, src);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (src);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge metadata - invalid param.
 */
TEST(edgeMeta, copyInvalidParam02_n)
{
  nns_edge_metadata_h desc;
  int ret;

  ret = nns_edge_metadata_create (&desc);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_copy (desc, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (desc);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize edge metadata.
 */
TEST(edgeMeta, serialize)
{
  nns_edge_metadata_h src, desc;
  char *value;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_metadata_create (&src);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_create (&desc);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (src, "temp-key1", "temp-value1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_set (src, "temp-key2", "temp-value2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_set (src, "temp-key3", "temp-value3");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_serialize (src, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (desc, data, data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (desc, "temp-key1", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value1");
  SAFE_FREE (value);

  ret = nns_edge_metadata_get (desc, "temp-key2", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value2");
  SAFE_FREE (value);

  ret = nns_edge_metadata_get (desc, "temp-key3", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value3");
  SAFE_FREE (value);

  ret = nns_edge_metadata_destroy (src);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_destroy (desc);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Serialize edge metadata - invalid param.
 */
TEST(edgeMeta, serializeInvalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_metadata_serialize (NULL, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize edge metadata - invalid param.
 */
TEST(edgeMeta, serializeInvalidParam02_n)
{
  nns_edge_metadata_h meta;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_serialize (meta, NULL, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Serialize edge metadata - invalid param.
 */
TEST(edgeMeta, serializeInvalidParam03_n)
{
  nns_edge_metadata_h meta;
  void *data;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_serialize (meta, &data, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Deserialize edge metadata - invalid param.
 */
TEST(edgeMeta, deserializeInvalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U + sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);
  ((unsigned int *) data)[0] = 0U;

  ret = nns_edge_metadata_deserialize (NULL, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata - invalid param.
 */
TEST(edgeMeta, deserializeInvalidParam02_n)
{
  nns_edge_metadata_h meta;
  nns_size_t data_len;
  int ret;

  data_len = 10U + sizeof (unsigned int);

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, NULL, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Deserialize edge metadata - invalid param.
 */
TEST(edgeMeta, deserializeInvalidParam03_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = 10U + sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);
  ((unsigned int *) data)[0] = 0U;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, 0);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Class to set up and tear down queue testing
 */
class edgeQueue: public ::testing::Test
{
  protected:
    virtual void SetUp() override
    {
      EXPECT_EQ (nns_edge_queue_create (&queue_h), NNS_EDGE_ERROR_NONE);
    }

    virtual void TearDown() override
    {
      EXPECT_EQ (nns_edge_queue_destroy (queue_h), NNS_EDGE_ERROR_NONE);
    }

  protected:
    nns_edge_queue_h queue_h;
};

/**
 * @brief Thread to push new data into queue.
 */
static void *
_test_thread_edge_queue_push (void *thread_data)
{
  nns_edge_queue_h queue_h = thread_data;
  unsigned int i, j;
  void *data;
  nns_size_t dsize;

  for (i = 0; i < 6U; i++) {
    usleep (50000);

    dsize = 5 * sizeof (unsigned int);
    data = malloc (dsize);
    if (data) {
      for (j = 0; j < 5U; j++)
        ((unsigned int *) data)[j] = i * 10U + j;
    }

    EXPECT_EQ (nns_edge_queue_push (queue_h, data, dsize, nns_edge_free),
      NNS_EDGE_ERROR_NONE);
  }

  return NULL;
}

/**
 * @brief Push and pop data.
 */
TEST_F(edgeQueue, pushData)
{
  void *data1, *data2, *data3, *result;
  nns_size_t dsize, rsize;
  unsigned int i, len;

  dsize = 5 * sizeof (unsigned int);

  data1 = malloc (dsize);
  ASSERT_TRUE (data1 != NULL);
  for (i = 0; i < 5U; i++)
    ((unsigned int *) data1)[i] = i + 10U;

  data2 = malloc (dsize);
  ASSERT_TRUE (data1 != NULL);
  for (i = 0; i < 5U; i++)
    ((unsigned int *) data2)[i] = i + 20U;

  data3 = malloc (dsize);
  ASSERT_TRUE (data1 != NULL);
  for (i = 0; i < 5U; i++)
    ((unsigned int *) data3)[i] = i + 30U;

  EXPECT_EQ (nns_edge_queue_push (queue_h, data1, dsize, NULL), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 1U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data2, dsize, NULL), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 2U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data3, dsize, NULL), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 3U);

  rsize = 0U;
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &result, &rsize), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 2U);
  EXPECT_EQ (result, data1);
  EXPECT_EQ (dsize, rsize);
  for (i = 0; i < 5U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i + 10U);

  rsize = 0U;
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &result, &rsize), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 1U);
  EXPECT_EQ (result, data2);
  EXPECT_EQ (dsize, rsize);
  for (i = 0; i < 5U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i + 20U);

  rsize = 0U;
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &result, &rsize), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 0U);
  EXPECT_EQ (result, data3);
  EXPECT_EQ (dsize, rsize);
  for (i = 0; i < 5U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i + 30U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data1, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 1U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data2, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 2U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data3, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 3U);
}

/**
 * @brief Wait for pushing data.
 */
TEST_F(edgeQueue, pushDataOnThread)
{
  pthread_t push_thread;
  pthread_attr_t attr;
  unsigned int i, j, len, retry;

  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);
  pthread_create (&push_thread, &attr, _test_thread_edge_queue_push, queue_h);
  pthread_attr_destroy (&attr);

  for (i = 0; i < 3U; i++) {
    void *result = NULL;
    nns_size_t rsize = 0U;

    EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 0U, &result, &rsize), NNS_EDGE_ERROR_NONE);

    for (j = 0; j < 5U; j++)
      EXPECT_EQ (((unsigned int *) result)[j], i * 10U + j);
    EXPECT_EQ (rsize, 5 * sizeof (unsigned int));

    SAFE_FREE (result);
  }

  retry = 0U;
  do {
    usleep (20000);
    len = nns_edge_queue_get_length (queue_h);
  } while (len < 3U && retry++ < 200U);
}

/**
 * @brief Create queue - invalid param.
 */
TEST_F(edgeQueue, createInvalidParam01_n)
{
  EXPECT_EQ (nns_edge_queue_create (NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Destroy queue - invalid param.
 */
TEST_F(edgeQueue, destroyInvalidParam01_n)
{
  EXPECT_EQ (nns_edge_queue_destroy (NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Get length of queue - invalid param.
 */
TEST_F(edgeQueue, getLengthInvalidParam01_n)
{
  unsigned int len;

  len = nns_edge_queue_get_length (NULL);
  EXPECT_EQ (len, 0U);
}

/**
 * @brief Set limit of queue.
 */
TEST_F(edgeQueue, setLimit)
{
  void *data;
  nns_size_t dsize;
  unsigned int i, len;

  dsize = sizeof (unsigned int);
  data = malloc (dsize);
  ASSERT_TRUE (data != NULL);

  EXPECT_EQ (nns_edge_queue_set_limit (queue_h, 3U, NNS_EDGE_QUEUE_LEAK_NEW), NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 5U; i++)
    nns_edge_queue_push (queue_h, data, dsize, NULL);

  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 3U);

  SAFE_FREE (data);
}

/**
 * @brief Set leaky option of queue.
 */
TEST_F(edgeQueue, setLeaky)
{
  void *data;
  nns_size_t dsize, rsize;
  unsigned int i, len;
  int ret;

  /* leaky option new */
  EXPECT_EQ (nns_edge_queue_set_limit (queue_h, 3U, NNS_EDGE_QUEUE_LEAK_NEW), NNS_EDGE_ERROR_NONE);

  dsize = sizeof (unsigned int);

  for (i = 0; i < 5U; i++) {
    data = malloc (dsize);
    ASSERT_TRUE (data != NULL);

    *((unsigned int *) data) = i + 1;

    ret = nns_edge_queue_push (queue_h, data, dsize, nns_edge_free);
    if (i < 3U) {
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    } else {
      EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
      SAFE_FREE (data);
    }
  }

  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 3U);

  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (*((unsigned int *) data), 1U);
  SAFE_FREE (data);
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (*((unsigned int *) data), 2U);
  SAFE_FREE (data);
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (*((unsigned int *) data), 3U);
  SAFE_FREE (data);

  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 0U);

  /* leaky option old */
  EXPECT_EQ (nns_edge_queue_set_limit (queue_h, 3U, NNS_EDGE_QUEUE_LEAK_OLD), NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 5U; i++) {
    data = malloc (dsize);
    ASSERT_TRUE (data != NULL);

    *((unsigned int *) data) = i + 1;

    EXPECT_EQ (nns_edge_queue_push (queue_h, data, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  }

  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 3U);

  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (*((unsigned int *) data), 3U);
  SAFE_FREE (data);
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (*((unsigned int *) data), 4U);
  SAFE_FREE (data);
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (*((unsigned int *) data), 5U);
  SAFE_FREE (data);

  len = nns_edge_queue_get_length (queue_h);
  EXPECT_EQ (len, 0U);
}

/**
 * @brief Set limit of queue - invalid param.
 */
TEST_F(edgeQueue, setLimitInvalidParam01_n)
{
  EXPECT_EQ (nns_edge_queue_set_limit (NULL, 5U, NNS_EDGE_QUEUE_LEAK_NEW), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Push data into queue - invalid param.
 */
TEST_F(edgeQueue, pushInvalidParam01_n)
{
  void *data;
  nns_size_t dsize;

  dsize = 5 * sizeof (unsigned int);
  data = malloc (dsize);
  ASSERT_TRUE (data != NULL);

  EXPECT_EQ (nns_edge_queue_push (NULL, data, dsize, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);

  SAFE_FREE (data);
}

/**
 * @brief Push data into queue - invalid param.
 */
TEST_F(edgeQueue, pushInvalidParam02_n)
{
  nns_size_t dsize;

  dsize = 5 * sizeof (unsigned int);
  EXPECT_EQ (nns_edge_queue_push (queue_h, NULL, dsize, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Push data into queue - invalid param.
 */
TEST_F(edgeQueue, pushInvalidParam03_n)
{
  void *data;

  data = malloc (5 * sizeof (unsigned int));
  ASSERT_TRUE (data != NULL);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data, 0U, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);

  SAFE_FREE (data);
}

/**
 * @brief Pop data from queue - invalid param.
 */
TEST_F(edgeQueue, popInvalidParam01_n)
{
  void *data;
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_pop (NULL, &data, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Pop data from queue - invalid param.
 */
TEST_F(edgeQueue, popInvalidParam02_n)
{
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_pop (queue_h, NULL, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Pop data from queue - invalid param.
 */
TEST_F(edgeQueue, popInvalidParam03_n)
{
  void *data;

  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Wait and pop data from queue, timed out.
 */
TEST_F(edgeQueue, waitPopTimedout)
{
  void *data;
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 10U, &data, &size), NNS_EDGE_ERROR_IO);
}

/**
 * @brief Wait and pop data from queue - invalid param.
 */
TEST_F(edgeQueue, waitPopInvalidParam01_n)
{
  void *data;
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_wait_pop (NULL, 0U, &data, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Wait and pop data from queue - invalid param.
 */
TEST_F(edgeQueue, waitPopInvalidParam02_n)
{
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 0U, NULL, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Wait and pop data from queue - invalid param.
 */
TEST_F(edgeQueue, waitPopInvalidParam03_n)
{
  void *data;

  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 0U, &data, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Util to get the version.
 */
TEST(edgeUtil, getVersion)
{
  unsigned int major1, minor1, micro1;
  unsigned int major2, minor2, micro2;
  uint64_t ver_key;
  char *ver_string;

  nns_edge_get_version (&major1, &minor1, &micro1);
  ver_key = nns_edge_generate_version_key ();
  EXPECT_TRUE (nns_edge_parse_version_key (ver_key, &major2, &minor2, &micro2));
  EXPECT_EQ (major1, major2);
  EXPECT_EQ (minor1, minor2);
  EXPECT_EQ (micro1, micro2);

  ver_string = nns_edge_strdup_printf ("%u.%u.%u", major1, minor1, micro1);
  EXPECT_STREQ (ver_string, VERSION);
  nns_edge_free (ver_string);
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
