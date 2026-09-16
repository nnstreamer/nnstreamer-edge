/**
 * @file        unittest_nnstreamer-edge-mqtt.cc
 * @date        15 Mar 2023
 * @brief       Unittest for nnstreamer-edge MQTT direct data transmission.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Gichan Jang <gichan2.jang@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include <chrono>
#include <dlfcn.h>
#include <future>
#include <memory>
#include <thread>
#include <vector>
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-mqtt.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge.h"

/**
 * @brief Data struct for unittest.
 */
typedef struct {
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
 * @brief Get the monotonic time in milliseconds.
 */
static int64_t
_test_get_time_ms (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

struct mosquitto;

/**
 * @brief Check whether the MQTT backend is libmosquitto, whose functions are wrapped below.
 */
static bool
_test_is_mosquitto (void)
{
  return dlsym (RTLD_DEFAULT, "mosquitto_lib_init") != NULL;
}

/**
 * @brief Run the publish callback in another thread whenever it is set or reset.
 */
static bool _test_probe_publish_callback = false;

/**
 * @brief Set when a probed publish callback did not return within 2 seconds.
 */
static bool _test_publish_callback_blocked = false;

/**
 * @brief The threads that ran a probed publish callback, joined by the test.
 */
static std::vector<std::thread> _test_probes;

/**
 * @brief Run a publish callback with message id 0, which libmosquitto never uses.
 */
static void
_test_run_publish_callback (void (*callback) (struct mosquitto *, void *, int),
    struct mosquitto *mosq, std::shared_ptr<std::promise<void>> ran)
{
  callback (mosq, NULL, 0);
  ran->set_value ();
}

/**
 * @brief Wrap mosquitto_publish_callback_set() to check that the callback can run while it is set or reset.
 */
extern "C" void
mosquitto_publish_callback_set (
    struct mosquitto *mosq, void (*on_publish) (struct mosquitto *, void *, int))
{
  using callback_f = void (*) (struct mosquitto *, void *, int);
  using set_f = void (*) (struct mosquitto *, callback_f);
  static set_f real_set = (set_f) dlsym (RTLD_NEXT, "mosquitto_publish_callback_set");
  static callback_f last = NULL;
  callback_f callback = on_publish ? on_publish : last;

  if (_test_probe_publish_callback && callback) {
    auto ran = std::make_shared<std::promise<void>> ();
    std::future<void> done = ran->get_future ();

    _test_probes.emplace_back (_test_run_publish_callback, callback, mosq, ran);

    if (done.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
      _test_publish_callback_blocked = true;
  }

  if (on_publish)
    last = on_publish;
  real_set (mosq, on_publish);
}

/**
 * @brief The number of empty publishes, which clear a retained message.
 */
static unsigned int _test_clearing_publishes = 0U;

/**
 * @brief Make every empty publish fail.
 */
static bool _test_fail_clearing_publish = false;

/**
 * @brief Wrap mosquitto_publish() to count and optionally fail the publish that clears a retained message.
 */
extern "C" int
mosquitto_publish (struct mosquitto *mosq, int *mid, const char *topic,
    int payloadlen, const void *payload, int qos, bool retain)
{
  using publish_f = int (*) (
      struct mosquitto *, int *, const char *, int, const void *, int, bool);
  static publish_f real_publish = (publish_f) dlsym (RTLD_NEXT, "mosquitto_publish");

  if (payloadlen == 0) {
    _test_clearing_publishes++;
    if (_test_fail_clearing_publish)
      return 4; /* MOSQ_ERR_NO_CONN */
  }

  return real_publish (mosq, mid, topic, payloadlen, payload, qos, retain);
}

/**
 * @brief Connect to the local host using the information received from mqtt.
 */
TEST (edgeMqttHybrid, connectLocal)
{
  nns_edge_h server_h, client_h;
  ne_test_data_s *_td_server, *_td_client;
  nns_edge_data_h data_h;
  nns_size_t data_len;
  void *data;
  unsigned int i, retry;
  int ret = 0;
  char *val;
  int64_t start;

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

  /* The client has no retained message of its own to clear. */
  start = _test_get_time_ms ();
  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_LT (_test_get_time_ms () - start, 2000);

  EXPECT_TRUE (_td_server->received > 0);
  EXPECT_TRUE (_td_client->received > 0);

  _free_test_data (_td_server);
  _free_test_data (_td_client);
}

/**
 * @brief Connect to the mqtt broker with invalid param.
 */
TEST (edgeMqttHybrid, connectInvalidParam1_n)
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
TEST (edgeMqttHybrid, connectInvalidParam2_n)
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
TEST (edgeMqttHybrid, connectInvalidParam3_n)
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
TEST (edgeMqttHybrid, connectInvalidParam4_n)
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
TEST (edgeMqttHybrid, connectInvalidParam5_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-id", "temp-mqtt-topic", "tcp://none", 1883, &broker_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect to the mqtt broker with invalid port number.
 */
TEST (edgeMqttHybrid, connectInvalidParam6_n)
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
TEST (edgeMqttHybrid, closeInvalidParam_n)
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
TEST (edgeMqttHybrid, publishInvalidParam_n)
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
TEST (edgeMqttHybrid, publishInvalidParam2_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  const char *msg = "TEMP_MESSAGE";

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
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
TEST (edgeMqttHybrid, publishInvalidParam3_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  const char *msg = "TEMP_MESSAGE";

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
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
TEST (edgeMqttHybrid, subscribeInvalidParam_n)
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
TEST (edgeMqttHybrid, getMessageInvalidParam1_n)
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
TEST (edgeMqttHybrid, getMessageInvalidParam2_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  nns_size_t msg_len;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (broker_h, NULL, &msg_len, 0U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get message with invalid param.
 */
TEST (edgeMqttHybrid, getMessageInvalidParam3_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  void *msg = NULL;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (broker_h, &msg, NULL, 0U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get message from empty message queue.
 */
TEST (edgeMqttHybrid, getMessageWithinTimeout_n)
{
  int ret = -1;
  nns_edge_broker_h broker_h;
  void *msg = NULL;
  nns_size_t msg_len;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-id", "temp-mqtt-topic", "127.0.0.1", 1883, &broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (broker_h, &msg, &msg_len, 1000U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief The message queue of a broker handle stays bounded while nothing drains it.
 */
TEST (edgeMqttHybrid, messageQueueLimit)
{
  nns_edge_broker_h broker_h;
  void *msg = NULL;
  nns_size_t msg_len;
  char published[32], *first = NULL, *last = NULL;
  unsigned int i, popped = 0U;
  int ret;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-id", "temp-mqtt-queue-topic", "127.0.0.1", 1883, &broker_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_subscribe (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Nothing reads the queue here, which is the case that used to grow without limit. */
  for (i = 0; i < NNS_EDGE_MQTT_MAX_MESSAGES * 2U; i++) {
    snprintf (published, sizeof (published), "msg-%u", i);
    ret = nns_edge_mqtt_publish (broker_h, published, (int) strlen (published) + 1);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  }

  /* Let the broker deliver what it can before the queue is drained. */
  usleep (2000000);

  while (nns_edge_mqtt_get_message (broker_h, &msg, &msg_len, 100U) == NNS_EDGE_ERROR_NONE) {
    if (!first)
      first = nns_edge_strdup ((char *) msg);
    SAFE_FREE (last);
    last = (char *) msg;
    popped++;
  }

  EXPECT_GT (popped, 0U);
  EXPECT_LE (popped, NNS_EDGE_MQTT_MAX_MESSAGES);

  /**
   * Only a full queue proves which end leaks. A broker slow enough to leave the
   * queue short says nothing about the leaky option, while the bound above
   * holds whatever the broker did.
   */
  if (popped == NNS_EDGE_MQTT_MAX_MESSAGES) {
    EXPECT_STRNE (first, "msg-0");
  }

  SAFE_FREE (first);
  SAFE_FREE (last);

  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Closing a publisher sets and resets its publish callback without holding a lock the callback takes.
 */
TEST (edgeMqttHybrid, closeSetsPublishCallbackUnlocked)
{
  nns_edge_broker_h broker_h;
  const char published[] = "temp-retained";
  int ret;

  if (!_check_mqtt_broker ())
    return;
  if (!_test_is_mosquitto ())
    GTEST_SKIP () << "The test wraps libmosquitto, the MQTT backend is not mosquitto.";

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-pub", "temp-mqtt-probe-topic", "127.0.0.1", 1883, &broker_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_publish (broker_h, published, (int) sizeof (published));
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_publish_callback_blocked = false;
  _test_probe_publish_callback = true;
  ret = nns_edge_mqtt_close (broker_h);
  _test_probe_publish_callback = false;
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  for (auto &probe : _test_probes)
    probe.join ();
  EXPECT_EQ (_test_probes.size (), 2U);
  _test_probes.clear ();
  EXPECT_FALSE (_test_publish_callback_blocked);
}

/**
 * @brief Closing a subscriber, whose topic is a filter it cannot publish to, does not wait for a clear that never comes.
 */
TEST (edgeMqttHybrid, closeSubscriber)
{
  nns_edge_broker_h broker_h;
  int64_t start;
  int ret;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-sub",
      "edge/inference/+/temp-mqtt-close-topic/#", "127.0.0.1", 1883, &broker_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_subscribe (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  start = _test_get_time_ms ();
  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_LT (_test_get_time_ms () - start, 2000);
}

/**
 * @brief Publishing to a topic filter fails, and the close that follows has nothing to clear.
 */
TEST (edgeMqttHybrid, publishToTopicFilter_n)
{
  nns_edge_broker_h broker_h;
  const char msg[] = "temp-message";
  int64_t start;
  int ret;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_connect ("temp-mqtt-filter",
      "edge/inference/+/temp-mqtt-filter-topic/#", "127.0.0.1", 1883, &broker_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_publish (broker_h, msg, (int) sizeof (msg));
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  start = _test_get_time_ms ();
  ret = nns_edge_mqtt_close (broker_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_LT (_test_get_time_ms () - start, 2000);
}

/**
 * @brief A publisher clears its retained message when it closes, also right after a burst of messages.
 */
TEST (edgeMqttHybrid, closeClearsRetained)
{
  nns_edge_broker_h pub_h, sub_h;
  char published[32];
  void *msg = NULL;
  nns_size_t msg_len;
  unsigned int i;
  int64_t start;
  int ret;

  if (!_check_mqtt_broker ())
    return;
  if (!_test_is_mosquitto ())
    GTEST_SKIP () << "The test wraps libmosquitto, the MQTT backend is not mosquitto.";

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-pub", "temp-mqtt-clear-topic", "127.0.0.1", 1883, &pub_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* The acknowledgements of these are still arriving while the close waits for its own. */
  for (i = 0; i < 100U; i++) {
    snprintf (published, sizeof (published), "msg-%u", i);
    ret = nns_edge_mqtt_publish (pub_h, published, (int) strlen (published) + 1);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  }

  _test_clearing_publishes = 0U;
  start = _test_get_time_ms ();
  ret = nns_edge_mqtt_close (pub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_LT (_test_get_time_ms () - start, 5000);
  EXPECT_EQ (_test_clearing_publishes, 1U);

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-sub", "temp-mqtt-clear-topic", "127.0.0.1", 1883, &sub_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_subscribe (sub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (sub_h, &msg, &msg_len, 1000U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  SAFE_FREE (msg);

  ret = nns_edge_mqtt_close (sub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief A handle that has published nothing leaves the retained message of another node on its topic.
 */
TEST (edgeMqttHybrid, closeKeepsRetainedOfOthers)
{
  nns_edge_broker_h pub_h, idle_h, sub_h;
  const char published[] = "temp-retained";
  void *msg = NULL;
  nns_size_t msg_len;
  int ret;

  if (!_check_mqtt_broker ())
    return;
  if (!_test_is_mosquitto ())
    GTEST_SKIP () << "The test wraps libmosquitto, the MQTT backend is not mosquitto.";

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-pub", "temp-mqtt-keep-topic", "127.0.0.1", 1883, &pub_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_publish (pub_h, published, (int) sizeof (published));
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-idle", "temp-mqtt-keep-topic", "127.0.0.1", 1883, &idle_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  _test_clearing_publishes = 0U;
  ret = nns_edge_mqtt_close (idle_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (_test_clearing_publishes, 0U);

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-sub", "temp-mqtt-keep-topic", "127.0.0.1", 1883, &sub_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_subscribe (sub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_mqtt_get_message (sub_h, &msg, &msg_len, 2000U);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ ((char *) msg, published);
  SAFE_FREE (msg);

  ret = nns_edge_mqtt_close (sub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_close (pub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief A publisher whose clearing publish fails closes without waiting for an acknowledgement.
 */
TEST (edgeMqttHybrid, closeClearingPublishFails_n)
{
  nns_edge_broker_h broker_h;
  const char published[] = "temp-retained";
  int64_t start;
  int ret;

  if (!_check_mqtt_broker ())
    return;
  if (!_test_is_mosquitto ())
    GTEST_SKIP () << "The test wraps libmosquitto, the MQTT backend is not mosquitto.";

  ret = nns_edge_mqtt_connect (
      "temp-mqtt-pub", "temp-mqtt-fail-topic", "127.0.0.1", 1883, &broker_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_publish (broker_h, published, (int) sizeof (published));
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_clearing_publishes = 0U;
  _test_fail_clearing_publish = true;
  start = _test_get_time_ms ();
  ret = nns_edge_mqtt_close (broker_h);
  _test_fail_clearing_publish = false;
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (_test_clearing_publishes, 1U);
  EXPECT_LT (_test_get_time_ms () - start, 2000);

  /* Remove what the failed clear left on the broker. */
  ret = nns_edge_mqtt_connect (
      "temp-mqtt-pub", "temp-mqtt-fail-topic", "127.0.0.1", 1883, &broker_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_mqtt_publish (broker_h, published, (int) sizeof (published));
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
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
TEST (edgeMqtt, connectLocal)
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
TEST (edgeMqtt, checkConnectionInvalidParam_n)
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
TEST (edgeMqtt, setEventCallbackInvalidParam_n)
{
  int ret = -1;

  if (!_check_mqtt_broker ())
    return;

  ret = nns_edge_mqtt_set_event_callback (NULL, NULL, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief What the subscriber saw in the malformed payload test.
 */
typedef struct {
  unsigned int valid; /**< Data events carrying the one memory the test publishes. */
  unsigned int empty; /**< Data events carrying no memory at all. */
  bool saw_last; /**< The message published after the malformed one arrived. */
} ne_test_invalid_payload_s;

/**
 * @brief Edge event callback sorting the data events of the malformed payload test.
 */
static int
_test_edge_invalid_payload_event_cb (nns_edge_event_h event_h, void *user_data)
{
  ne_test_invalid_payload_s *rd = (ne_test_invalid_payload_s *) user_data;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  nns_edge_data_h data_h;
  unsigned int count = 0U;
  char *val = NULL;

  if (!rd || nns_edge_event_get_type (event_h, &event) != NNS_EDGE_ERROR_NONE)
    return NNS_EDGE_ERROR_NONE;

  if (event != NNS_EDGE_EVENT_NEW_DATA_RECEIVED)
    return NNS_EDGE_ERROR_NONE;

  if (nns_edge_event_parse_new_data (event_h, &data_h) != NNS_EDGE_ERROR_NONE)
    return NNS_EDGE_ERROR_NONE;

  nns_edge_data_get_count (data_h, &count);
  if (count == 0U)
    rd->empty++;
  else if (count == 1U)
    rd->valid++;

  if (nns_edge_data_get_info (data_h, "seq", &val) == NNS_EDGE_ERROR_NONE) {
    if (strcmp (val, "last") == 0)
      rd->saw_last = true;
    SAFE_FREE (val);
  }

  nns_edge_data_destroy (data_h);
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Publish a well formed edge data carrying one memory, tagged with the given sequence name.
 */
static int
_test_publish_valid (nns_edge_broker_h broker_h, const char *seq)
{
  nns_edge_data_h data_h;
  unsigned int payload[4] = { 1U, 2U, 3U, 4U };
  int ret;

  ret = nns_edge_data_create (&data_h);
  if (ret != NNS_EDGE_ERROR_NONE)
    return ret;

  ret = nns_edge_data_add (data_h, payload, sizeof (payload), NULL);
  if (ret == NNS_EDGE_ERROR_NONE)
    ret = nns_edge_data_set_info (data_h, "seq", seq);
  if (ret == NNS_EDGE_ERROR_NONE)
    ret = nns_edge_mqtt_publish_data (broker_h, data_h);

  nns_edge_data_destroy (data_h);
  return ret;
}

/**
 * @brief A malformed MQTT payload is dropped instead of raising a data event
 * with an empty handle.
 * @note Publishing is retained, so a subscriber that is not yet subscribed
 * receives only the last message. A first valid message proves the subscription
 * is live before the malformed one is sent, and a last one proves the malformed
 * one was consumed, since one client's messages on one topic are delivered in
 * order.
 */
TEST (edgeMqtt, deserializeInvalidPayload_n)
{
  nns_edge_h sub_h;
  nns_edge_broker_h pub_h;
  ne_test_invalid_payload_s rd;
  char *topic;
  const uint8_t garbage[] = { 0x01, 0x02, 0x03 };
  char *full_topic;
  unsigned int retry;
  int ret;

  if (!_check_mqtt_broker ())
    return;

  memset (&rd, 0, sizeof (rd));

  ret = nns_edge_create_handle ("temp-sub-invalid-payload",
      NNS_EDGE_CONNECT_TYPE_MQTT, NNS_EDGE_NODE_TYPE_SUB, &sub_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* A topic of its own, so no retained message of an earlier run is delivered here. */
  topic = nns_edge_strdup_printf ("MQTT_INVALID_PAYLOAD_%d", (int) getpid ());

  ret = nns_edge_set_event_callback (sub_h, _test_edge_invalid_payload_event_cb, &rd);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (sub_h, "TOPIC", topic);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_start (sub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (sub_h, "127.0.0.1", 1883);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* A SUB subscribes to edge/inference/+/<topic>/#, so match that pattern. */
  full_topic = nns_edge_strdup_printf ("edge/inference/127.0.0.1/%s/1234", topic);
  ret = nns_edge_mqtt_connect (
      "temp-pub-invalid-payload", full_topic, "127.0.0.1", 1883, &pub_h);
  SAFE_FREE (full_topic);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* No ASSERT past this point: the callback holds rd, so the handle must always be released. */
  if (ret == NNS_EDGE_ERROR_NONE) {
    EXPECT_EQ (_test_publish_valid (pub_h, "first"), NNS_EDGE_ERROR_NONE);

    retry = 0U;
    while (rd.valid == 0U && retry++ < 100U)
      usleep (100000);
    EXPECT_EQ (rd.valid, 1U);

    /* Shorter than nns_edge_data_header_s, the deserializer must reject it. */
    ret = nns_edge_mqtt_publish (pub_h, garbage, (int) sizeof (garbage));
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

    EXPECT_EQ (_test_publish_valid (pub_h, "last"), NNS_EDGE_ERROR_NONE);

    retry = 0U;
    while (!rd.saw_last && retry++ < 100U)
      usleep (100000);

    EXPECT_TRUE (rd.saw_last);
    EXPECT_EQ (rd.valid, 2U);
    EXPECT_EQ (rd.empty, 0U);

    ret = nns_edge_mqtt_close (pub_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  }

  ret = nns_edge_release_handle (sub_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (topic);
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
