/**
 * @file        unittest_nnstreamer-edge.cc
 * @date        27 June 2022
 * @brief       Unittest for nnstreamer-edge library.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Jaeyun Jung <jy1210.jung@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-metadata.h"
#include "nnstreamer-edge-mqtt.h"
#include "nnstreamer-edge-queue.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge.h"

/**
 * @brief Make the nns_edge_get_host_string() below fail.
 */
static bool nns_edge_test_host_string_fails = false;

/**
 * @brief nns_edge_get_host_string() replacement that the tests can make fail on demand.
 * @note This shadows the definition in the library for the whole process, so a
 *       test should set the flag around the call it wants to fail and clear it
 *       right after.
 */
extern "C" char *
nns_edge_get_host_string (const char *host, const int port)
{
  if (nns_edge_test_host_string_fails)
    return NULL;

  return nns_edge_strdup_printf ("%s:%d", host, port);
}

/**
 * @brief Count the file descriptors this process has open.
 */
static int
_get_open_fd_count (void)
{
  DIR *dir;
  struct dirent *entry;
  int count = 0;

  dir = opendir ("/proc/self/fd");
  if (!dir)
    return -1;

  while ((entry = readdir (dir)) != NULL) {
    if (entry->d_name[0] != '.')
      count++;
  }

  closedir (dir);
  return count;
}

/**
 * @brief Data struct for unittest.
 */
