/**
 * @file        unittest_nnstreamer-edge-thread.cc
 * @date        07 Sep 2026
 * @brief       Unittest for the concurrent use of a nnstreamer-edge handle.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      MyungJoo Ham <myungjoo.ham@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge.h"

/**
 * @brief Data struct for the concurrency unittest.
 */
typedef struct {
  nns_edge_h handle;
  std::atomic<unsigned int> received;
  std::atomic<unsigned int> connected;
} ne_thread_test_data_s;

/**
 * @brief Edge event callback for test. Invoked from the message and listener threads.
 */
static int
_thread_test_event_cb (nns_edge_event_h event_h, void *user_data)
{
  ne_thread_test_data_s *_td = (ne_thread_test_data_s *) user_data;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  nns_edge_data_h data_h;

  if (!_td)
    return NNS_EDGE_ERROR_NONE;

  if (nns_edge_event_get_type (event_h, &event) != NNS_EDGE_ERROR_NONE)
    return NNS_EDGE_ERROR_NONE;

  switch (event) {
    case NNS_EDGE_EVENT_NEW_DATA_RECEIVED:
      if (nns_edge_event_parse_new_data (event_h, &data_h) == NNS_EDGE_ERROR_NONE) {
        _td->received++;
        nns_edge_data_destroy (data_h);
      }
      break;
    case NNS_EDGE_EVENT_CONNECTION_COMPLETED:
      _td->connected++;
      break;
    default:
      break;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Create a data handle holding a payload of the given size.
 */
static nns_edge_data_h
_create_test_data (nns_size_t data_len = 16U * sizeof (unsigned int))
{
  nns_edge_data_h data_h = NULL;
  void *data;

  if (nns_edge_data_create (&data_h) != NNS_EDGE_ERROR_NONE)
    return NULL;

  data = calloc (1, data_len);
  if (!data || nns_edge_data_add (data_h, data, data_len, nns_edge_free) != NNS_EDGE_ERROR_NONE) {
    SAFE_FREE (data);
    nns_edge_data_destroy (data_h);
    return NULL;
  }

  return data_h;
}

/**
 * @brief Create and start a listening node on the given port.
 */
static nns_edge_h
_start_test_server (const char *id, int port, ne_thread_test_data_s *_td,
    nns_edge_node_type_e node_type = NNS_EDGE_NODE_TYPE_QUERY_SERVER)
{
  nns_edge_h server_h = NULL;
  char *val;

  if (nns_edge_create_handle (id, NNS_EDGE_CONNECT_TYPE_TCP, node_type, &server_h) != NNS_EDGE_ERROR_NONE)
    return NULL;

  val = nns_edge_strdup_printf ("%d", port);
  nns_edge_set_event_callback (server_h, _thread_test_event_cb, _td);
  nns_edge_set_info (server_h, "IP", "127.0.0.1");
  nns_edge_set_info (server_h, "PORT", val);
  nns_edge_set_info (server_h, "CAPS", "test server");
  nns_edge_set_info (server_h, "QUEUE_SIZE", "10:OLD");
  SAFE_FREE (val);

  _td->handle = server_h;

  if (nns_edge_start (server_h) != NNS_EDGE_ERROR_NONE) {
    nns_edge_release_handle (server_h);
    return NULL;
  }

  return server_h;
}

/**
 * @brief Create, start and connect a client node to the given port.
 */
static nns_edge_h
_start_test_client (const char *id, int port, ne_thread_test_data_s *_td,
    nns_edge_node_type_e node_type = NNS_EDGE_NODE_TYPE_QUERY_CLIENT)
{
  nns_edge_h client_h = NULL;

  if (nns_edge_create_handle (id, NNS_EDGE_CONNECT_TYPE_TCP, node_type, &client_h) != NNS_EDGE_ERROR_NONE)
    return NULL;

  nns_edge_set_event_callback (client_h, _thread_test_event_cb, _td);
  nns_edge_set_info (client_h, "IP", "127.0.0.1");
  nns_edge_set_info (client_h, "CAPS", "test client");

  _td->handle = client_h;

  if (nns_edge_start (client_h) != NNS_EDGE_ERROR_NONE) {
    nns_edge_release_handle (client_h);
    return NULL;
  }

  if (nns_edge_connect (client_h, "127.0.0.1", port) != NNS_EDGE_ERROR_NONE) {
    nns_edge_release_handle (client_h);
    return NULL;
  }

  return client_h;
}

/**
 * @brief Broadcast from the send thread while peers are disconnecting.
 * @details The send thread walks the connection list while the message thread of
 * a dropped peer removes its entry. Without a lock the walk uses freed memory.
 */
TEST (edgeThread, sendWhilePeerDrops)
{
  ne_thread_test_data_s server_td = {};
  nns_edge_h server_h;
  std::atomic<bool> running (true);
  std::thread peers[4];
  unsigned int i;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("thread-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  /* A large payload keeps the send thread inside the connection list walk. */
  std::thread sender ([&] () {
    while (running.load ()) {
      nns_edge_data_h data_h = _create_test_data (256U * 1024U);
      if (data_h) {
        nns_edge_send (server_h, data_h);
        nns_edge_data_destroy (data_h);
      }
    }
  });

  for (i = 0; i < 4U; i++) {
    peers[i] = std::thread ([&, i] () {
      unsigned int n = 0;
      while (running.load ()) {
        ne_thread_test_data_s client_td = {};
        nns_edge_h client_h;
        char *id = nns_edge_strdup_printf ("thread-client-%u-%u", i, n++);

        client_h = _start_test_client (id, port, &client_td);
        SAFE_FREE (id);

        if (client_h) {
          usleep (10000);
          EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);
        }
      }
    });
  }

  usleep (1500000);
  running.store (false);

  for (i = 0; i < 4U; i++)
    peers[i].join ();
  sender.join ();

  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
  EXPECT_GT (server_td.connected.load (), 0U);
}

/**
 * @brief Replace the capability string while the listener thread sends it.
 * @details The listener thread reads caps_str outside the handle lock while
 * nns_edge_set_info() frees and replaces it.
 */
TEST (edgeThread, setCapsWhileAccept)
{
  ne_thread_test_data_s server_td = {};
  nns_edge_h server_h;
  std::atomic<bool> updating (true);
  unsigned int i;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("caps-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  std::thread updater ([&] () {
    unsigned int n = 0;
    while (updating.load ()) {
      char *caps
          = nns_edge_strdup_printf ("test server caps %0*u", (int) (n % 64U) + 1, n);
      EXPECT_EQ (nns_edge_set_info (server_h, "CAPS", caps), NNS_EDGE_ERROR_NONE);
      SAFE_FREE (caps);
      n++;
      usleep (200);
    }
  });

  for (i = 0; i < 20U; i++) {
    ne_thread_test_data_s client_td = {};
    nns_edge_h client_h;
    char *id = nns_edge_strdup_printf ("caps-client-%u", i);

    client_h = _start_test_client (id, port, &client_td);
    SAFE_FREE (id);

    if (client_h) {
      usleep (10000);
      EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);
    }
  }

  updating.store (false);
  updater.join ();

  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Release the handle while the listener and send threads are running.
 * @details nns_edge_release_handle() must join both threads without holding the
 * handle lock they take, otherwise it deadlocks and this test times out.
 */
TEST (edgeThread, releaseWhileAccepting)
{
  ne_thread_test_data_s server_td = {};
  ne_thread_test_data_s client_td = {};
  nns_edge_h server_h, client_h;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("release-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  client_h = _start_test_client ("release-client", port, &client_td);
  ASSERT_TRUE (client_h != NULL);

  std::thread sender ([&] () {
    unsigned int n;
    for (n = 0; n < 50U; n++) {
      nns_edge_data_h data_h = _create_test_data ();
      if (data_h) {
        nns_edge_send (client_h, data_h);
        nns_edge_data_destroy (data_h);
      }
      usleep (1000);
    }
  });

  usleep (20000);
  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);

  sender.join ();
  EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Release the handle right after a burst of sends.
 * @details nns_edge_release_handle() joins the send thread. The stop signal of
 * the send queue is lost if that thread is on its way into the wait, and the
 * join then never returns.
 */
TEST (edgeThread, releaseWhileSending)
{
  ne_thread_test_data_s server_td = {};
  ne_thread_test_data_s client_td = {};
  nns_edge_h server_h, client_h;
  std::atomic<bool> sending (true);
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("busy-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  client_h = _start_test_client ("busy-client", port, &client_td);
  ASSERT_TRUE (client_h != NULL);
  usleep (200000);

  std::thread sender ([&] () {
    while (sending.load ()) {
      nns_edge_data_h data_h = _create_test_data ();
      if (data_h) {
        nns_edge_send (client_h, data_h);
        nns_edge_data_destroy (data_h);
      }
    }
  });

  usleep (500000);
  sending.store (false);
  sender.join ();

  EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Reconnect the client while its own send thread transfers data.
 * @details nns_edge_disconnect() frees the sink connection the send thread is
 * about to use, and nns_edge_connect() replaces it with a new one.
 */
TEST (edgeThread, reconnectWhileSending)
{
  ne_thread_test_data_s server_td = {};
  ne_thread_test_data_s client_td = {};
  nns_edge_h server_h, client_h;
  std::atomic<bool> sending (true);
  unsigned int i;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("reconn-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  client_h = _start_test_client ("reconn-client", port, &client_td);
  ASSERT_TRUE (client_h != NULL);

  std::thread sender ([&] () {
    while (sending.load ()) {
      nns_edge_data_h data_h = _create_test_data ();
      if (data_h) {
        nns_edge_send (client_h, data_h);
        nns_edge_data_destroy (data_h);
      }
      usleep (500);
    }
  });

  for (i = 0; i < 40U; i++) {
    EXPECT_EQ (nns_edge_disconnect (client_h), NNS_EDGE_ERROR_NONE);
    usleep (2000);
    nns_edge_connect (client_h, "127.0.0.1", port);
    usleep (10000);
  }

  sending.store (false);
  sender.join ();

  EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Reconnect a subscriber while the publisher is sending.
 * @details A subscriber registers its connection in _nns_edge_connect_to(),
 * where the message thread that may remove it again is started.
 */
TEST (edgeThread, subReconnectWhilePublishing)
{
  ne_thread_test_data_s pub_td = {};
  ne_thread_test_data_s sub_td = {};
  nns_edge_h pub_h, sub_h;
  std::atomic<bool> sending (true);
  unsigned int i;
  int port;

  port = nns_edge_get_available_port ();
  pub_h = _start_test_server ("pub-node", port, &pub_td, NNS_EDGE_NODE_TYPE_PUB);
  ASSERT_TRUE (pub_h != NULL);

  sub_h = _start_test_client ("sub-node", port, &sub_td, NNS_EDGE_NODE_TYPE_SUB);
  ASSERT_TRUE (sub_h != NULL);

  std::thread sender ([&] () {
    while (sending.load ()) {
      nns_edge_data_h data_h = _create_test_data (64U * 1024U);
      if (data_h) {
        nns_edge_send (pub_h, data_h);
        nns_edge_data_destroy (data_h);
      }
      usleep (500);
    }
  });

  for (i = 0; i < 30U; i++) {
    EXPECT_EQ (nns_edge_disconnect (sub_h), NNS_EDGE_ERROR_NONE);
    usleep (2000);
    nns_edge_connect (sub_h, "127.0.0.1", port);
    usleep (10000);
  }

  sending.store (false);
  sender.join ();

  EXPECT_EQ (nns_edge_release_handle (sub_h), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_release_handle (pub_h), NNS_EDGE_ERROR_NONE);

  EXPECT_GT (sub_td.received.load (), 0U);
}

/**
 * @brief Disconnect a subscriber before its message thread gets going.
 * @details nns_edge_disconnect() lowers the running flag of the connection and
 * joins its message thread. A thread that raises that flag itself once it is
 * scheduled undoes the request, and the join never returns.
 */
TEST (edgeThread, disconnectBeforeThreadRuns)
{
  ne_thread_test_data_s pub_td = {};
  ne_thread_test_data_s sub_td = {};
  nns_edge_h pub_h, sub_h;
  unsigned int i;
  int port;

  port = nns_edge_get_available_port ();
  pub_h = _start_test_server ("race-pub", port, &pub_td, NNS_EDGE_NODE_TYPE_PUB);
  ASSERT_TRUE (pub_h != NULL);

  sub_h = _start_test_client ("race-sub", port, &sub_td, NNS_EDGE_NODE_TYPE_SUB);
  ASSERT_TRUE (sub_h != NULL);

  /* No wait in between, the thread should still be starting up. */
  for (i = 0; i < 100U; i++) {
    EXPECT_EQ (nns_edge_disconnect (sub_h), NNS_EDGE_ERROR_NONE);
    nns_edge_connect (sub_h, "127.0.0.1", port);
  }

  EXPECT_EQ (nns_edge_release_handle (sub_h), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_release_handle (pub_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Send data to a connected peer selected by its client ID.
 */
TEST (edgeThread, sendClientId)
{
  ne_thread_test_data_s server_td = {};
  ne_thread_test_data_s client_td = {};
  nns_edge_h server_h, client_h;
  nns_edge_data_h data_h;
  unsigned int retry;
  char *client_id = NULL;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("id-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  client_h = _start_test_client ("id-client", port, &client_td);
  ASSERT_TRUE (client_h != NULL);

  for (retry = 0U; retry < 100U; retry++) {
    if (server_td.connected.load () > 0U)
      break;
    usleep (10000);
  }
  ASSERT_GT (server_td.connected.load (), 0U);

  ASSERT_EQ (nns_edge_get_info (client_h, "client_id", &client_id), NNS_EDGE_ERROR_NONE);

  data_h = _create_test_data ();
  ASSERT_TRUE (data_h != NULL);
  EXPECT_EQ (nns_edge_data_set_info (data_h, "client_id", client_id), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_send (server_h, data_h), NNS_EDGE_ERROR_NONE);

  for (retry = 0U; retry < 100U; retry++) {
    if (client_td.received.load () > 0U)
      break;
    usleep (10000);
  }
  EXPECT_GT (client_td.received.load (), 0U);

  nns_edge_data_destroy (data_h);
  SAFE_FREE (client_id);

  EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Send data to an unknown client ID - invalid param.
 */
TEST (edgeThread, sendClientIdUnknown_n)
{
  ne_thread_test_data_s server_td = {};
  ne_thread_test_data_s client_td = {};
  nns_edge_h server_h, client_h;
  nns_edge_data_h data_h;
  unsigned int retry;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("unknown-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  client_h = _start_test_client ("unknown-client", port, &client_td);
  ASSERT_TRUE (client_h != NULL);

  for (retry = 0U; retry < 100U; retry++) {
    if (server_td.connected.load () > 0U)
      break;
    usleep (10000);
  }
  ASSERT_GT (server_td.connected.load (), 0U);

  data_h = _create_test_data ();
  ASSERT_TRUE (data_h != NULL);
  EXPECT_EQ (nns_edge_data_set_info (data_h, "client_id", "1"), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_send (server_h, data_h), NNS_EDGE_ERROR_NONE);

  usleep (300000);
  EXPECT_EQ (client_td.received.load (), 0U);

  nns_edge_data_destroy (data_h);

  EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Send data with no connected peer - invalid state.
 */
TEST (edgeThread, sendNoConnection_n)
{
  ne_thread_test_data_s server_td = {};
  nns_edge_h server_h;
  nns_edge_data_h data_h;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _start_test_server ("empty-server", port, &server_td);
  ASSERT_TRUE (server_h != NULL);

  data_h = _create_test_data ();
  ASSERT_TRUE (data_h != NULL);

  EXPECT_EQ (nns_edge_send (server_h, data_h), NNS_EDGE_ERROR_IO);
  EXPECT_EQ (nns_edge_is_connected (server_h), NNS_EDGE_ERROR_CONNECTION_FAILURE);

  nns_edge_data_destroy (data_h);
  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Main gtest.
 */
int
main (int argc, char **argv)
{
  int result = -1;

  try {
    testing::InitGoogleTest (&argc, argv);
  } catch (...) {
    nns_edge_loge ("Caught exception, failed to init google test.");
    return 0;
  }

  try {
    result = RUN_ALL_TESTS ();
  } catch (...) {
    nns_edge_loge ("Caught exception, failed to run the unittest.");
  }

  return result;
}
