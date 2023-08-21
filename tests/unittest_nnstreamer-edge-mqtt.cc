/**
 * @file        unittest_nnstreamer-edge-mqtt.cc
 * @date        15 Mar 2023
 * @brief       Unittest for nnstreamer-edge MQTT direct data transmission.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Gichan Jang <gichan2.jang@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include "nnstreamer-edge.h"
#include "nnstreamer-edge-mqtt.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"

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
_test_edge_hybrid_event_cb (nns_edge_event_h event_h, void *user_data)
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
    case NNS_EDGE_EVENT_NEW_DATA_RECEIVED:
      _td->received++;

      ret = nns_edge_event_parse_new_data (event_h, &data_h);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

      /* Compare metadata */
      ret = nns_edge_data_get_info (data_h, "test-key", &val);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      EXPECT_STREQ (val, "test-value");
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
 * @brief Check whether MQTT broker is running or not.
 */
static bool
_check_mqtt_broker ()
{
  int ret = 0;

  ret = system ("ps aux | grep mosquitto | grep -v grep");
  if (0 != ret) {
    nns_edge_logw ("MQTT broker is not running. Skip query hybrid test.");
    return false;
  }

  return true;
}

/**
 * @brief Connect to the local host using the information received from mqtt.
 */
TEST(edgeMqttHybrid, connectLocal)
{
  nns_edge_h server_h, client_h;
  ne_test_data_s *_td_server, *_td_client;
  nns_edge_data_h data_h;
  nns_size_t data_len;
  void *data;
  unsigned int i, retry;
  int ret = 0;
  char *val;

  if (!_check_mqtt_broker ())
    return;

  _td_server = _get_test_data (true);
  _td_client = _get_test_data (false);
  ASSERT_TRUE (_td_server != NULL && _td_client != NULL);

  /* Prepare server (127.0.0.1:port) */
  nns_edge_create_handle ("temp-server", NNS_EDGE_CONNECT_TYPE_HYBRID,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &server_h);
  nns_edge_set_event_callback (server_h, _test_edge_hybrid_event_cb, _td_server);
  nns_edge_set_info (server_h, "DEST_HOST", "127.0.0.1");
  nns_edge_set_info (server_h, "DEST_PORT", "1883");
  nns_edge_set_info (server_h, "TOPIC", "temp-mqtt-topic");
  nns_edge_set_info (server_h, "CAPS", "test server");
  nns_edge_set_info (server_h, "QUEUE_SIZE", "10:NEW");
  _td_server->handle = server_h;

  /* Prepare client */
  nns_edge_create_handle ("temp-client", NNS_EDGE_CONNECT_TYPE_HYBRID,
     NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client_h);
  nns_edge_set_event_callback (client_h, _test_edge_hybrid_event_cb, _td_client);
  nns_edge_set_info (client_h, "CAPS", "test client");
  nns_edge_set_info (client_h, "TOPIC", "temp-mqtt-topic");
  _td_client->handle = client_h;

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_connect (client_h, "127.0.0.1", 1883);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  usleep (10000);

  sleep (2);

  /* Send request to server */
  data_len = 10U * sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  for (i = 0; i < 10U; i++)
    ((unsigned int *) data)[i] = i;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_get_info (client_h, "client_id", &val);
  nns_edge_data_set_info (data_h, "client_id", val);
  SAFE_FREE (val);

  ret = nns_edge_data_set_info (data_h, "test-key", "test-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 5U; i++) {
    ret = nns_edge_send (client_h, data_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    usleep (10000);
  }

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Wait for responding data (20 seconds) */
  retry = 0U;
  do {
    usleep (100000);
    if (_td_client->received > 0)
      break;
  } while (retry++ < 200U);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  EXPECT_TRUE (_td_server->received > 0);
  EXPECT_TRUE (_td_client->received > 0);

  _free_test_data (_td_server);
  _free_test_data (_td_client);
}

/**
 * @brief Connect to the mqtt broker with invalid param.
 */
TEST(edgeMqttHybrid, connectInvalidParam1_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (NULL, "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_connect ("", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect to the mqtt broker with invalid param.
 */
TEST(edgeMqttHybrid, connectInvalidParam2_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", NULL, "127.0.0.1", 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "", "127.0.0.1", 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect to the mqtt broker with invalid param.
 */
TEST(edgeMqttHybrid, connectInvalidParam3_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", NULL, 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "", 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect to the mqtt broker with invalid param.
 */
TEST(edgeMqttHybrid, connectInvalidParam4_n)
{
  int ret = -1;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect to the mqtt broker with invalid host address.
 */
TEST(edgeMqttHybrid, connectInvalidParam5_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "tcp://none", 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect to the mqtt broker with invalid port number.
 */
TEST(edgeMqttHybrid, connectInvalidParam6_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 0, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Close the mqtt handle with invalid param.
 */
TEST(edgeMqttHybrid, closeInvalidParam_n)
{
  int ret = -1;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_close (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Publish with invalid param.
 */
TEST(edgeMqttHybrid, publishInvalidParam_n)
{
  int ret = -1;
  const char *msg = "TEMP_MESSAGE";

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_publish (NULL, msg, strlen (msg) + 1);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Publish with invalid param.
 */
TEST(edgeMqttHybrid, publishInvalidParam2_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  const char *msg = "TEMP_MESSAGE";

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* data is null */
  ret = nns_edge_mqtt_publish (broker_h, NULL, strlen (msg) + 1);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Publish with invalid param.
 */
TEST(edgeMqttHybrid, publishInvalidParam3_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  const char *msg = "TEMP_MESSAGE";

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* data length is 0 */
  ret = nns_edge_mqtt_publish (broker_h, msg, 0);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Subscribe the topic with invalid param.
 */
TEST(edgeMqttHybrid, subscribeInvalidParam_n)
{
  int ret = -1;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_subscribe (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get message with invalid param.
 */
TEST(edgeMqttHybrid, getMessageInvalidParam1_n)
{
  int ret = -1;
  void *msg = NULL;
  nns_size_t msg_len;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_get_message (NULL, &msg, &msg_len, 0U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get message with invalid param.
 */
TEST(edgeMqttHybrid, getMessageInvalidParam2_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  nns_size_t msg_len;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (broker_h, NULL, &msg_len, 0U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get message with invalid param.
 */
TEST(edgeMqttHybrid, getMessageInvalidParam3_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  void *msg = NULL;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (broker_h, &msg, NULL, 0U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get message from empty message queue.
 */
TEST(edgeMqttHybrid, getMessageWithinTimeout_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  void *msg = NULL;
  nns_size_t msg_len;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (broker_h, &msg, &msg_len, 1000U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Edge event callback for test MQTT data transmission.
 */
static int
_test_edge_event_cb (nns_edge_event_h event_h, void *user_data)
{
  ne_test_data_s *_td = (ne_test_data_s *) user_data;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
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

      /* Compare received data */
      ret = nns_edge_data_get_count (data_h, &count);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      EXPECT_EQ (count, 2U);

      ret = nns_edge_data_get (data_h, 0, &data, &data_len);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      for (i = 0; i < 10U; i++)
        EXPECT_EQ (((unsigned int *) data)[i], i);

      ret = nns_edge_data_get (data_h, 1, &data, &data_len);
      EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
      for (i = 0; i < 20U; i++)
        EXPECT_EQ (((unsigned int *) data)[i], 20 - i);

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
TEST(edgeMqtt, connectLocal)
{
  nns_edge_h server_h, client1_h, client2_h;
  ne_test_data_s *_td_server, *_td_client1, *_td_client2;
  nns_edge_data_h data_h;
  nns_size_t data_len;
  void *data1, *data2;
  unsigned int i, retry;
  int ret, port;
  char *val;

  if (!_check_mqtt_broker ())
    return;

  _td_server = _get_test_data (true);
  _td_client1 = _get_test_data (false);
  _td_client2 = _get_test_data (false);
  ASSERT_TRUE (_td_server != NULL && _td_client1 != NULL && _td_client2 != NULL);
  port = nns_edge_get_available_port ();

  /* Prepare server (127.0.0.1:port) */
  val = nns_edge_strdup_printf ("%d", port);
  nns_edge_create_handle ("temp-sender", NNS_EDGE_CONNECT_TYPE_MQTT,
      NNS_EDGE_NODE_TYPE_PUB, &server_h);
  nns_edge_set_info (server_h, "IP", "127.0.0.1");
  nns_edge_set_info (server_h, "PORT", val);
  nns_edge_set_info (server_h, "DEST_IP", "127.0.0.1");
  nns_edge_set_info (server_h, "DEST_PORT", "1883");
  nns_edge_set_info (server_h, "TOPIC", "MQTT_TEST_TOPIC");
  _td_server->handle = server_h;
  SAFE_FREE (val);

  /* Prepare client */
  nns_edge_create_handle ("temp-receiver", NNS_EDGE_CONNECT_TYPE_MQTT,
      NNS_EDGE_NODE_TYPE_SUB, &client1_h);
  nns_edge_set_event_callback (client1_h, _test_edge_event_cb, _td_client1);
  nns_edge_set_info (client1_h, "TOPIC", "MQTT_TEST_TOPIC");
  _td_client1->handle = client1_h;

  nns_edge_create_handle ("temp-client2", NNS_EDGE_CONNECT_TYPE_MQTT,
      NNS_EDGE_NODE_TYPE_SUB, &client2_h);
  nns_edge_set_event_callback (client2_h, _test_edge_event_cb, _td_client2);
  nns_edge_set_info (client2_h, "TOPIC", "MQTT_TEST_TOPIC");
  _td_client2->handle = client2_h;

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client1_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client2_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_connect (client1_h, "127.0.0.1", 1883);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  usleep (10000);
  ret = nns_edge_connect (client2_h, "127.0.0.1", 1883);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  sleep (2);

  /* Send request to server */
  data_len = 10U * sizeof (unsigned int);
  data1 = malloc (data_len);
  ASSERT_TRUE (data1 != NULL);

  data2 = malloc (data_len * 2);
  ASSERT_TRUE (data2 != NULL);

  for (i = 0; i < 10U; i++)
    ((unsigned int *) data1)[i] = i;

  for (i = 0; i < 20U; i++)
    ((unsigned int *) data2)[i] = 20 - i;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data1, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, data2, data_len * 2, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 5U; i++) {
    ret = nns_edge_send (server_h, data_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    usleep (10000);
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

  EXPECT_TRUE (_td_client1->received > 0);
  EXPECT_TRUE (_td_client2->received > 0);

  _free_test_data (_td_server);
  _free_test_data (_td_client1);
  _free_test_data (_td_client2);
}

/**
 * @brief Check connection with invalid param.
 */
TEST(edgeMqtt, checkConnectionInvalidParam_n)
{
  int ret = -1;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_is_connected (NULL);
  EXPECT_NE (ret, true);
}

/**
 * @brief Set event callback with invalid param.
 */
TEST(edgeMqtt, setEventCallbackInvalidParam_n)
{
  int ret = -1;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_set_event_callback (NULL, NULL, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
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