typedef struct {
  nns_edge_h handle;
  bool running;
  bool is_server;
  bool event_cb_released;
  bool reject_connection;
  unsigned int received;
  unsigned int connection_completed;
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
 * @brief Mirror of the internal header of the serialized edge data.
 * @note Keep this in sync with nns_edge_data_header_s in nnstreamer-edge-data.c.
 * _get_serialized_data () asserts the size and the position of every field, so a mismatch fails there instead of silently checking the wrong bytes.
 */
typedef struct {
  uint32_t key;
  uint64_t version;
  uint32_t num_mem;
  nns_size_t data_len[NNS_EDGE_DATA_LIMIT];
  nns_size_t meta_len;
} ne_test_data_header_s;

static volatile unsigned char ne_test_stack_sink;

/**
 * @brief Leave a non-zero pattern on the stack area that the next call will use.
 * @note Best effort. Whether the pattern lands where the callee places its header depends on the frame layout, so this can let a test pass vacuously but never fail spuriously.
 */
static void
_dirty_stack (void)
{
  unsigned char pattern[4096];

  memset (pattern, 0xAA, sizeof (pattern));
  ne_test_stack_sink = pattern[sizeof (pattern) - 1];
}

/**
 * @brief Serialize an edge data handle holding a single raw memory and no metadata.
 */
static void
_get_serialized_data (void **data, nns_size_t *data_len, nns_size_t *mem_len)
{
  nns_edge_data_h data_h;
  ne_test_data_header_s *header;
  void *mem;
  int ret;

  *data = NULL;
  *data_len = 0U;
  *mem_len = 64U;

  mem = nns_edge_malloc (*mem_len);
  ASSERT_TRUE (mem != NULL);
  memset (mem, 0x5A, *mem_len);

  ret = nns_edge_data_create (&data_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (data_h, mem, *mem_len, nns_edge_free);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (data_h, data, data_len);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ASSERT_EQ (*data_len, sizeof (ne_test_data_header_s) + *mem_len);

  /** Reordering the private header would keep its size, so check where the fields landed. */
  header = (ne_test_data_header_s *) (*data);
  ASSERT_TRUE (nns_edge_parse_version_key (header->version, NULL, NULL, NULL));
  ASSERT_EQ (header->num_mem, 1U);
  ASSERT_EQ (header->data_len[0], *mem_len);
  ASSERT_EQ (header->meta_len, 0U);

  ret = nns_edge_data_destroy (data_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
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
TEST (edge, connectLocal)
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
 * @brief Data struct to check the connection closed by the peer.
 */
typedef struct {
  nns_edge_h handle;
  bool disconnect_in_cb;
  bool disconnect_in_data_cb;
  unsigned int delay;
  unsigned int started;
  unsigned int completed;
} ne_test_closed_s;

/**
 * @brief Edge event callback to check the message thread of the closed connection.
 */
static int
_test_closed_event_cb (nns_edge_event_h event_h, void *user_data)
{
  ne_test_closed_s *_td = (ne_test_closed_s *) user_data;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  int ret;

  if (!_td)
    return NNS_EDGE_ERROR_NONE;

  ret = nns_edge_event_get_type (event_h, &event);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  if (event == NNS_EDGE_EVENT_NEW_DATA_RECEIVED && _td->disconnect_in_data_cb) {
    nns_edge_data_h data_h;

    ret = nns_edge_event_parse_new_data (event_h, &data_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    nns_edge_data_destroy (data_h);

    /**
     * @note This is test code, calling edge API in the message thread.
     * Recommend not to call edge API in event callback.
     */
    ret = nns_edge_disconnect (_td->handle);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

    _td->started++;
    _td->completed++;
    return NNS_EDGE_ERROR_NONE;
  }

  if (event != NNS_EDGE_EVENT_CONNECTION_CLOSED)
    return NNS_EDGE_ERROR_NONE;

  if (_td->disconnect_in_cb) {
    /**
     * @note This is test code, calling edge API in the message thread.
     * Recommend not to call edge API in event callback. The test increases
     * the counter after the call, so that the main thread cannot release the
     * handle while this thread is waiting for the handle lock.
     */
    ret = nns_edge_disconnect (_td->handle);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  }

  _td->started++;

  if (_td->delay > 0U)
    usleep (_td->delay);

  _td->completed++;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Prepare server handle to test the closed connection.
 */
static void
_prepare_closed_test_server (ne_test_closed_s *_td, nns_edge_h *server_h, int port)
{
  char *val;

  val = nns_edge_strdup_printf ("%d", port);

  nns_edge_create_handle ("temp-server", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, server_h);
  nns_edge_set_event_callback (*server_h, _test_closed_event_cb, _td);
  nns_edge_set_info (*server_h, "IP", "127.0.0.1");
  nns_edge_set_info (*server_h, "PORT", val);
  nns_edge_set_info (*server_h, "CAPS", "test server");
  _td->handle = *server_h;
  SAFE_FREE (val);
}

/**
 * @brief Prepare client handle to test the closed connection.
 */
static void
_prepare_closed_test_client (nns_edge_h *client_h)
{
  nns_edge_create_handle ("temp-client", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, client_h);
  nns_edge_set_event_callback (*client_h, _test_closed_event_cb, NULL);
  nns_edge_set_info (*client_h, "CAPS", "test client");
}

/**
 * @brief Wait for the given count of the connection-closed event.
 */
static void
_wait_closed_event (ne_test_closed_s *_td, unsigned int count)
{
  unsigned int retry = 0U;

  do {
    usleep (10000);
    if (_td->started >= count)
      break;
  } while (retry++ < 500U);
}

/**
 * @brief The message thread of the closed connection should not outlive the handle.
 */
TEST (edge, connectionClosedByPeer)
{
  nns_edge_h server_h, client_h;
  ne_test_closed_s td;
  int ret, port;

  memset (&td, 0, sizeof (ne_test_closed_s));
  td.delay = 500000U;

  port = nns_edge_get_available_port ();
  _prepare_closed_test_server (&td, &server_h, port);
  _prepare_closed_test_client (&client_h);

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  /* Release the peer, then the server starts to close the connection. */
  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _wait_closed_event (&td, 1U);
  ASSERT_GE (td.started, 1U);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* The message thread should be terminated while releasing the handle. */
  EXPECT_EQ (td.completed, td.started);
}

/**
 * @brief Repeated connection lost should not accumulate the message thread.
 */
TEST (edge, connectionClosedByPeerRepeated)
{
  nns_edge_h server_h, client_h;
  ne_test_closed_s td;
  unsigned int i;
  int ret, port;

  memset (&td, 0, sizeof (ne_test_closed_s));
  td.delay = 300000U;

  port = nns_edge_get_available_port ();
  _prepare_closed_test_server (&td, &server_h, port);

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  for (i = 1U; i <= 3U; i++) {
    _prepare_closed_test_client (&client_h);

    ret = nns_edge_start (client_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    ret = nns_edge_connect (client_h, "127.0.0.1", port);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

    usleep (200000);

    ret = nns_edge_release_handle (client_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

    _wait_closed_event (&td, i);
    EXPECT_GE (td.started, i);
  }

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* Every message thread should be terminated while releasing the handle. */
  EXPECT_GE (td.started, 3U);
  EXPECT_EQ (td.completed, td.started);
}

/**
 * @brief Get the number of the file descriptors this process has open.
 */
static unsigned int
_get_open_fd (void)
{
  struct dirent *entry;
  unsigned int count = 0U;
  DIR *dir;

  dir = opendir ("/proc/self/fd");
  if (!dir)
    return 0U;

  while ((entry = readdir (dir)) != NULL) {
    if (entry->d_name[0] != '.')
      count++;
  }

  closedir (dir);
  return count;
}

/**
 * @brief Disconnecting a node itself should not report a connection closed by the peer.
 */
TEST (edge, disconnectWithoutClosedEvent)
{
  nns_edge_h server_h, client_h;
  ne_test_closed_s td;
  int ret, port;

  memset (&td, 0, sizeof (ne_test_closed_s));

  port = nns_edge_get_available_port ();
  _prepare_closed_test_server (&td, &server_h, port);
  _prepare_closed_test_client (&client_h);

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ASSERT_EQ (nns_edge_is_connected (server_h), NNS_EDGE_ERROR_NONE);

  ret = nns_edge_disconnect (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (300000);

  /* The peer did not close the connection, this node did. */
  EXPECT_EQ (td.started, 0U);

  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief The socket of a lost peer should be closed when the connection is removed.
 */
TEST (edge, connectionClosedByPeerClosesSocket)
{
  nns_edge_h server_h, client_h;
  ne_test_closed_s td;
  unsigned int open_fd;
  int ret, port;

  memset (&td, 0, sizeof (ne_test_closed_s));

  port = nns_edge_get_available_port ();
  _prepare_closed_test_server (&td, &server_h, port);

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  open_fd = _get_open_fd ();
  if (open_fd == 0U) {
    ret = nns_edge_release_handle (server_h);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
    GTEST_SKIP () << "Cannot read the open file descriptors of this process.";
  }

  _prepare_closed_test_client (&client_h);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _wait_closed_event (&td, 1U);
  ASSERT_GE (td.started, 1U);

  usleep (200000);

  /* The connection data is released later, its sockets are not. */
  EXPECT_LE (_get_open_fd (), open_fd);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief The new peer may be gone while the server is still accepting it.
 */
TEST (edge, connectionClosedByPeerWhileAccepting)
{
  nns_edge_h server_h, client1_h, client2_h;
  ne_test_closed_s td;
  int ret, port;

  memset (&td, 0, sizeof (ne_test_closed_s));
  td.delay = 300000U;

  port = nns_edge_get_available_port ();
  _prepare_closed_test_server (&td, &server_h, port);

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  /* Lose the first peer, the server stays in its event callback for a while. */
  _prepare_closed_test_client (&client1_h);
  ret = nns_edge_start (client1_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (client1_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_release_handle (client1_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _wait_closed_event (&td, 1U);
  ASSERT_GE (td.started, 1U);

  /* The second peer is gone before the server completes the connection. */
  _prepare_closed_test_client (&client2_h);
  ret = nns_edge_start (client2_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (client2_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* The server is now waiting for the message thread of the first peer. */
  usleep (100000);

  ret = nns_edge_release_handle (client2_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (500000);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  EXPECT_EQ (td.completed, td.started);
}

/**
 * @brief Disconnect in the data callback should stop the message thread.
 * @note The API does not allow calling it in the callback, this checks the misuse is handled.
 */
TEST (edge, disconnectInDataCb_n)
{
  nns_edge_h server_h, client_h;
  ne_test_closed_s td;
  nns_edge_data_h data_h;
  nns_size_t data_len;
  void *data;
  int ret, port;

  memset (&td, 0, sizeof (ne_test_closed_s));
  td.disconnect_in_data_cb = true;

  port = nns_edge_get_available_port ();
  _prepare_closed_test_server (&td, &server_h, port);
  _prepare_closed_test_client (&client_h);

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  data_len = 10U * sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (data_h, data, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_send (client_h, data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _wait_closed_event (&td, 1U);
  ASSERT_GE (td.started, 1U);

  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  EXPECT_EQ (td.completed, td.started);
}

/**
 * @brief Disconnect in the event callback, the message thread cannot release its own connection.
 * @note The API does not allow calling it in the callback, this checks the misuse is handled.
 */
TEST (edge, connectionClosedByPeerDisconnectInCb_n)
{
  nns_edge_h server_h, client_h;
  ne_test_closed_s td;
  int ret, port;

  memset (&td, 0, sizeof (ne_test_closed_s));
  td.disconnect_in_cb = true;

  port = nns_edge_get_available_port ();
  _prepare_closed_test_server (&td, &server_h, port);
  _prepare_closed_test_client (&client_h);

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _wait_closed_event (&td, 1U);
  ASSERT_GE (td.started, 1U);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  EXPECT_EQ (td.completed, td.started);
}

/**
 * @brief Edge event callback rejecting a new connection, for test.
 */
static int
_test_reject_connection_cb (nns_edge_event_h event_h, void *user_data)
{
  ne_test_data_s *_td = (ne_test_data_s *) user_data;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  int ret;

  ret = nns_edge_event_get_type (event_h, &event);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  if (NNS_EDGE_EVENT_CONNECTION_COMPLETED == event && _td) {
    _td->connection_completed++;

    if (_td->reject_connection)
      return NNS_EDGE_ERROR_UNKNOWN;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Connect to the given port with a raw socket, for test.
 */
static int
_test_open_raw_socket (int port)
{
  struct sockaddr_in saddr = { 0 };
  int sockfd;

  sockfd = socket (AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0)
    return -1;

  saddr.sin_family = AF_INET;
  saddr.sin_addr.s_addr = inet_addr ("127.0.0.1");
  saddr.sin_port = htons (port);

  if (connect (sockfd, (struct sockaddr *) &saddr, sizeof (saddr)) < 0) {
    close (sockfd);
    return -1;
  }

  return sockfd;
}

/**
 * @brief Start an edge handle listening on the local host, for test.
 */
static void
_test_start_listener (nns_edge_node_type_e node_type, ne_test_data_s *_td,
    nns_edge_h *edge_h, int *port)
{
  nns_edge_h handle;
  char *val;
  int ret;

  *edge_h = NULL;
  *port = nns_edge_get_available_port ();
  ASSERT_TRUE (*port > 0);

  ret = nns_edge_create_handle (
      "temp-listener", NNS_EDGE_CONNECT_TYPE_TCP, node_type, &handle);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_event_callback (handle, _test_reject_connection_cb, _td);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (handle, "IP", "127.0.0.1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  val = nns_edge_strdup_printf ("%d", *port);
  ret = nns_edge_set_info (handle, "PORT", val);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  SAFE_FREE (val);

  ret = nns_edge_set_info (handle, "CAPS", "test caps");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _td->handle = handle;
  *edge_h = handle;

  ret = nns_edge_start (handle);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Open a raw connection and wait until the listener handles it.
 */
static int
_test_connect_and_wait (int port, ne_test_data_s *_td, unsigned int expected)
{
  unsigned int retry = 0U;
  int sockfd;

  sockfd = _test_open_raw_socket (port);
  if (sockfd < 0)
    return -1;

  do {
    usleep (20000);
  } while (_td->connection_completed < expected && retry++ < 200U);

  return sockfd;
}

/**
 * @brief Reject new connections of a listening node, and release the handle.
 * @note The rejected connection should not be released twice.
 */
static void
_test_reject_connections (nns_edge_node_type_e node_type)
{
  nns_edge_h edge_h;
  ne_test_data_s *_td;
  unsigned int i;
  int ret, port, sockfd;

  _td = _get_test_data (true);
  ASSERT_TRUE (_td != NULL);
  _td->reject_connection = true;

  _test_start_listener (node_type, _td, &edge_h, &port);
  ASSERT_TRUE (edge_h != NULL);

  for (i = 1U; i <= 3U; i++) {
    sockfd = _test_connect_and_wait (port, _td, i);
    ASSERT_TRUE (sockfd >= 0);
    EXPECT_EQ (_td->connection_completed, i);
    close (sockfd);
    usleep (100000);
  }

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _free_test_data (_td);
}

/**
 * @brief The publisher rejects new connections in the event callback.
 */
TEST (edge, rejectPubConnection_n)
{
  _test_reject_connections (NNS_EDGE_NODE_TYPE_PUB);
}

/**
 * @brief The query client rejects new connections in the event callback.
 */
TEST (edge, rejectQueryConnection_n)
{
  _test_reject_connections (NNS_EDGE_NODE_TYPE_QUERY_CLIENT);
}

/**
 * @brief The peer disappears before the query server gets its host info.
 */
TEST (edge, acceptIncompleteConnection_n)
{
  nns_edge_h server_h;
  ne_test_data_s *_td_server;
  nns_edge_node_type_e node_type = NNS_EDGE_NODE_TYPE_QUERY_SERVER;
  int ret, port, sockfd;

  _td_server = _get_test_data (true);
  ASSERT_TRUE (_td_server != NULL);

  _test_start_listener (node_type, _td_server, &server_h, &port);
  ASSERT_TRUE (server_h != NULL);

  sockfd = _test_open_raw_socket (port);
  ASSERT_TRUE (sockfd >= 0);
  close (sockfd);
  usleep (300000);

  EXPECT_EQ (_td_server->connection_completed, 0U);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _free_test_data (_td_server);
}

/**
 * @brief Send data while a rejected connection is still in the table.
 */
TEST (edge, sendAfterRejectedConnection)
{
  nns_edge_h server_h;
  ne_test_data_s *_td_server;
  nns_edge_data_h data_h;
  nns_size_t data_len;
  void *data;
  int ret, port, rejected_fd, accepted_fd;

  _td_server = _get_test_data (true);
  ASSERT_TRUE (_td_server != NULL);
  _td_server->reject_connection = true;

  _test_start_listener (NNS_EDGE_NODE_TYPE_PUB, _td_server, &server_h, &port);
  ASSERT_TRUE (server_h != NULL);

  rejected_fd = _test_connect_and_wait (port, _td_server, 1U);
  ASSERT_TRUE (rejected_fd >= 0);
  EXPECT_EQ (_td_server->connection_completed, 1U);

  _td_server->reject_connection = false;
  accepted_fd = _test_connect_and_wait (port, _td_server, 2U);
  ASSERT_TRUE (accepted_fd >= 0);
  EXPECT_EQ (_td_server->connection_completed, 2U);

  data_len = 10U * sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (data_h, data, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_send (server_h, data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  usleep (300000);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  close (rejected_fd);
  close (accepted_fd);

  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _free_test_data (_td_server);
}

/**
 * @brief Create edge handle - invalid param.
 */
TEST (edge, createHandleInvalidParam01_n)
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
TEST (edge, createHandleInvalidParam02_n)
{
  int ret;

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Create edge handle - invalid param.
 */
TEST (edge, createHandleInvalidParam03_n)
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
TEST (edge, startInvalidParam01_n)
{
  int ret;

  ret = nns_edge_start (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Start - invalid param.
 */
TEST (edge, startInvalidParam02_n)
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
 * @brief Starting an already started handle should be a no-op, not a second listener.
 */
TEST (edge, startTwice)
{
  nns_edge_h edge_h;
  int ret, fd_before, fd_after;

  fd_before = _get_open_fd_count ();
  ASSERT_GE (fd_before, 0);

  ret = nns_edge_create_handle ("temp-id", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &edge_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_start (edge_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_stop (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  fd_after = _get_open_fd_count ();
  EXPECT_EQ (fd_before, fd_after);
}

/**
 * @brief Connect to server - the client cannot allocate its own host string.
 */
TEST (edge, connectHostStringAllocFail_n)
{
  nns_edge_h server_h, client_h;
  char *val;
  int ret, port;

  port = nns_edge_get_available_port ();
  ASSERT_GT (port, 0);
  val = nns_edge_strdup_printf ("%d", port);

  ret = nns_edge_create_handle ("temp-server", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &server_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (server_h, _test_edge_event_cb, NULL);
  nns_edge_set_info (server_h, "IP", "127.0.0.1");
  nns_edge_set_info (server_h, "PORT", val);
  nns_edge_set_info (server_h, "CAPS", "test server");
  SAFE_FREE (val);

  ret = nns_edge_create_handle ("temp-client", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (client_h, _test_edge_event_cb, NULL);
  nns_edge_set_info (client_h, "CAPS", "test client");

  ret = nns_edge_start (server_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);

  nns_edge_test_host_string_fails = true;
  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  nns_edge_test_host_string_fails = false;
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  /* The handle should still be usable after the failed connection. */
  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Release edge handle - invalid param.
 */
TEST (edge, releaseHandleInvalidParam01_n)
{
  int ret;

  ret = nns_edge_release_handle (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Release edge handle - invalid param.
 */
TEST (edge, releaseHandleInvalidParam02_n)
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
TEST (edge, setEventCbSetNullCallback)
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
TEST (edge, setEventCbInvalidParam01_n)
{
  int ret;

  ret = nns_edge_set_event_callback (NULL, _test_edge_event_cb, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set event callback - invalid param.
 */
TEST (edge, setEventCbInvalidParam02_n)
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
TEST (edge, connectInvalidParam01_n)
{
  int ret;

  ret = nns_edge_connect (NULL, "127.0.0.1", 80);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Connect - invalid param.
 */
TEST (edge, connectInvalidParam02_n)
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
TEST (edge, connectInvalidParam03_n)
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
TEST (edge, connectInvalidParam04_n)
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
TEST (edge, connectInvalidParam05_n)
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
TEST (edge, connectInvalidParam06_n)
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
TEST (edge, disconnectInvalidParam01_n)
{
  int ret;

  ret = nns_edge_disconnect (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Disconnect - invalid param.
 */
TEST (edge, disconnectInvalidParam02_n)
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
TEST (edge, sendInvalidParam01_n)
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
TEST (edge, sendInvalidParam02_n)
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
TEST (edge, sendInvalidParam03_n)
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
TEST (edge, setInfoInvalidParam01_n)
{
  int ret;

  ret = nns_edge_set_info (NULL, "caps", "temp-caps");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info - invalid param.
 */
TEST (edge, setInfoInvalidParam02_n)
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
TEST (edge, setInfoInvalidParam03_n)
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
TEST (edge, setInfoInvalidParam04_n)
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
TEST (edge, setInfoInvalidParam05_n)
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
TEST (edge, setInfoInvalidParam06_n)
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
TEST (edge, setInfoInvalidParam07_n)
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
TEST (edge, setInfoInvalidParam08_n)
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
TEST (edge, setInfoInvalidParam09_n)
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
TEST (edge, getInfo)
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
TEST (edge, getInfoInvalidParam01_n)
{
  char *value = NULL;
  int ret;

  ret = nns_edge_get_info (NULL, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info - invalid param.
 */
TEST (edge, getInfoInvalidParam02_n)
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
TEST (edge, getInfoInvalidParam03_n)
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
TEST (edge, getInfoInvalidParam04_n)
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
TEST (edge, getInfoInvalidParam05_n)
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
TEST (edge, getInfoInvalidParam06_n)
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
TEST (edgeData, clearInfoInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_clear_info (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Clear info of edge-data - invalid param.
 */
TEST (edgeData, clearInfoInvalidParam02_n)
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
TEST (edgeData, clearInfo)
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
TEST (edgeData, createInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_create (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge-data - invalid param.
 */
TEST (edgeData, destroyInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_destroy (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge-data - invalid param.
 */
TEST (edgeData, destroyInvalidParam02_n)
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
TEST (edgeData, validate)
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
TEST (edgeData, validateInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_is_valid (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Validate edge-data - invalid param.
 */
TEST (edgeData, validateInvalidParam02_n)
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
TEST (edgeData, copy)
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
TEST (edgeData, copyInvalidParam01_n)
{
  nns_edge_data_h desc_h;
  int ret;

  ret = nns_edge_data_copy (NULL, &desc_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Copy edge-data - invalid param.
 */
TEST (edgeData, copyInvalidParam02_n)
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
TEST (edgeData, copyInvalidParam03_n)
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
TEST (edgeData, addMaxData_n)
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
TEST (edgeData, addInvalidParam01_n)
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
TEST (edgeData, addInvalidParam02_n)
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
TEST (edgeData, addInvalidParam03_n)
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
TEST (edgeData, addInvalidParam04_n)
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
TEST (edgeData, get)
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
TEST (edgeData, getInvalidParam01_n)
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
TEST (edgeData, getInvalidParam02_n)
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
TEST (edgeData, getInvalidParam03_n)
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
TEST (edgeData, getInvalidParam04_n)
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
TEST (edgeData, getInvalidParam05_n)
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
TEST (edgeData, getCountInvalidParam01_n)
{
  unsigned int count;
  int ret;

  ret = nns_edge_data_get_count (NULL, &count);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get count of edge-data - invalid param.
 */
TEST (edgeData, getCountInvalidParam02_n)
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
TEST (edgeData, getCountInvalidParam03_n)
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
TEST (edgeData, setInfoInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_set_info (NULL, "temp-key", "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set info of edge-data - invalid param.
 */
TEST (edgeData, setInfoInvalidParam02_n)
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
TEST (edgeData, setInfoInvalidParam03_n)
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
TEST (edgeData, setInfoInvalidParam04_n)
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
TEST (edgeData, getInfoInvalidParam01_n)
{
  char *value = NULL;
  int ret;

  ret = nns_edge_data_get_info (NULL, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get info of edge-data - invalid param.
 */
TEST (edgeData, getInfoInvalidParam02_n)
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
TEST (edgeData, getInfoInvalidParam03_n)
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
TEST (edgeData, getInfoInvalidParam04_n)
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
TEST (edgeData, getInfoInvalidParam05_n)
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
TEST (edgeData, serializeInvalidParam01_n)
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
TEST (edgeData, serializeInvalidParam02_n)
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
TEST (edgeData, serializeInvalidParam03_n)
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
TEST (edgeData, serializeInvalidParam04_n)
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
TEST (edgeData, deserializeInvalidParam01_n)
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
TEST (edgeData, deserializeInvalidParam02_n)
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
TEST (edgeData, deserializeInvalidParam03_n)
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
TEST (edgeData, deserializeInvalidParam04_n)
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
 * @brief Deserialize meta to edge-data - invalid param. A malformed metadata
 * blob (nnstreamer/nnstreamer-edge#259) must not crash the TCP receive path.
 */
TEST (edgeData, deserializeInvalidParam05_n)
{
  nns_edge_data_h data_h;
  void *data;
  nns_size_t data_len;
  char *ptr;
  int ret;

  data_len = sizeof (unsigned int) + sizeof ("key") + sizeof ("value");
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 0xFFFFFFFFU;
  ptr = (char *) data + sizeof (unsigned int);
  memcpy (ptr, "key", sizeof ("key"));
  memcpy (ptr + sizeof ("key"), "value", sizeof ("value"));

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize_meta (data_h, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize a valid serialized edge-data whose metadata section was corrupted in place.
 */
TEST (edgeData, deserializeInvalidParam06_n)
{
  nns_edge_data_h src_h, dest_h;
  void *data1, *data2, *serialized_data, *meta_data;
  char *meta_ptr;
  nns_size_t data_len, serialized_len, meta_len;
  unsigned int i, num;
  int ret;

  data_len = 4U * sizeof (unsigned int);
  data1 = malloc (data_len);
  ASSERT_TRUE (data1 != NULL);
  for (i = 0; i < 4U; i++)
    ((unsigned int *) data1)[i] = i;

  data2 = malloc (data_len);
  ASSERT_TRUE (data2 != NULL);
  for (i = 0; i < 4U; i++)
    ((unsigned int *) data2)[i] = 4U - i;

  ret = nns_edge_data_create (&src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (src_h, "temp-key1", "temp-data-val1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_set_info (src_h, "temp-key2", "temp-data-val2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_add (src_h, data1, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (src_h, data2, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize_meta (src_h, &meta_data, &meta_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  SAFE_FREE (meta_data);

  ret = nns_edge_data_serialize (src_h, &serialized_data, &serialized_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ASSERT_TRUE (serialized_len >= meta_len && meta_len >= sizeof (unsigned int));

  /* Only the pair count changes, so the size checks still accept it. */
  meta_ptr = (char *) serialized_data + (serialized_len - meta_len);
  num = 0xFFFFFFFFU;
  memcpy (meta_ptr, &num, sizeof (unsigned int));

  ret = nns_edge_data_create (&dest_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (dest_h, serialized_data, serialized_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (dest_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (serialized_data);
}

/**
 * @brief Deserialize a valid serialized edge-data with an uncorrupted metadata section.
 */
TEST (edgeData, deserializeMetaBoundary)
{
  nns_edge_data_h src_h, dest_h;
  void *data1, *data2, *data3, *serialized_data, *result;
  nns_size_t data_len, serialized_len, result_len;
  char *result_value;
  unsigned int i, result_count;
  int ret;

  data_len = 4U * sizeof (unsigned int);
  data1 = malloc (data_len);
  ASSERT_TRUE (data1 != NULL);
  for (i = 0; i < 4U; i++)
    ((unsigned int *) data1)[i] = i;

  data2 = malloc (data_len);
  ASSERT_TRUE (data2 != NULL);
  for (i = 0; i < 4U; i++)
    ((unsigned int *) data2)[i] = 4U - i;

  ret = nns_edge_data_create (&src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_set_info (src_h, "temp-key1", "temp-data-val1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_set_info (src_h, "temp-key2", "temp-data-val2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* The odd-sized memory leaves the metadata section unaligned. */
  data3 = malloc (3U);
  ASSERT_TRUE (data3 != NULL);
  memset (data3, 0x5A, 3U);

  ret = nns_edge_data_add (src_h, data1, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (src_h, data2, data_len, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (src_h, data3, 3U, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_serialize (src_h, &serialized_data, &serialized_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (src_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_create (&dest_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (dest_h, serialized_data, serialized_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_count (dest_h, &result_count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result_count, 3U);

  ret = nns_edge_data_get (dest_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  for (i = 0; i < 4U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i);

  ret = nns_edge_data_get (dest_h, 1, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  for (i = 0; i < 4U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], 4U - i);

  ret = nns_edge_data_get (dest_h, 2, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result_len, 3U);

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
 * @brief Serialize and deserialize the edge-data.
 */
TEST (edgeDataSerialize, normal)
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
TEST (edgeDataSerialize, invalidParam01_n)
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
TEST (edgeData, invalidParam02_n)
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
TEST (edgeData, clearInvalidParam01_n)
{
  int ret;

  ret = nns_edge_data_clear (NULL);
  EXPECT_NE (NNS_EDGE_ERROR_NONE, ret);
}

/**
 * @brief Clear raw data in edge-data - invalid param.
 */
TEST (edgeData, clearInvalidParam02_n)
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
TEST (edgeData, clear)
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
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_add (data_h, data, data_len, nns_edge_free);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);

  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_EQ (NNS_EDGE_ERROR_NONE, ret);
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
TEST (edgeDataSerialize, invalidParam03_n)
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
TEST (edgeDataSerialize, invalidParam04_n)
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
TEST (edgeDataDeserialize, invalidParam01_n)
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
TEST (edgeDataDeserialize, invalidParam02_n)
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
TEST (edgeDataDeserialize, invalidParam03_n)
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
TEST (edgeDataDeserialize, invalidParam04_n)
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
TEST (edgeDataIsSerialized, invalidParam01_n)
{
  int ret;

  ret = nns_edge_data_is_serialized (NULL, 1U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Util to check serialized data - invalid param.
 */
TEST (edgeDataIsSerialized, invalidParam02_n)
{
  void *data;
  int ret;

  data = nns_edge_malloc (sizeof (ne_test_data_header_s));
  ASSERT_TRUE (data != NULL);
  memset (data, 0, sizeof (ne_test_data_header_s));

  /* invalid data key */
  ret = nns_edge_data_is_serialized (data, sizeof (ne_test_data_header_s));
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - invalid param.
 */
TEST (edgeDataIsSerialized, invalidParam03_n)
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
 * @brief Serialize edge-data - the unused part of the header should not leak stack memory.
 */
TEST (edgeDataSerialize, headerIsCleared)
{
  nns_edge_data_h data_h;
  void *data = NULL;
  void *zeros;
  nns_size_t data_len = 0U;
  nns_size_t offset;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /** The memset in the serializer is what makes this pass, the dirty stack only exposes its absence. */
  _dirty_stack ();

  ret = nns_edge_data_serialize (data_h, &data, &data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /** No raw memory and no metadata, the buffer is the header only. */
  ASSERT_EQ (data_len, sizeof (ne_test_data_header_s));

  /** Only key, version and num_mem are set, the rest including padding is zero. */
  offset = offsetof (ne_test_data_header_s, num_mem) + sizeof (uint32_t);
  zeros = nns_edge_malloc (data_len - offset);
  ASSERT_TRUE (zeros != NULL);
  memset (zeros, 0, data_len - offset);

  EXPECT_EQ (0, memcmp ((char *) data + offset, zeros, data_len - offset));

  SAFE_FREE (zeros);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Serialize and deserialize edge-data without metadata.
 */
TEST (edgeDataSerialize, noMetadata)
{
  nns_edge_data_h data_h;
  void *data = NULL;
  void *result = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  nns_size_t result_len = 0U;
  unsigned int count = 0U;
  nns_size_t i;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (data_h, data, data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (count, 1U);

  ret = nns_edge_data_get (data_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result_len, mem_len);
  for (i = 0; i < mem_len; i++)
    EXPECT_EQ (((unsigned char *) result)[i], 0x5AU);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge-data into a handle that is not empty.
 */
TEST (edgeDataDeserialize, replaceHandleContent)
{
  nns_edge_data_h data_h;
  void *data = NULL;
  void *mem = NULL;
  void *result = NULL;
  char *result_value = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  nns_size_t result_len = 0U;
  unsigned int count = 0U;
  unsigned int i;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /** Fill the handle, deserialize should release these before overwriting them. */
  for (i = 0; i < 3U; i++) {
    mem = nns_edge_malloc (128U);
    ASSERT_TRUE (mem != NULL);
    memset (mem, 0x11, 128U);

    ret = nns_edge_data_add (data_h, mem, 128U, nns_edge_free);
    EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  }

  ret = nns_edge_data_set_info (data_h, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (data_h, data, data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (count, 1U);

  ret = nns_edge_data_get (data_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result_len, mem_len);

  /** The metadata of the previous content should be gone as well. */
  ret = nns_edge_data_get_info (data_h, "temp-key", &result_value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  SAFE_FREE (result_value);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Copy edge-data which was filled by deserialize.
 */
TEST (edgeDataDeserialize, copyDeserialized)
{
  nns_edge_data_h data_h, copied_h;
  void *data = NULL;
  void *result = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  nns_size_t result_len = 0U;
  unsigned int count = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (data_h, data, data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_copy (data_h, &copied_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_get_count (copied_h, &count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (count, 1U);

  ret = nns_edge_data_get (copied_h, 0, &result, &result_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (result_len, mem_len);

  ret = nns_edge_data_destroy (copied_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge-data - buffer shorter than the header.
 */
TEST (edgeDataDeserialize, invalidParam05_n)
{
  nns_edge_data_h data_h;
  void *data;
  int ret;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  data = nns_edge_malloc (1U);
  ASSERT_TRUE (data != NULL);
  memset (data, 0, 1U);

  ret = nns_edge_data_deserialize (data_h, data, 1U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Deserialize edge-data - the memory sizes in the header overflow.
 */
TEST (edgeDataDeserialize, invalidParam06_n)
{
  nns_edge_data_h data_h;
  ne_test_data_header_s *header;
  void *data = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  header = (ne_test_data_header_s *) data;
  header->num_mem = 2U;
  header->data_len[0] = 0xFFFFFFFFFFFFFF00ULL;
  header->data_len[1] = mem_len - header->data_len[0];

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (data_h, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - buffer shorter than the header.
 */
TEST (edgeDataIsSerialized, invalidParam04_n)
{
  void *data;
  int ret;

  data = nns_edge_malloc (1U);
  ASSERT_TRUE (data != NULL);
  memset (data, 0, 1U);

  ret = nns_edge_data_is_serialized (data, 1U);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - valid header truncated by one byte.
 */
TEST (edgeDataIsSerialized, invalidParam05_n)
{
  void *data = NULL;
  void *truncated;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  nns_size_t truncated_len;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  truncated_len = sizeof (ne_test_data_header_s) - 1U;
  truncated = nns_edge_memdup (data, truncated_len);
  ASSERT_TRUE (truncated != NULL);

  ret = nns_edge_data_is_serialized (truncated, truncated_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (truncated);
  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - the memory sizes in the header overflow.
 */
TEST (edgeDataIsSerialized, invalidParam06_n)
{
  ne_test_data_header_s *header;
  void *data = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  header = (ne_test_data_header_s *) data;
  header->num_mem = 2U;
  header->data_len[0] = 0xFFFFFFFFFFFFFF00ULL;
  header->data_len[1] = mem_len - header->data_len[0];

  ret = nns_edge_data_is_serialized (data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - the header declares an empty memory.
 */
TEST (edgeDataIsSerialized, invalidParam07_n)
{
  ne_test_data_header_s *header;
  void *data = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  header = (ne_test_data_header_s *) data;
  header->num_mem = 2U;
  header->data_len[0] = 0U;
  header->data_len[1] = mem_len;

  ret = nns_edge_data_is_serialized (data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - the metadata does not fit in the buffer.
 */
TEST (edgeDataIsSerialized, invalidParam08_n)
{
  ne_test_data_header_s *header;
  void *data = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  header = (ne_test_data_header_s *) data;
  header->meta_len = 0x100U;

  ret = nns_edge_data_is_serialized (data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - the header declares less than the buffer holds.
 */
TEST (edgeDataIsSerialized, invalidParam09_n)
{
  ne_test_data_header_s *header;
  void *data = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  header = (ne_test_data_header_s *) data;
  header->data_len[0] = mem_len - 1U;

  ret = nns_edge_data_is_serialized (data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - invalid version in the header.
 */
TEST (edgeDataIsSerialized, invalidParam10_n)
{
  ne_test_data_header_s *header;
  void *data = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  header = (ne_test_data_header_s *) data;
  header->version = 0ULL;

  ret = nns_edge_data_is_serialized (data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Util to check serialized data - too many memories in the header.
 */
TEST (edgeDataIsSerialized, invalidParam11_n)
{
  ne_test_data_header_s *header;
  void *data = NULL;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  header = (ne_test_data_header_s *) data;
  header->num_mem = NNS_EDGE_DATA_LIMIT + 1;

  ret = nns_edge_data_is_serialized (data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge-data - the handle is empty when the metadata is broken.
 */
TEST (edgeDataDeserialize, invalidParam07_n)
{
  nns_edge_data_h data_h;
  ne_test_data_header_s *header;
  void *data = NULL;
  void *broken;
  char *meta;
  nns_size_t data_len = 0U;
  nns_size_t mem_len = 0U;
  nns_size_t broken_len;
  nns_size_t meta_len = sizeof (uint32_t) + 3U;
  unsigned int count = 1U;
  int ret;

  _get_serialized_data (&data, &data_len, &mem_len);

  broken_len = data_len + meta_len;
  broken = nns_edge_malloc (broken_len);
  ASSERT_TRUE (broken != NULL);
  memcpy (broken, data, data_len);

  /** One key-value pair with an empty key, which nns_edge_metadata_set() rejects. */
  meta = (char *) broken + data_len;
  ((uint32_t *) meta)[0] = 1U;
  meta[sizeof (uint32_t)] = '\0';
  meta[sizeof (uint32_t) + 1U] = 'v';
  meta[sizeof (uint32_t) + 2U] = '\0';

  header = (ne_test_data_header_s *) broken;
  header->meta_len = meta_len;

  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_data_deserialize (data_h, broken, broken_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  /** The raw memories should not be left behind when the metadata is rejected. */
  ret = nns_edge_data_get_count (data_h, &count);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (count, 0U);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (broken);
  SAFE_FREE (data);
}

/**
 * @brief Create edge event - invalid param.
 */
TEST (edgeEvent, createInvalidParam01_n)
{
  nns_edge_event_h event_h;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_UNKNOWN, &event_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Create edge event - invalid param.
 */
TEST (edgeEvent, createInvalidParam02_n)
{
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge event - invalid param.
 */
TEST (edgeEvent, destroyInvalidParam01_n)
{
  int ret;

  ret = nns_edge_event_destroy (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge event - invalid param.
 */
TEST (edgeEvent, destroyInvalidParam02_n)
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
TEST (edgeEvent, setDataInvalidParam01_n)
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
TEST (edgeEvent, setDataInvalidParam02_n)
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
TEST (edgeEvent, setDataInvalidParam03_n)
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
TEST (edgeEvent, setDataInvalidParam04_n)
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
 * @brief Get edge event data.
 */
TEST (edgeEvent, getData)
{
  nns_edge_event_h event_h;
  void *input = NULL, *output = NULL;
  nns_size_t input_len = 0U, output_len = 0U;
  int ret;

  input_len = 10U * sizeof (int);
  input = malloc (input_len);
  ASSERT_TRUE (input != NULL);

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_set_data (event_h, input, input_len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_get_data (event_h, &output, &output_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_TRUE (input == output);
  EXPECT_TRUE (input_len == output_len);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (input);
}

/**
 * @brief Get edge event data - invalid param.
 */
TEST (edgeEvent, getDataInvalidParam01_n)
{
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_event_get_data (NULL, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event data - invalid param.
 */
TEST (edgeEvent, getDataInvalidParam02_n)
{
  nns_edge_event_h event_h;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_get_data (event_h, NULL, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event data - invalid param.
 */
TEST (edgeEvent, getDataInvalidParam03_n)
{
  nns_edge_event_h event_h;
  void *data;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_get_data (event_h, &data, NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event data - invalid param.
 */
TEST (edgeEvent, getDataInvalidParam04_n)
{
  nns_edge_event_h event_h;
  void *data;
  nns_size_t data_len;
  int ret;

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CUSTOM, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC_DEAD);

  ret = nns_edge_event_get_data (event_h, &data, &data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  nns_edge_handle_set_magic (event_h, NNS_EDGE_MAGIC);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event type.
 */
TEST (edgeEvent, getType)
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
TEST (edgeEvent, getTypeInvalidParam01_n)
{
  nns_edge_event_e event;
  int ret;

  ret = nns_edge_event_get_type (NULL, &event);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge event type - invalid param.
 */
TEST (edgeEvent, getTypeInvalidParam02_n)
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
TEST (edgeEvent, getTypeInvalidParam03_n)
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
TEST (edgeEvent, parseNewData)
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
TEST (edgeEvent, parseNewDataInvalidParam01_n)
{
  nns_edge_data_h data_h;
  int ret;

  ret = nns_edge_event_parse_new_data (NULL, &data_h);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse new data of edge event - invalid param.
 */
TEST (edgeEvent, parseNewDataInvalidParam02_n)
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
TEST (edgeEvent, parseNewDataInvalidParam03_n)
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
TEST (edgeEvent, parseNewDataInvalidParam04_n)
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
TEST (edgeEvent, parseCapability)
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
 * @brief Parse capability of edge event - the data is not null-terminated.
 */
TEST (edgeEvent, parseCapabilityNotTerminated)
{
  const char capability[] = "temp-capability";
  nns_edge_event_h event_h;
  char *data, *caps = NULL;
  size_t len;
  int ret;

  len = strlen (capability);
  data = (char *) malloc (len);
  ASSERT_TRUE (data != NULL);
  memcpy (data, capability, len);

  ret = nns_edge_event_create (NNS_EDGE_EVENT_CAPABILITY, &event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_event_set_data (event_h, data, len, NULL);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /** The parser is bounded by the length, so it stops at the end of the data. */
  ret = nns_edge_event_parse_capability (event_h, &caps);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (caps, capability);
  SAFE_FREE (caps);

  ret = nns_edge_event_destroy (event_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  free (data);
}

/**
 * @brief Parse capability of edge event - invalid param.
 */
TEST (edgeEvent, parseCapabilityInvalidParam01_n)
{
  char *caps = NULL;
  int ret;

  ret = nns_edge_event_parse_capability (NULL, &caps);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Parse capability of edge event - invalid param.
 */
TEST (edgeEvent, parseCapabilityInvalidParam02_n)
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
TEST (edgeEvent, parseCapabilityInvalidParam03_n)
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
TEST (edgeEvent, parseCapabilityInvalidParam04_n)
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
TEST (edgeMeta, createInvalidParam01_n)
{
  int ret;

  ret = nns_edge_metadata_create (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Destroy edge metadata - invalid param.
 */
TEST (edgeMeta, destroyInvalidParam01_n)
{
  int ret;

  ret = nns_edge_metadata_destroy (NULL);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge metadata - invalid param.
 */
TEST (edgeMeta, setInvalidParam01_n)
{
  int ret;

  ret = nns_edge_metadata_set (NULL, "temp-key", "temp-value");
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Set edge metadata - invalid param.
 */
TEST (edgeMeta, setInvalidParam02_n)
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
TEST (edgeMeta, setInvalidParam03_n)
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
TEST (edgeMeta, setInvalidParam04_n)
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
TEST (edgeMeta, setInvalidParam05_n)
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
TEST (edgeMeta, getInvalidParam01_n)
{
  char *value = NULL;
  int ret;

  ret = nns_edge_metadata_get (NULL, "temp-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Get edge metadata - invalid param.
 */
TEST (edgeMeta, getInvalidParam02_n)
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
TEST (edgeMeta, getInvalidParam03_n)
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
TEST (edgeMeta, getInvalidParam04_n)
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
TEST (edgeMeta, copy)
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
TEST (edgeMeta, copyInvalidParam01_n)
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
TEST (edgeMeta, copyInvalidParam02_n)
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
TEST (edgeMeta, serialize)
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
TEST (edgeMeta, serializeInvalidParam01_n)
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
TEST (edgeMeta, serializeInvalidParam02_n)
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
TEST (edgeMeta, serializeInvalidParam03_n)
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
TEST (edgeMeta, deserializeInvalidParam01_n)
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
TEST (edgeMeta, deserializeInvalidParam02_n)
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
TEST (edgeMeta, deserializeInvalidParam03_n)
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
 * @brief Deserialize edge metadata - invalid param. Header shorter than the pair count field.
 */
TEST (edgeMeta, deserializeInvalidParam04_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  unsigned int i;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  for (i = 1U; i <= 3U; i++) {
    data_len = i;
    data = malloc (data_len);
    ASSERT_TRUE (data != NULL);
    memset (data, 0, data_len);

    ret = nns_edge_metadata_deserialize (meta, data, data_len);
    EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

    SAFE_FREE (data);
  }

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Deserialize edge metadata - invalid param. A peer-supplied pair
 * count of 0xFFFFFFFF combined with the old `||` loop condition let the
 * parser walk past the end of the buffer (nnstreamer/nnstreamer-edge#259).
 */
TEST (edgeMeta, deserializeInvalidParam05_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  char *ptr;
  int ret;

  data_len = sizeof (unsigned int) + sizeof ("key") + sizeof ("value");
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 0xFFFFFFFFU;
  ptr = (char *) data + sizeof (unsigned int);
  memcpy (ptr, "key", sizeof ("key"));
  memcpy (ptr + sizeof ("key"), "value", sizeof ("value"));

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata - invalid param. Key string has no terminating NUL.
 */
TEST (edgeMeta, deserializeInvalidParam06_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  char *ptr;
  int ret;

  data_len = sizeof (unsigned int) + 3U;
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 1U;
  ptr = (char *) data + sizeof (unsigned int);
  memcpy (ptr, "key", 3U);

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata - invalid param. Value string has no terminating NUL.
 */
TEST (edgeMeta, deserializeInvalidParam07_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  char *ptr;
  int ret;

  data_len = sizeof (unsigned int) + sizeof ("key") + 5U;
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 1U;
  ptr = (char *) data + sizeof (unsigned int);
  memcpy (ptr, "key", sizeof ("key"));
  memcpy (ptr + sizeof ("key"), "value", 5U);

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata - invalid param. Declared pair count exceeds the pairs present in the buffer.
 */
TEST (edgeMeta, deserializeInvalidParam08_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  char *ptr;
  int ret;

  data_len = sizeof (unsigned int) + sizeof ("key") + sizeof ("value");
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 3U;
  ptr = (char *) data + sizeof (unsigned int);
  memcpy (ptr, "key", sizeof ("key"));
  memcpy (ptr + sizeof ("key"), "value", sizeof ("value"));

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata - invalid param. Pair count of 0 with trailing bytes left unconsumed.
 */
TEST (edgeMeta, deserializeInvalidParam09_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  int ret;

  data_len = sizeof (unsigned int) + 4U;
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 0U;
  memset ((char *) data + sizeof (unsigned int), 'A', 4U);

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata - invalid param. Empty key is rejected by nns_edge_metadata_set().
 */
TEST (edgeMeta, deserializeInvalidParam10_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  char *ptr;
  int ret;

  data_len = sizeof (unsigned int) + 1U + sizeof ("value");
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 1U;
  ptr = (char *) data + sizeof (unsigned int);
  ptr[0] = '\0';
  memcpy (ptr + 1U, "value", sizeof ("value"));

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata - invalid param. Duplicate keys cannot reach the declared pair count.
 */
TEST (edgeMeta, deserializeInvalidParam11_n)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  char *ptr;
  int ret;

  data_len = sizeof (unsigned int) + 2U * (sizeof ("dup") + sizeof ("val1"));
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);

  ((unsigned int *) data)[0] = 2U;
  ptr = (char *) data + sizeof (unsigned int);
  memcpy (ptr, "dup", sizeof ("dup"));
  ptr += sizeof ("dup");
  memcpy (ptr, "val1", sizeof ("val1"));
  ptr += sizeof ("val1");
  memcpy (ptr, "dup", sizeof ("dup"));
  ptr += sizeof ("dup");
  memcpy (ptr, "val2", sizeof ("val2"));

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief Deserialize edge metadata with a zero pair count.
 */
TEST (edgeMeta, deserializeEmpty)
{
  nns_edge_metadata_h meta;
  void *data;
  nns_size_t data_len;
  char *value;
  int ret;

  data_len = sizeof (unsigned int);
  data = malloc (data_len);
  ASSERT_TRUE (data != NULL);
  ((unsigned int *) data)[0] = 0U;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, data, data_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (meta, "any-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (data);
}

/**
 * @brief A failed deserialize empties the handle and leaves it usable, and a well-formed buffer deserializes correctly.
 */
TEST (edgeMeta, deserializeRecovery)
{
  nns_edge_metadata_h meta;
  void *malformed, *serialized, *truncated;
  nns_size_t malformed_len, serialized_len, truncated_len;
  char *value;
  int ret;

  ret = nns_edge_metadata_create (&meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (meta, "stale-key", "stale-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  truncated_len = 2U;
  truncated = malloc (truncated_len);
  ASSERT_TRUE (truncated != NULL);
  memset (truncated, 0, truncated_len);

  ret = nns_edge_metadata_deserialize (meta, truncated, truncated_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  SAFE_FREE (truncated);

  ret = nns_edge_metadata_get (meta, "stale-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (meta, "stale-key", "stale-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  malformed_len = sizeof (unsigned int) + 3U;
  malformed = malloc (malformed_len);
  ASSERT_TRUE (malformed != NULL);
  ((unsigned int *) malformed)[0] = 1U;
  memcpy ((char *) malformed + sizeof (unsigned int), "key", 3U);

  ret = nns_edge_metadata_deserialize (meta, malformed, malformed_len);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  SAFE_FREE (malformed);

  ret = nns_edge_metadata_get (meta, "stale-key", &value);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_set (meta, "temp-key", "temp-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (meta, "temp-key", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value");
  SAFE_FREE (value);

  ret = nns_edge_metadata_set (meta, "key1", "value1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_metadata_set (meta, "key2", "value2");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_serialize (meta, &serialized, &serialized_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_deserialize (meta, serialized, serialized_len);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_metadata_get (meta, "temp-key", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "temp-value");
  SAFE_FREE (value);

  ret = nns_edge_metadata_get (meta, "key1", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "value1");
  SAFE_FREE (value);

  ret = nns_edge_metadata_get (meta, "key2", &value);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (value, "value2");
  SAFE_FREE (value);

  ret = nns_edge_metadata_destroy (meta);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (serialized);
}

/**
 * @brief Class to set up and tear down queue testing
 */
class edgeQueue : public ::testing::Test
{
  protected:
  virtual void SetUp () override
  {
    EXPECT_EQ (nns_edge_queue_create (&queue_h), NNS_EDGE_ERROR_NONE);
  }

  virtual void TearDown () override
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

    EXPECT_EQ (nns_edge_queue_push (queue_h, data, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  }

  return NULL;
}

/**
 * @brief Push and pop data.
 */
TEST_F (edgeQueue, pushData)
{
  void *data1, *data2, *data3, *result;
  nns_size_t dsize, rsize;
  unsigned int i, len = 0U;

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
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 1U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data2, dsize, NULL), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 2U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data3, dsize, NULL), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 3U);

  rsize = 0U;
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &result, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 2U);
  EXPECT_EQ (result, data1);
  EXPECT_EQ (dsize, rsize);
  for (i = 0; i < 5U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i + 10U);

  rsize = 0U;
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &result, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 1U);
  EXPECT_EQ (result, data2);
  EXPECT_EQ (dsize, rsize);
  for (i = 0; i < 5U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i + 20U);

  rsize = 0U;
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &result, &rsize), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 0U);
  EXPECT_EQ (result, data3);
  EXPECT_EQ (dsize, rsize);
  for (i = 0; i < 5U; i++)
    EXPECT_EQ (((unsigned int *) result)[i], i + 30U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data1, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 1U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data2, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 2U);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data3, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 3U);
}

/**
 * @brief Wait for pushing data.
 */
TEST_F (edgeQueue, pushDataOnThread)
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

  len = retry = 0U;
  do {
    usleep (20000);
    EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  } while (len < 3U && retry++ < 200U);
}

/**
 * @brief Create queue - invalid param.
 */
TEST_F (edgeQueue, createInvalidParam01_n)
{
  EXPECT_EQ (nns_edge_queue_create (NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Destroy queue - invalid param.
 */
TEST_F (edgeQueue, destroyInvalidParam01_n)
{
  EXPECT_EQ (nns_edge_queue_destroy (NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Destroy queue - invalid param.
 */
TEST_F (edgeQueue, destroyInvalidParam02_n)
{
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC_DEAD);
  EXPECT_EQ (nns_edge_queue_destroy (queue_h), NNS_EDGE_ERROR_INVALID_PARAMETER);
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC);
}

/**
 * @brief Get length of queue - invalid param.
 */
TEST_F (edgeQueue, getLengthInvalidParam01_n)
{
  unsigned int len = 0U;

  EXPECT_EQ (nns_edge_queue_get_length (NULL, &len), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Get length of queue - invalid param.
 */
TEST_F (edgeQueue, getLengthInvalidParam02_n)
{
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Get length of queue - invalid param.
 */
TEST_F (edgeQueue, getLengthInvalidParam03_n)
{
  unsigned int len = 0U;

  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC_DEAD);
  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_INVALID_PARAMETER);
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC);
}

/**
 * @brief Set limit of queue.
 */
TEST_F (edgeQueue, setLimit)
{
  void *data;
  nns_size_t dsize;
  unsigned int i, len = 0U;

  dsize = sizeof (unsigned int);
  data = malloc (dsize);
  ASSERT_TRUE (data != NULL);

  EXPECT_EQ (nns_edge_queue_set_limit (queue_h, 3U, NNS_EDGE_QUEUE_LEAK_NEW), NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 5U; i++)
    nns_edge_queue_push (queue_h, data, dsize, NULL);

  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 3U);

  SAFE_FREE (data);
}

/**
 * @brief Set leaky option of queue.
 */
TEST_F (edgeQueue, setLeaky)
{
  void *data;
  nns_size_t dsize, rsize;
  unsigned int i, len = 0U;
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

  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
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

  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 0U);

  /* leaky option old */
  EXPECT_EQ (nns_edge_queue_set_limit (queue_h, 3U, NNS_EDGE_QUEUE_LEAK_OLD), NNS_EDGE_ERROR_NONE);

  for (i = 0; i < 5U; i++) {
    data = malloc (dsize);
    ASSERT_TRUE (data != NULL);

    *((unsigned int *) data) = i + 1;

    EXPECT_EQ (nns_edge_queue_push (queue_h, data, dsize, nns_edge_free), NNS_EDGE_ERROR_NONE);
  }

  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
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

  EXPECT_EQ (nns_edge_queue_get_length (queue_h, &len), NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (len, 0U);
}

/**
 * @brief Set limit of queue - invalid param.
 */
TEST_F (edgeQueue, setLimitInvalidParam01_n)
{
  EXPECT_EQ (nns_edge_queue_set_limit (NULL, 5U, NNS_EDGE_QUEUE_LEAK_NEW),
      NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Set limit of queue - invalid param.
 */
TEST_F (edgeQueue, setLimitInvalidParam02_n)
{
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC_DEAD);
  EXPECT_EQ (nns_edge_queue_set_limit (queue_h, 5U, NNS_EDGE_QUEUE_LEAK_NEW),
      NNS_EDGE_ERROR_INVALID_PARAMETER);
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC);
}

/**
 * @brief Push data into queue - invalid param.
 */
TEST_F (edgeQueue, pushInvalidParam01_n)
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
TEST_F (edgeQueue, pushInvalidParam02_n)
{
  nns_size_t dsize;

  dsize = 5 * sizeof (unsigned int);
  EXPECT_EQ (nns_edge_queue_push (queue_h, NULL, dsize, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Push data into queue - invalid param.
 */
TEST_F (edgeQueue, pushInvalidParam03_n)
{
  void *data;

  data = malloc (5 * sizeof (unsigned int));
  ASSERT_TRUE (data != NULL);

  EXPECT_EQ (nns_edge_queue_push (queue_h, data, 0U, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);

  SAFE_FREE (data);
}

/**
 * @brief Push data into queue - invalid param.
 */
TEST_F (edgeQueue, pushInvalidParam04_n)
{
  void *data;
  nns_size_t dsize;

  dsize = 5 * sizeof (unsigned int);
  data = malloc (dsize);
  ASSERT_TRUE (data != NULL);

  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC_DEAD);
  EXPECT_EQ (nns_edge_queue_push (queue_h, data, dsize, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC);

  SAFE_FREE (data);
}

/**
 * @brief Pop data from queue - invalid param.
 */
TEST_F (edgeQueue, popInvalidParam01_n)
{
  void *data;
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_pop (NULL, &data, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Pop data from queue - invalid param.
 */
TEST_F (edgeQueue, popInvalidParam02_n)
{
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_pop (queue_h, NULL, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Pop data from queue - invalid param.
 */
TEST_F (edgeQueue, popInvalidParam03_n)
{
  void *data;

  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Pop data from queue - invalid param.
 */
TEST_F (edgeQueue, popInvalidParam04_n)
{
  void *data;
  nns_size_t size;

  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC_DEAD);
  EXPECT_EQ (nns_edge_queue_pop (queue_h, &data, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC);
}

/**
 * @brief Wait and pop data from queue, timed out.
 */
TEST_F (edgeQueue, waitPopTimedout)
{
  void *data;
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 10U, &data, &size), NNS_EDGE_ERROR_IO);
}

/**
 * @brief A stopped queue does not wait, whichever order the two threads run in.
 */
TEST_F (edgeQueue, stopWaitBeforeWait)
{
  void *data;
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_stop_wait (queue_h), NNS_EDGE_ERROR_NONE);

  /* Would block forever without the stop, as the timeout of 0 is infinite. */
  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 0U, &data, &size), NNS_EDGE_ERROR_IO);
}

/**
 * @brief Stopping the queue wakes a thread that is already waiting on it.
 */
TEST_F (edgeQueue, stopWaitWhileWaiting)
{
  std::thread waiter ([&] () {
    void *data;
    nns_size_t size;
    EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 0U, &data, &size), NNS_EDGE_ERROR_IO);
  });

  usleep (100000);
  EXPECT_EQ (nns_edge_queue_stop_wait (queue_h), NNS_EDGE_ERROR_NONE);
  waiter.join ();
}

/**
 * @brief Stop waiting for new data in queue - invalid param.
 */
TEST_F (edgeQueue, stopWaitInvalidParam01_n)
{
  EXPECT_EQ (nns_edge_queue_stop_wait (NULL), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Wait and pop data from queue - invalid param.
 */
TEST_F (edgeQueue, waitPopInvalidParam01_n)
{
  void *data;
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_wait_pop (NULL, 10U, &data, &size), NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Wait and pop data from queue - invalid param.
 */
TEST_F (edgeQueue, waitPopInvalidParam02_n)
{
  nns_size_t size;

  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 10U, NULL, &size),
      NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Wait and pop data from queue - invalid param.
 */
TEST_F (edgeQueue, waitPopInvalidParam03_n)
{
  void *data;

  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 10U, &data, NULL),
      NNS_EDGE_ERROR_INVALID_PARAMETER);
}

/**
 * @brief Wait and pop data from queue - invalid param.
 */
TEST_F (edgeQueue, waitPopInvalidParam04_n)
{
  void *data;
  nns_size_t size;

  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC_DEAD);
  EXPECT_EQ (nns_edge_queue_wait_pop (queue_h, 10U, &data, &size),
      NNS_EDGE_ERROR_INVALID_PARAMETER);
  nns_edge_handle_set_magic (queue_h, NNS_EDGE_MAGIC);
}

/**
 * @brief Util to get the version.
 */
TEST (edgeUtil, getVersion)
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
 * @brief Util to parse the host string.
 */
TEST (edgeUtil, parseHostString)
{
  char *host = NULL;
  int port = -1;

  nns_edge_parse_host_string ("192.168.0.1:5001", &host, &port);
  EXPECT_STREQ (host, "192.168.0.1");
  EXPECT_EQ (port, 5001);
  nns_edge_free (host);

  /** A string that starts with ':' yields an empty host, not a null one. */
  nns_edge_parse_host_string (":5001", &host, &port);
  EXPECT_STREQ (host, "");
  EXPECT_EQ (port, 5001);
  nns_edge_free (host);
}

/**
 * @brief Util to parse the host string - null string.
 */
TEST (edgeUtil, parseHostStringInvalidParam01_n)
{
  char *host = (char *) &host;
  int port = -1;

  nns_edge_parse_host_string (NULL, &host, &port);
  EXPECT_TRUE (host == NULL);
  EXPECT_EQ (port, 0);
}

/**
 * @brief Util to parse the host string - no port in the string.
 */
TEST (edgeUtil, parseHostStringInvalidParam02_n)
{
  char *host = (char *) &host;
  int port = -1;

  nns_edge_parse_host_string ("192.168.0.1", &host, &port);
  EXPECT_TRUE (host == NULL);
  EXPECT_EQ (port, 0);
}

/**
 * @brief Util to parse the host string - empty string.
 */
TEST (edgeUtil, parseHostStringInvalidParam03_n)
{
  char *host = (char *) &host;
  int port = -1;

  nns_edge_parse_host_string ("", &host, &port);
  EXPECT_TRUE (host == NULL);
  EXPECT_EQ (port, 0);
}

/**
 * @brief Util to parse the host string - one output param at a time.
 */
TEST (edgeUtil, parseHostStringPartialOutput)
{
  int port = -1;
  char *host = NULL;

  /** The output param that is given is parsed, whichever one it is. */
  nns_edge_parse_host_string ("192.168.0.1:5001", NULL, &port);
  EXPECT_EQ (port, 5001);

  nns_edge_parse_host_string ("192.168.0.1:5001", &host, NULL);
  EXPECT_STREQ (host, "192.168.0.1");
  nns_edge_free (host);

  port = -1;
  nns_edge_parse_host_string ("192.168.0.1", NULL, &port);
  EXPECT_EQ (port, 0);
}

/**
 * @brief Command info on the wire.
 * @note This should be identical to nns_edge_cmd_info_s in nnstreamer-edge-internal.c. A test sending a raw command fails when the two drift apart.
 */
typedef struct {
  uint32_t magic;
  uint32_t cmd;
  uint64_t version;
  int64_t client_id;
  uint32_t num;
  nns_size_t mem_size[NNS_EDGE_DATA_LIMIT];
  nns_size_t meta_size;
} ne_test_cmd_info_s;

/**
 * @brief Command values on the wire, see nns_edge_cmd_e in nnstreamer-edge-internal.c.
 */
#define NE_TEST_CMD_ERROR (0)
#define NE_TEST_CMD_TRANSFER_DATA (1)
#define NE_TEST_CMD_HOST_INFO (2)
#define NE_TEST_CMD_CAPABILITY (3)

/**
 * @brief Send the whole buffer to the socket.
 */
static bool
_test_send_all (int fd, const void *data, size_t size)
{
  size_t sent = 0;
  ssize_t rret;

  while (sent < size) {
    rret = send (fd, (const char *) data + sent, size - sent, MSG_NOSIGNAL);
    if (rret <= 0)
      return false;
    sent += rret;
  }

  return true;
}

/**
 * @brief Receive the whole buffer from the socket.
 */
static bool
_test_recv_all (int fd, void *data, size_t size)
{
  size_t received = 0;
  ssize_t rret;

  while (received < size) {
    rret = recv (fd, (char *) data + received, size - received, 0);
    if (rret <= 0)
      return false;
    received += rret;
  }

  return true;
}

/**
 * @brief Initialize the command info to send to the peer.
 */
static void
_test_cmd_info_init (ne_test_cmd_info_s *info, uint32_t cmd)
{
  memset (info, 0, sizeof (ne_test_cmd_info_s));
  info->magic = NNS_EDGE_MAGIC;
  info->cmd = cmd;
  info->version = nns_edge_generate_version_key ();
  info->client_id = nns_edge_generate_id ();
}

/**
 * @brief Set the timeout of the socket, to fail the test instead of blocking it.
 */
static void
_test_set_socket_timeout (int fd)
{
  struct timeval tv;

  tv.tv_sec = 10;
  tv.tv_usec = 0;
  setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
  setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));
}

/**
 * @brief Start the query server listening on the given port.
 */
static nns_edge_h
_test_start_query_server (int port)
{
  nns_edge_h server_h = NULL;
  char *val;

  if (nns_edge_create_handle ("temp-server", NNS_EDGE_CONNECT_TYPE_TCP,
          NNS_EDGE_NODE_TYPE_QUERY_SERVER, &server_h)
      != NNS_EDGE_ERROR_NONE)
    return NULL;

  val = nns_edge_strdup_printf ("%d", port);
  nns_edge_set_info (server_h, "IP", "127.0.0.1");
  nns_edge_set_info (server_h, "PORT", val);
  nns_edge_set_info (server_h, "CAPS", "test server");
  SAFE_FREE (val);

  if (nns_edge_start (server_h) != NNS_EDGE_ERROR_NONE) {
    nns_edge_release_handle (server_h);
    return NULL;
  }

  usleep (200000);

  return server_h;
}

/**
 * @brief Connect to the query server as a raw client and consume the capability command.
 */
static int
_test_connect_raw_client (int port)
{
  struct sockaddr_in saddr = { 0 };
  ne_test_cmd_info_s info;
  char *caps;
  int fd;

  fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0)
    return -1;

  _test_set_socket_timeout (fd);

  saddr.sin_family = AF_INET;
  saddr.sin_port = htons (port);
  saddr.sin_addr.s_addr = inet_addr ("127.0.0.1");

  if (connect (fd, (struct sockaddr *) &saddr, sizeof (saddr)) < 0) {
    close (fd);
    return -1;
  }

  if (!_test_recv_all (fd, &info, sizeof (info))
      || info.cmd != NE_TEST_CMD_CAPABILITY || info.num != 1U) {
    close (fd);
    return -1;
  }

  caps = (char *) malloc (info.mem_size[0]);
  if (!caps || !_test_recv_all (fd, caps, info.mem_size[0])) {
    free (caps);
    close (fd);
    return -1;
  }

  free (caps);

  return fd;
}

/**
 * @brief Send a raw host info command and check that the server drops the connection.
 */
static void
_test_send_host_info (int port, uint32_t num, const char *host_str, size_t host_len)
{
  ne_test_cmd_info_s info;
  char buf[1];
  int fd;

  fd = _test_connect_raw_client (port);
  ASSERT_TRUE (fd >= 0);

  _test_cmd_info_init (&info, NE_TEST_CMD_HOST_INFO);
  info.num = num;
  if (num > 0)
    info.mem_size[0] = host_len;

  EXPECT_TRUE (_test_send_all (fd, &info, sizeof (info)));
  if (num > 0)
    EXPECT_TRUE (_test_send_all (fd, host_str, host_len));

  /** The server reports the error and closes instead of parsing the string. */
  EXPECT_TRUE (_test_recv_all (fd, &info, sizeof (info)));
  EXPECT_EQ (info.cmd, (uint32_t) NE_TEST_CMD_ERROR);
  EXPECT_EQ (recv (fd, buf, sizeof (buf), 0), 0);

  close (fd);
}

/**
 * @brief Host info command carrying a string that is not null-terminated.
 */
TEST (edge, hostInfoNotTerminated_n)
{
  nns_edge_h server_h;
  const char *host_str = "127.0.0.1:1234";
  int port;

  port = nns_edge_get_available_port ();
  server_h = _test_start_query_server (port);
  ASSERT_TRUE (server_h != NULL);

  _test_send_host_info (port, 1U, host_str, strlen (host_str));

  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Host info command carrying no memory at all.
 */
TEST (edge, hostInfoNoMemory_n)
{
  nns_edge_h server_h;
  int port;

  port = nns_edge_get_available_port ();
  server_h = _test_start_query_server (port);
  ASSERT_TRUE (server_h != NULL);

  _test_send_host_info (port, 0U, NULL, 0);

  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Host info command carrying a string without the port.
 */
TEST (edge, hostInfoNoPort_n)
{
  nns_edge_h server_h;
  const char *host_str = "127.0.0.1";
  int port;

  port = nns_edge_get_available_port ();
  server_h = _test_start_query_server (port);
  ASSERT_TRUE (server_h != NULL);

  _test_send_host_info (port, 1U, host_str, strlen (host_str) + 1);

  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Host info command carrying a string with no host in it.
 */
TEST (edge, hostInfoEmptyHost_n)
{
  nns_edge_h server_h;
  const char *host_str = ":1234";
  int port;

  port = nns_edge_get_available_port ();
  server_h = _test_start_query_server (port);
  ASSERT_TRUE (server_h != NULL);

  _test_send_host_info (port, 1U, host_str, strlen (host_str) + 1);

  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Host info command carrying a port that does not fit in a port number.
 */
TEST (edge, hostInfoInvalidPort_n)
{
  nns_edge_h server_h;
  const char *host_str = "127.0.0.1:70000";
  int port;

  port = nns_edge_get_available_port ();
  server_h = _test_start_query_server (port);
  ASSERT_TRUE (server_h != NULL);

  _test_send_host_info (port, 1U, host_str, strlen (host_str) + 1);

  EXPECT_EQ (nns_edge_release_handle (server_h), NNS_EDGE_ERROR_NONE);
}

/**
 * @brief Data for the raw server thread.
 */
typedef struct {
  int listener_fd;
  size_t caps_len;
  uint32_t expected_cmd;
} ne_test_raw_server_s;

/**
 * @brief Raw server thread, sends the capability command to the connected client.
 */
static void *
_test_raw_server_thread (void *data)
{
  ne_test_raw_server_s *rs = (ne_test_raw_server_s *) data;
  ne_test_cmd_info_s info;
  const char *caps = "test server";
  int fd;

  fd = accept (rs->listener_fd, NULL, NULL);
  if (fd < 0)
    return NULL;

  _test_set_socket_timeout (fd);

  _test_cmd_info_init (&info, NE_TEST_CMD_CAPABILITY);
  info.num = 1U;
  info.mem_size[0] = rs->caps_len;

  if (_test_send_all (fd, &info, sizeof (info)) && _test_send_all (fd, caps, rs->caps_len)) {
    /** The client answers with the host info, or with an error if it refused. */
    if (_test_recv_all (fd, &info, sizeof (info)))
      EXPECT_EQ (info.cmd, rs->expected_cmd);
  }

  close (fd);

  return NULL;
}

/**
 * @brief Count the capability events delivered to the application.
 */
static int
_test_capability_event_cb (nns_edge_event_h event_h, void *user_data)
{
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  unsigned int *received = (unsigned int *) user_data;
  char *caps = NULL;

  if (nns_edge_event_get_type (event_h, &event) != NNS_EDGE_ERROR_NONE)
    return NNS_EDGE_ERROR_NONE;

  if (event == NNS_EDGE_EVENT_CAPABILITY) {
    if (nns_edge_event_parse_capability (event_h, &caps) == NNS_EDGE_ERROR_NONE) {
      *received = *received + 1;
      SAFE_FREE (caps);
    }
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Capability command carrying a string that is not null-terminated.
 */
TEST (edge, capabilityNotTerminated_n)
{
  nns_edge_h client_h;
  ne_test_raw_server_s rs;
  struct sockaddr_in saddr = { 0 };
  pthread_t server_thread;
  unsigned int received = 0U;
  int port, ret;

  port = nns_edge_get_available_port ();

  rs.listener_fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_TRUE (rs.listener_fd >= 0);
  rs.caps_len = strlen ("test server");
  rs.expected_cmd = NE_TEST_CMD_ERROR;

  saddr.sin_family = AF_INET;
  saddr.sin_port = htons (port);
  saddr.sin_addr.s_addr = inet_addr ("127.0.0.1");
  ASSERT_EQ (bind (rs.listener_fd, (struct sockaddr *) &saddr, sizeof (saddr)), 0);
  ASSERT_EQ (listen (rs.listener_fd, 1), 0);
  ASSERT_EQ (pthread_create (&server_thread, NULL, _test_raw_server_thread, &rs), 0);

  ret = nns_edge_create_handle ("temp-client", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (client_h, _test_capability_event_cb, &received);
  nns_edge_set_info (client_h, "CAPS", "test client");

  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /** The client rejects the command, so it never reports the capability. */
  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_NE (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (received, 0U);

  EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);

  pthread_join (server_thread, NULL);
  close (rs.listener_fd);
}

/**
 * @brief Capability command carrying a null-terminated string.
 */
TEST (edge, capabilityTerminated)
{
  nns_edge_h client_h;
  ne_test_raw_server_s rs;
  struct sockaddr_in saddr = { 0 };
  pthread_t server_thread;
  unsigned int received = 0U;
  int port, ret;

  port = nns_edge_get_available_port ();

  rs.listener_fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  ASSERT_TRUE (rs.listener_fd >= 0);
  rs.caps_len = strlen ("test server") + 1;
  rs.expected_cmd = NE_TEST_CMD_HOST_INFO;

  saddr.sin_family = AF_INET;
  saddr.sin_port = htons (port);
  saddr.sin_addr.s_addr = inet_addr ("127.0.0.1");
  ASSERT_EQ (bind (rs.listener_fd, (struct sockaddr *) &saddr, sizeof (saddr)), 0);
  ASSERT_EQ (listen (rs.listener_fd, 1), 0);
  ASSERT_EQ (pthread_create (&server_thread, NULL, _test_raw_server_thread, &rs), 0);

  ret = nns_edge_create_handle ("temp-client", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (client_h, _test_capability_event_cb, &received);
  nns_edge_set_info (client_h, "CAPS", "test client");

  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (received, 1U);

  EXPECT_EQ (nns_edge_release_handle (client_h), NNS_EDGE_ERROR_NONE);

  pthread_join (server_thread, NULL);
  close (rs.listener_fd);
}

#define NE_TEST_PEER_MEMS (4U)

/**
 * @brief Scripted peer that talks the wire protocol to a real edge handle.
 */
typedef struct {
  int port;
  int listen_fd;
  nns_size_t cap_size; /**< Announced capability size, zero for the real length. */
  bool send_data;
  unsigned int num;
  nns_size_t mem_size[NE_TEST_PEER_MEMS]; /**< Announced size of each memory. */
  nns_size_t mem_actual[NE_TEST_PEER_MEMS]; /**< Bytes actually written for each memory. */
  nns_size_t meta_size; /**< Announced metadata size. */
  void *meta; /**< Serialized metadata, owned by the caller. */
  nns_size_t meta_actual;
  unsigned int answer_timeout_ms; /**< Give up waiting for the answer, 0 to wait. */
  bool answer_timed_out;
  bool answered;
  unsigned int answer_cmd; /**< Command the node answered the capability with. */
  bool sent_data;
  bool stop;
  pthread_t thread;
} ne_test_peer_s;

/**
 * @brief What a subscriber saw while the scripted peer was talking to it.
 */
typedef struct {
  unsigned int received;
  unsigned int closed;
  unsigned int count;
  nns_size_t len[NE_TEST_PEER_MEMS];
  bool payload_ok;
  char *meta_value;
} ne_test_recv_s;

/**
 * @brief Byte the peer writes at the given offset of the given memory.
 */
static uint8_t
_test_peer_byte (unsigned int index, nns_size_t offset)
{
  return (uint8_t) (index * 31U + offset);
}

/**
 * @brief Write a memory of the agreed pattern, in chunks so a large one is cheap to build.
 */
static bool
_test_peer_send_pattern (int fd, unsigned int index, nns_size_t size)
{
  uint8_t chunk[4096];
  nns_size_t sent = 0, len, i;

  while (sent < size) {
    len = (size - sent) < sizeof (chunk) ? (size - sent) : sizeof (chunk);
    for (i = 0; i < len; i++)
      chunk[i] = _test_peer_byte (index, sent + i);

    if (!_test_send_all (fd, chunk, (size_t) len))
      return false;
    sent += len;
  }

  return true;
}

/**
 * @brief Run the peer: hand out a capability, take the answer, then send one data command.
 */
static void *
_test_peer_thread (void *data)
{
  ne_test_peer_s *peer = (ne_test_peer_s *) data;
  ne_test_cmd_info_s info;
  const char *caps = "test peer";
  char buf[256];
  unsigned int i;
  int fd;

  fd = accept (peer->listen_fd, NULL, NULL);
  if (fd < 0)
    return NULL;

  _test_set_socket_timeout (fd);

  _test_cmd_info_init (&info, NE_TEST_CMD_CAPABILITY);
  info.num = 1U;
  info.mem_size[0] = peer->cap_size > 0 ? peer->cap_size : strlen (caps) + 1;

  if (!_test_send_all (fd, &info, sizeof (info)))
    goto done;
  if (!_test_send_all (fd, caps, strlen (caps) + 1))
    goto done;

  /**
   * The answer is host info, or an error command if the capability was refused. A
   * node still waiting for a capability it should have refused never answers, so
   * bound the wait and drop the socket rather than block on that node forever.
   */
  if (peer->answer_timeout_ms > 0) {
    struct pollfd poll_fd;

    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    if (poll (&poll_fd, 1, (int) peer->answer_timeout_ms) <= 0) {
      peer->answer_timed_out = true;
      close (fd);
      fd = -1;
      goto done;
    }
  }

  if (!_test_recv_all (fd, &info, sizeof (info)))
    goto done;

  for (i = 0; i < info.num && i < NNS_EDGE_DATA_LIMIT; i++) {
    if (info.mem_size[i] > sizeof (buf))
      goto done;
    if (!_test_recv_all (fd, buf, (size_t) info.mem_size[i]))
      goto done;
  }
  peer->answered = true;
  peer->answer_cmd = info.cmd;

  if (peer->send_data) {
    _test_cmd_info_init (&info, NE_TEST_CMD_TRANSFER_DATA);
    info.num = peer->num;
    for (i = 0; i < peer->num && i < NE_TEST_PEER_MEMS; i++)
      info.mem_size[i] = peer->mem_size[i];
    info.meta_size = peer->meta_size;

    if (!_test_send_all (fd, &info, sizeof (info)))
      goto done;

    for (i = 0; i < peer->num && i < NE_TEST_PEER_MEMS; i++) {
      if (!_test_peer_send_pattern (fd, i, peer->mem_actual[i]))
        goto done;
    }

    if (peer->meta_actual > 0 && !_test_send_all (fd, peer->meta, (size_t) peer->meta_actual))
      goto done;

    peer->sent_data = true;
  }

done:
  while (!peer->stop)
    usleep (10000);

  if (fd >= 0)
    close (fd);
  return NULL;
}

/**
 * @brief Open the listening socket of the scripted peer and run its thread.
 */
static bool
_test_peer_start (ne_test_peer_s *peer)
{
  struct sockaddr_in addr;
  int reuse = 1;

  peer->listen_fd = socket (AF_INET, SOCK_STREAM, 0);
  if (peer->listen_fd < 0)
    return false;

  setsockopt (peer->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse));

  memset (&addr, 0, sizeof (addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr ("127.0.0.1");
  addr.sin_port = htons (peer->port);

  if (bind (peer->listen_fd, (struct sockaddr *) &addr, sizeof (addr)) < 0
      || listen (peer->listen_fd, 1) < 0) {
    close (peer->listen_fd);
    peer->listen_fd = -1;
    return false;
  }

  if (pthread_create (&peer->thread, NULL, _test_peer_thread, peer) != 0) {
    close (peer->listen_fd);
    peer->listen_fd = -1;
    return false;
  }

  return true;
}

/**
 * @brief Stop the scripted peer and close its listening socket.
 */
static void
_test_peer_stop (ne_test_peer_s *peer)
{
  peer->stop = true;
  pthread_join (peer->thread, NULL);
  close (peer->listen_fd);
  peer->listen_fd = -1;
}

/**
 * @brief Edge event callback recording what a node received.
 */
static int
_test_recv_event_cb (nns_edge_event_h event_h, void *user_data)
{
  ne_test_recv_s *rd = (ne_test_recv_s *) user_data;
  nns_edge_event_e event = NNS_EDGE_EVENT_UNKNOWN;
  nns_edge_data_h data_h;
  void *data;
  char *val = NULL;
  nns_size_t len, j;
  unsigned int i;

  if (!rd || nns_edge_event_get_type (event_h, &event) != NNS_EDGE_ERROR_NONE)
    return NNS_EDGE_ERROR_NONE;

  switch (event) {
    case NNS_EDGE_EVENT_NEW_DATA_RECEIVED:
      if (nns_edge_event_parse_new_data (event_h, &data_h) != NNS_EDGE_ERROR_NONE)
        break;

      rd->payload_ok = true;
      nns_edge_data_get_count (data_h, &rd->count);

      for (i = 0; i < rd->count && i < NE_TEST_PEER_MEMS; i++) {
        if (nns_edge_data_get (data_h, i, &data, &len) != NNS_EDGE_ERROR_NONE) {
          rd->payload_ok = false;
          break;
        }

        rd->len[i] = len;
        for (j = 0; j < len; j++) {
          if (((uint8_t *) data)[j] != _test_peer_byte (i, j)) {
            rd->payload_ok = false;
            break;
          }
        }
      }

      if (nns_edge_data_get_info (data_h, "test-meta", &val) == NNS_EDGE_ERROR_NONE) {
        SAFE_FREE (rd->meta_value);
        rd->meta_value = val;
      }

      nns_edge_data_destroy (data_h);
      rd->received++;
      break;
    case NNS_EDGE_EVENT_CONNECTION_CLOSED:
      rd->closed++;
      break;
    default:
      break;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Create a subscriber that connects to the scripted peer.
 */
static nns_edge_h
_test_sub_create (const char *id, ne_test_recv_s *rd, const char *limit)
{
  nns_edge_h edge_h = NULL;

  if (nns_edge_create_handle (id, NNS_EDGE_CONNECT_TYPE_TCP, NNS_EDGE_NODE_TYPE_SUB, &edge_h)
      != NNS_EDGE_ERROR_NONE)
    return NULL;

  nns_edge_set_event_callback (edge_h, _test_recv_event_cb, rd);
  nns_edge_set_info (edge_h, "IP", "127.0.0.1");
  nns_edge_set_info (edge_h, "CAPS", "test sub");

  if (limit)
    nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", limit);

  return edge_h;
}

/**
 * @brief Wait until the subscriber has seen a data event or lost the connection.
 */
static void
_test_wait_event (ne_test_recv_s *rd)
{
  unsigned int retry = 0U;

  do {
    usleep (20000);
    if (rd->received > 0 || rd->closed > 0)
      break;
  } while (retry++ < 200U);

  /* Let the message thread finish with the handle before the test releases it. */
  usleep (100000);
}

/**
 * @brief Build the metadata blob a peer sends along with a data command.
 */
static void *
_test_build_meta (nns_size_t *len)
{
  nns_edge_data_h data_h;
  void *meta = NULL;

  if (nns_edge_data_create (&data_h) != NNS_EDGE_ERROR_NONE)
    return NULL;

  nns_edge_data_set_info (data_h, "test-meta", "from-peer");
  nns_edge_data_serialize_meta (data_h, &meta, len);
  nns_edge_data_destroy (data_h);

  return meta;
}

/**
 * @brief The handshake of a well behaved peer is unaffected by the transfer limit.
 */
TEST (edgeTransfer, capabilityWithinLimit)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-cap-ok", &rd, NULL);
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_EQ (rd.closed, 0U);

  /* Give the message thread time to start before the handle is released. */
  usleep (200000);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_peer_stop (&peer);
  EXPECT_TRUE (peer.answered);
  EXPECT_EQ (peer.answer_cmd, NE_TEST_CMD_HOST_INFO);
  SAFE_FREE (rd.meta_value);
}

/**
 * @brief Data and metadata from a well behaved peer arrive intact under the default limit.
 */
TEST (edgeTransfer, dataWithinLimit)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  nns_size_t meta_len = 0;
  void *meta;
  int ret;

  meta = _test_build_meta (&meta_len);
  ASSERT_TRUE (meta != NULL && meta_len > 0);

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 3U;
  peer.mem_size[0] = peer.mem_actual[0] = 64U;
  peer.mem_size[1] = peer.mem_actual[1] = 1024U;
  peer.mem_size[2] = peer.mem_actual[2] = 3U;
  peer.meta_size = peer.meta_actual = meta_len;
  peer.meta = meta;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-data-ok", &rd, NULL);
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 1U);
  EXPECT_EQ (rd.closed, 0U);
  EXPECT_EQ (rd.count, 3U);
  EXPECT_EQ (rd.len[0], 64U);
  EXPECT_EQ (rd.len[1], 1024U);
  EXPECT_EQ (rd.len[2], 3U);
  EXPECT_TRUE (rd.payload_ok);
  EXPECT_STREQ (rd.meta_value, "from-peer");

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_peer_stop (&peer);
  EXPECT_TRUE (peer.sent_data);
  SAFE_FREE (rd.meta_value);
  nns_edge_free (meta);
}

/**
 * @brief A payload far larger than any realistic frame still passes the default limit.
 */
TEST (edgeTransfer, defaultLimitAcceptsLargeData)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 1U;
  peer.mem_size[0] = peer.mem_actual[0] = 4U * 1024U * 1024U;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-large", &rd, NULL);
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 1U);
  EXPECT_EQ (rd.closed, 0U);
  EXPECT_EQ (rd.len[0], 4U * 1024U * 1024U);
  EXPECT_TRUE (rd.payload_ok);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_peer_stop (&peer);
  SAFE_FREE (rd.meta_value);
}

/**
 * @brief A command whose announced total is exactly the limit is accepted.
 */
TEST (edgeTransfer, dataAtLimit)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  nns_size_t meta_len = 0;
  void *meta;
  char *limit;
  int ret;

  meta = _test_build_meta (&meta_len);
  ASSERT_TRUE (meta != NULL && meta_len > 0);

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 2U;
  peer.mem_size[0] = peer.mem_actual[0] = 500U;
  peer.mem_size[1] = peer.mem_actual[1] = 524U;
  peer.meta_size = peer.meta_actual = meta_len;
  peer.meta = meta;
  ASSERT_TRUE (_test_peer_start (&peer));

  limit = nns_edge_strdup_printf ("%" PRIu64, (uint64_t) (1024U + meta_len));
  edge_h = _test_sub_create ("sub-at-limit", &rd, limit);
  nns_edge_free (limit);
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 1U);
  EXPECT_EQ (rd.closed, 0U);
  EXPECT_EQ (rd.count, 2U);
  EXPECT_TRUE (rd.payload_ok);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_peer_stop (&peer);
  SAFE_FREE (rd.meta_value);
  nns_edge_free (meta);
}

/**
 * @brief A limit of zero restores the unbounded behavior for a node that needs it.
 */
TEST (edgeTransfer, dataUnlimited)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 1U;
  peer.mem_size[0] = peer.mem_actual[0] = 4096U;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-unlimited", &rd, "0");
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 1U);
  EXPECT_EQ (rd.closed, 0U);
  EXPECT_EQ (rd.len[0], 4096U);
  EXPECT_TRUE (rd.payload_ok);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_peer_stop (&peer);
  SAFE_FREE (rd.meta_value);
}

/**
 * @brief A capability larger than the limit is refused before it is allocated.
 */
TEST (edgeTransfer, capabilityOverLimit_n)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.cap_size = 8ULL * 1024ULL * 1024ULL * 1024ULL;
  peer.answer_timeout_ms = 3000U;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-cap-over", &rd, NULL);
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_CONNECTION_FAILURE);
  EXPECT_EQ (rd.received, 0U);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_peer_stop (&peer);
  EXPECT_FALSE (peer.answer_timed_out);
  EXPECT_EQ (peer.answer_cmd, NE_TEST_CMD_ERROR);
  SAFE_FREE (rd.meta_value);
}

/**
 * @brief One byte over the limit is refused, which pins the boundary against dataAtLimit.
 */
TEST (edgeTransfer, dataOverLimit_n)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 1U;
  peer.mem_size[0] = 1025U;
  peer.mem_actual[0] = 0U;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-over", &rd, "1024");
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 0U);
  EXPECT_EQ (rd.closed, 1U);

  _test_peer_stop (&peer);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (rd.meta_value);
}

/**
 * @brief The metadata size counts towards the limit as well.
 */
TEST (edgeTransfer, dataMetaOverLimit_n)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 1U;
  peer.mem_size[0] = peer.mem_actual[0] = 16U;
  peer.meta_size = 4ULL * 1024ULL * 1024ULL * 1024ULL;
  peer.meta_actual = 0U;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-meta-over", &rd, "1024");
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 0U);
  EXPECT_EQ (rd.closed, 1U);

  _test_peer_stop (&peer);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (rd.meta_value);
}

/**
 * @brief Memories that are each within the limit are refused once their sum is not.
 */
TEST (edgeTransfer, dataSumOverLimit_n)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  unsigned int i;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = NE_TEST_PEER_MEMS;
  for (i = 0; i < NE_TEST_PEER_MEMS; i++) {
    peer.mem_size[i] = 300U;
    peer.mem_actual[i] = 0U;
  }
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-sum-over", &rd, "1024");
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 0U);
  EXPECT_EQ (rd.closed, 1U);

  _test_peer_stop (&peer);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (rd.meta_value);
}

/**
 * @brief Sizes that would wrap a 64 bit accumulator back under the limit are refused.
 * @details The receiver rejected this shape before the limit existed too, by failing the
 *          allocation, so this guards the arithmetic of the check rather than the fix.
 */
TEST (edgeTransfer, dataSizeOverflow_n)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 2U;
  peer.mem_size[0] = UINT64_MAX;
  peer.mem_size[1] = 2U;
  peer.mem_actual[0] = peer.mem_actual[1] = 0U;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-overflow", &rd, "1024");
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 0U);
  EXPECT_EQ (rd.closed, 1U);

  _test_peer_stop (&peer);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (rd.meta_value);
}

/**
 * @brief A rejected info value leaves the limit that was already in force alone.
 */
TEST (edgeTransfer, setInfoKeepsLimitOnFailure)
{
  ne_test_peer_s peer;
  ne_test_recv_s rd;
  nns_edge_h edge_h;
  int ret;

  memset (&peer, 0, sizeof (peer));
  memset (&rd, 0, sizeof (rd));
  peer.port = nns_edge_get_available_port ();
  peer.send_data = true;
  peer.num = 1U;
  peer.mem_size[0] = 4096U;
  peer.mem_actual[0] = 0U;
  ASSERT_TRUE (_test_peer_start (&peer));

  edge_h = _test_sub_create ("sub-keep-limit", &rd, "1024");
  ASSERT_TRUE (edge_h != NULL);

  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "not-a-number");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_INVALID_PARAMETER);

  ret = nns_edge_start (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_connect (edge_h, "127.0.0.1", peer.port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  _test_wait_event (&rd);

  EXPECT_EQ (rd.received, 0U);
  EXPECT_EQ (rd.closed, 1U);

  _test_peer_stop (&peer);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (rd.meta_value);
}

/**
 * @brief The limit bounds what a node accepts, not what it may send.
 */
TEST (edgeTransfer, sendOverReceiverLimit_n)
{
  nns_edge_h server_h, client_h;
  ne_test_recv_s rd_server, rd_client;
  nns_edge_data_h data_h;
  void *data;
  char *val;
  int ret, port;

  memset (&rd_server, 0, sizeof (rd_server));
  memset (&rd_client, 0, sizeof (rd_client));
  port = nns_edge_get_available_port ();

  val = nns_edge_strdup_printf ("%d", port);
  ret = nns_edge_create_handle ("limit-server", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &server_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (server_h, _test_recv_event_cb, &rd_server);
  nns_edge_set_info (server_h, "IP", "127.0.0.1");
  nns_edge_set_info (server_h, "PORT", val);
  nns_edge_set_info (server_h, "CAPS", "test server");
  ret = nns_edge_set_info (server_h, "MAX_TRANSFER_SIZE", "1024");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  SAFE_FREE (val);

  ret = nns_edge_create_handle ("limit-client", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (client_h, _test_recv_event_cb, &rd_client);
  nns_edge_set_info (client_h, "IP", "127.0.0.1");
  nns_edge_set_info (client_h, "CAPS", "test client");

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);
  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  usleep (200000);

  data = calloc (1, 4096U);
  ASSERT_TRUE (data != NULL);
  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (data_h, data, 4096U, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* The sender is not limited, so the send itself succeeds. */
  ret = nns_edge_send (client_h, data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  usleep (500000);

  EXPECT_EQ (rd_server.received, 0U);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (rd_server.meta_value);
  SAFE_FREE (rd_client.meta_value);
}

/**
 * @brief The same transfer arrives when it fits the limit of the receiving node.
 */
TEST (edgeTransfer, sendWithinReceiverLimit)
{
  nns_edge_h server_h, client_h;
  ne_test_recv_s rd_server, rd_client;
  nns_edge_data_h data_h;
  void *data;
  char *val;
  unsigned int retry;
  int ret, port;

  memset (&rd_server, 0, sizeof (rd_server));
  memset (&rd_client, 0, sizeof (rd_client));
  port = nns_edge_get_available_port ();

  val = nns_edge_strdup_printf ("%d", port);
  ret = nns_edge_create_handle ("ok-server", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_SERVER, &server_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (server_h, _test_recv_event_cb, &rd_server);
  nns_edge_set_info (server_h, "IP", "127.0.0.1");
  nns_edge_set_info (server_h, "PORT", val);
  nns_edge_set_info (server_h, "CAPS", "test server");
  SAFE_FREE (val);

  ret = nns_edge_create_handle ("ok-client", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &client_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);
  nns_edge_set_event_callback (client_h, _test_recv_event_cb, &rd_client);
  nns_edge_set_info (client_h, "IP", "127.0.0.1");
  nns_edge_set_info (client_h, "CAPS", "test client");

  ret = nns_edge_start (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_start (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  usleep (200000);
  ret = nns_edge_connect (client_h, "127.0.0.1", port);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  usleep (200000);

  data = calloc (1, 4096U);
  ASSERT_TRUE (data != NULL);
  ret = nns_edge_data_create (&data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_data_add (data_h, data, 4096U, nns_edge_free);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_send (client_h, data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  retry = 0U;
  do {
    usleep (100000);
    if (rd_server.received > 0)
      break;
  } while (retry++ < 50U);

  EXPECT_EQ (rd_server.received, 1U);
  EXPECT_EQ (rd_server.len[0], 4096U);

  ret = nns_edge_data_destroy (data_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (client_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_release_handle (server_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  SAFE_FREE (rd_server.meta_value);
  SAFE_FREE (rd_client.meta_value);
}

/**
 * @brief Set the max transfer size with the values an application would use.
 */
TEST (edgeTransfer, setInfoMaxTransferSize)
{
  nns_edge_h edge_h;
  char *val = NULL;
  int ret;

  ret = nns_edge_create_handle ("set-limit", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "0");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "1024");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_set_info (edge_h, "max_transfer_size", "268435456");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);

  /* The new key must not disturb the metadata an application keeps on the handle. */
  ret = nns_edge_set_info (edge_h, "user-key", "user-value");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  ret = nns_edge_get_info (edge_h, "user-key", &val);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
  EXPECT_STREQ (val, "user-value");
  SAFE_FREE (val);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
}

/**
 * @brief A value that is not a plain decimal number is refused.
 */
TEST (edgeTransfer, setInfoMaxTransferSize_n)
{
  nns_edge_h edge_h;
  int ret;

  ret = nns_edge_create_handle ("set-limit-bad", NNS_EDGE_CONNECT_TYPE_TCP,
      NNS_EDGE_NODE_TYPE_QUERY_CLIENT, &edge_h);
  ASSERT_EQ (ret, NNS_EDGE_ERROR_NONE);

  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "-1");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_INVALID_PARAMETER);
  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "abc");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_INVALID_PARAMETER);
  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "1024a");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_INVALID_PARAMETER);
  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", " 1024");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_INVALID_PARAMETER);
  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "1e9");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_INVALID_PARAMETER);
  ret = nns_edge_set_info (edge_h, "MAX_TRANSFER_SIZE", "99999999999999999999999");
  EXPECT_EQ (ret, NNS_EDGE_ERROR_INVALID_PARAMETER);

  ret = nns_edge_release_handle (edge_h);
  EXPECT_EQ (ret, NNS_EDGE_ERROR_NONE);
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
