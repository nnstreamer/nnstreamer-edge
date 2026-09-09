/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-internal.c
 * @date   6 April 2022
 * @brief  Common library to support communication among devices.
 * @see    https://github.com/nnstreamer/nnstreamer
 * @author Gichan Jang <gichan2.jang@samsung.com>
 * @bug    No known bugs except for NYI items
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>

#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge-queue.h"
#include "nnstreamer-edge-metadata.h"
#include "nnstreamer-edge-mqtt.h"
#include "nnstreamer-edge-custom-impl.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/**
 * @brief The maximum length of pending connections to accept socket.
 */
#define N_BACKLOG 10

/**
 * @brief The timeout (in milliseconds) to wait for an available node when the connection is lost.
 * @note This bounds how long a thread releasing the connection waits for its
 *       message thread to notice, it is not a knob to tune the reconnect with.
 */
#define RECONNECT_TIMEOUT_MS 100

/**
 * @brief The default limit for the total bytes a peer may announce in a single command.
 * @details A peer chooses the sizes in nns_edge_cmd_info_s and the receiver allocates them
 *          before any payload arrives, so an unbounded value is a remote denial of service.
 *          Override it with nns_edge_set_info (h, "MAX_TRANSFER_SIZE", ...), zero for no limit.
 */
#define NNS_EDGE_MAX_TRANSFER_SIZE (256U * 1024U * 1024U)

/**
 * @brief The default time (in milliseconds) a receive waits for a peer that went quiet.
 * @details This bounds silence, not a transfer: every byte that arrives starts the wait
 *          again, so a slow link is unaffected while a peer that announces a command and
 *          then stops sending can no longer park the message thread for ever. The message
 *          loop polls before it reads, so there the wait begins only once a command has
 *          started arriving. The capability and host info reads of the handshake are not
 *          polled first, so for those it bounds the wait for the first byte as well.
 *          Override it with nns_edge_set_info (h, "RECV_TIMEOUT", ...), zero to wait.
 */
#define NNS_EDGE_RECV_TIMEOUT_MS (10000U)

/**
 * @brief Data structure for edge handle.
 */
typedef struct
{
  uint32_t magic;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  char *id;
  char *topic;
  nns_edge_connect_type_e connect_type;
  char *host; /**< host name or IP address */
  int port; /**< port number (0~65535, default 0 to get available port.) */
  char *dest_host; /**< destination IP address (broker or target device) */
  int dest_port; /**< destination port number (broker or target device) */
  nns_edge_node_type_e node_type;
  nns_edge_metadata_h metadata;
  bool is_started;

  /* Edge event callback and user data */
  nns_edge_event_cb event_cb;
  void *user_data;

  int64_t client_id;
  char *caps_str;
  nns_size_t max_transfer_size; /**< Max bytes accepted from a peer in one command (0: unlimited). */
  unsigned int recv_timeout_ms; /**< Max time a receive waits for a quiet peer (0: forever). */

  /* list of connection data */
  pthread_mutex_t conn_lock;
  void *connections;

  /* list of connection data waiting for its message thread to be terminated */
  void *closed_connections;
  pthread_mutex_t closed_lock;

  /* socket listener */
  bool listening;
  int listener_fd;
  pthread_t listener_thread;

  /* thread and queue to send data */
  bool sending;
  nns_edge_queue_h send_queue;
  pthread_t send_thread;

  /* MQTT handle */
  void *broker_h;

  /* Data for custom connection */
  nns_edge_custom_connection_h custom_connection_h;
} nns_edge_handle_s;

/**
 * @brief enum for nnstreamer edge query commands.
 * @note A command carrying a string the peer sizes should be listed in _nns_edge_cmd_string_is_valid(), which is what bounds that string.
 * @note These values go on the wire. tests/unittest_nnstreamer-edge.cc mirrors them to talk to a real peer and asserts them, so a member added in the middle fails those tests until they are updated too.
 */
typedef enum
{
  _NNS_EDGE_CMD_ERROR = 0,
  _NNS_EDGE_CMD_TRANSFER_DATA,
  _NNS_EDGE_CMD_HOST_INFO,
  _NNS_EDGE_CMD_CAPABILITY,
  _NNS_EDGE_CMD_END
} nns_edge_cmd_e;

/**
 * @brief Structure for edge command info. It should be fixed size.
 */
typedef struct
{
  uint32_t magic;
  uint32_t cmd; /**< enum for query commands, see nns_edge_cmd_e. */
  uint64_t version;
  int64_t client_id;

  /* memory info */
  uint32_t num;
  nns_size_t mem_size[NNS_EDGE_DATA_LIMIT];
  nns_size_t meta_size;
} nns_edge_cmd_info_s;

/**
 * @brief Structure for edge command and buffers.
 */
typedef struct
{
  nns_edge_cmd_info_s info;
  void *mem[NNS_EDGE_DATA_LIMIT];
  void *meta;
} nns_edge_cmd_s;

/**
 * @brief Data structure for connection data.
 */
typedef struct _nns_edge_conn_data_s nns_edge_conn_data_s;

/**
 * @brief Data structure for edge connection.
 */
typedef struct
{
  char *host;
  int port;
  bool running;
  bool in_use; /**< The send thread transfers on it, protected by the connection lock. */
  pthread_t msg_thread;
  int sockfd;
  nns_size_t max_transfer_size;
  unsigned int recv_timeout_ms;
} nns_edge_conn_s;

/**
 * @brief Data structure for connection data.
 */
struct _nns_edge_conn_data_s
{
  nns_edge_conn_s *src_conn;
  nns_edge_conn_s *sink_conn;
  int64_t id;
  nns_edge_conn_data_s *next;
};

/**
 * @brief Data structure to keep a connection the send thread is using.
 */
typedef struct
{
  int64_t id;
  nns_edge_conn_s *conn;
} nns_edge_conn_ref_s;

/**
 * @brief Structures for thread data of message handling.
 */
typedef struct
{
  nns_edge_handle_s *eh;
  int64_t client_id;
  nns_edge_conn_s *conn;
} nns_edge_thread_data_s;

/**
 * @brief Parse the message received from the MQTT broker and connect to the server directly.
 */
static int _mqtt_hybrid_direct_connection (nns_edge_handle_s * eh,
    unsigned int timeout);

/**
 * @brief Lock to protect the connection list of the edge handle.
 * @note This is a leaf lock. Do not acquire the handle lock, join a thread, or run network I/O, while holding it. Creating a thread is allowed, the connection has to be in the list before its message thread looks for it.
 */
#define nns_edge_conn_lock_init(h) do { pthread_mutex_init (&(h)->conn_lock, NULL); } while (0)
#define nns_edge_conn_lock_destroy(h) do { pthread_mutex_destroy (&(h)->conn_lock); } while (0)
#define nns_edge_conn_lock(h) do { pthread_mutex_lock (&(h)->conn_lock); } while (0)
#define nns_edge_conn_unlock(h) do { pthread_mutex_unlock (&(h)->conn_lock); } while (0)

/**
 * @brief Set socket option. nnstreamer-edge handles TCP connection now.
 */
static void
_set_socket_option (nns_edge_conn_s * conn)
{
  int nodelay = 1;

  /* setting TCP_NODELAY to true in order to avoid packet batching as known as Nagle's algorithm */
  if (setsockopt (conn->sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
          sizeof (int)) < 0)
    nns_edge_logw ("Failed to set TCP delay option.");

  if (conn->recv_timeout_ms > 0) {
    struct timeval tv;

    tv.tv_sec = conn->recv_timeout_ms / 1000U;
    tv.tv_usec = (conn->recv_timeout_ms % 1000U) * 1000U;

    if (setsockopt (conn->sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv,
            sizeof (tv)) < 0)
      nns_edge_logw ("Failed to set the receive timeout.");
  }
}

/**
 * @brief Fill socket address struct from host name and port number.
 */
static bool
_fill_socket_addr (struct sockaddr_in *saddr, const char *host, const int port)
{
  /** @todo handle protocol (ipv4 and ipv6) */
  saddr->sin_family = AF_INET;
  saddr->sin_port = htons (port);

  if ((saddr->sin_addr.s_addr = inet_addr (host)) == INADDR_NONE) {
    int ret;
    char *port_str = NULL;
    struct addrinfo hints;
    struct addrinfo *addrs = NULL;

    memset (&hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (port > 0)
      port_str = nns_edge_strdup_printf ("%d", port);
    ret = getaddrinfo (host, port_str, &hints, &addrs);
    SAFE_FREE (port_str);

    if (ret != 0 || addrs == NULL)
      return false;

    memcpy (saddr, addrs->ai_addr, addrs->ai_addrlen);
    freeaddrinfo (addrs);
  }

  return true;
}

/**
 * @brief Send data to connected socket.
 */
static bool
_send_raw_data (nns_edge_conn_s * conn, void *data, nns_size_t size)
{
  nns_size_t sent = 0;
  nns_ssize_t rret;

  while (sent < size) {
    rret = send (conn->sockfd, (char *) data + sent, size - sent, MSG_NOSIGNAL);

    if (rret <= 0) {
      nns_edge_loge ("Failed to send raw data.");
      return false;
    }

    sent += rret;
  }

  return true;
}

/**
 * @brief Receive data from connected socket.
 */
static bool
_receive_raw_data (nns_edge_conn_s * conn, void *data, nns_size_t size)
{
  nns_size_t received = 0;
  nns_ssize_t rret;

  while (received < size) {
    rret = recv (conn->sockfd, (char *) data + received, size - received, 0);

    if (rret < 0 && errno == EINTR)
      continue;

    if (rret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      nns_edge_loge
          ("Failed to receive raw data, the connected node sent nothing for %u ms.",
          conn->recv_timeout_ms);
      return false;
    }

    if (rret <= 0) {
      nns_edge_loge ("Failed to receive raw data.");
      return false;
    }

    received += rret;
  }

  return true;
}

/**
 * @brief Internal function to check connection.
 */
static bool
_nns_edge_check_connection (nns_edge_conn_s * conn)
{
  struct pollfd poll_fd;
  int n;

  if (!conn || conn->sockfd < 0)
    return false;

  poll_fd.fd = conn->sockfd;
  poll_fd.events = POLLIN | POLLOUT | POLLPRI | POLLERR | POLLHUP;
  poll_fd.revents = 0;

  /** Timeout zero means that the poll() is returned immediately. */
  n = poll (&poll_fd, 1, 0);
  /**
   * Return value zero indicates that the system call timed out.
   * let's skip the check `n == 0` because timeout is set to 0.
   */
  if (n < 0 || poll_fd.revents & (POLLERR | POLLHUP)) {
    nns_edge_logw ("Socket is not available, possibly closed.");
    return false;
  }

  return true;
}

/**
 * @brief initialize edge command.
 */
static void
_nns_edge_cmd_init (nns_edge_cmd_s * cmd, nns_edge_cmd_e c, int64_t cid)
{
  if (!cmd)
    return;

  memset (cmd, 0, sizeof (nns_edge_cmd_s));
  nns_edge_handle_set_magic (&cmd->info, NNS_EDGE_MAGIC);
  cmd->info.cmd = c;
  cmd->info.version = nns_edge_generate_version_key ();
  cmd->info.client_id = cid;
  cmd->info.num = 0;
  cmd->info.meta_size = 0;
}

/**
 * @brief Clear allocated memory in edge command.
 */
static void
_nns_edge_cmd_clear (nns_edge_cmd_s * cmd)
{
  unsigned int i;

  if (!cmd)
    return;

  nns_edge_handle_set_magic (&cmd->info, NNS_EDGE_MAGIC_DEAD);

  for (i = 0; i < cmd->info.num; i++) {
    SAFE_FREE (cmd->mem[i]);
    cmd->info.mem_size[i] = 0U;
  }

  SAFE_FREE (cmd->meta);

  cmd->info.cmd = _NNS_EDGE_CMD_ERROR;
  cmd->info.version = 0;
  cmd->info.client_id = 0;
  cmd->info.num = 0;
  cmd->info.meta_size = 0;
}

/**
 * @brief Validate edge command.
 */
static bool
_nns_edge_cmd_is_valid (nns_edge_cmd_s * cmd)
{
  int command;

  if (!cmd)
    return false;

  command = (int) cmd->info.cmd;

  if (!nns_edge_handle_is_valid (&cmd->info) ||
      (command < 0 || command >= _NNS_EDGE_CMD_END)) {
    return false;
  }

  if (!nns_edge_parse_version_key (cmd->info.version, NULL, NULL, NULL))
    return false;

  /**
   * @todo The number of memories in data.
   * Total number of memories in edge-data should be less than NNS_EDGE_DATA_LIMIT.
   * Fetch nns-edge version info and check allowed memories if NNS_EDGE_DATA_LIMIT is updated.
   */
  if (cmd->info.num > NNS_EDGE_DATA_LIMIT)
    return false;

  return true;
}

/**
 * @brief Check whether the sizes announced by the peer fit in the transfer limit.
 * @details The limit is never exceeded by the running total, so the remaining budget
 *          used as the bound cannot underflow and the sum cannot overflow.
 */
static bool
_nns_edge_cmd_size_is_valid (nns_edge_cmd_s * cmd, nns_size_t limit)
{
  nns_size_t total = 0;
  unsigned int n;

  if (limit == 0U)
    return true;

  for (n = 0; n < cmd->info.num; n++) {
    if (cmd->info.mem_size[n] > limit - total)
      return false;
    total += cmd->info.mem_size[n];
  }

  return cmd->info.meta_size <= limit - total;
}

/**
 * @brief Send edge command to connected device.
 */
static int
_nns_edge_cmd_send (nns_edge_conn_s * conn, nns_edge_cmd_s * cmd)
{
  unsigned int n;

  if (!conn) {
    nns_edge_loge ("Failed to send command, edge connection is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!_nns_edge_cmd_is_valid (cmd)) {
    nns_edge_loge ("Failed to send command, invalid command.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!_nns_edge_check_connection (conn)) {
    nns_edge_loge ("Failed to send command, socket has error.");
    return NNS_EDGE_ERROR_IO;
  }

  if (!_send_raw_data (conn, &cmd->info, sizeof (nns_edge_cmd_info_s))) {
    nns_edge_loge ("Failed to send command to socket.");
    return NNS_EDGE_ERROR_IO;
  }

  for (n = 0; n < cmd->info.num; n++) {
    if (!_send_raw_data (conn, cmd->mem[n], cmd->info.mem_size[n])) {
      nns_edge_loge ("Failed to send %uth memory to socket.", n);
      return NNS_EDGE_ERROR_IO;
    }
  }

  if (cmd->info.meta_size > 0) {
    if (!_send_raw_data (conn, cmd->meta, cmd->info.meta_size)) {
      nns_edge_loge ("Failed to send metadata to socket.");
      return NNS_EDGE_ERROR_IO;
    }
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to check the string carried by the received command.
 * @note HOST_INFO and CAPABILITY carry exactly one null-terminated string, and the peer decides its length. Every other command is accepted here.
 */
static bool
_nns_edge_cmd_string_is_valid (const nns_edge_cmd_s * cmd)
{
  const char *str;

  if (cmd->info.cmd != _NNS_EDGE_CMD_HOST_INFO &&
      cmd->info.cmd != _NNS_EDGE_CMD_CAPABILITY)
    return true;

  if (cmd->info.num != 1U || cmd->info.mem_size[0] == 0 || !cmd->mem[0])
    return false;

  str = (const char *) cmd->mem[0];

  return (str[cmd->info.mem_size[0] - 1] == '\0');
}

/**
 * @brief Internal function to parse the host string a peer sent, from TCP or from the broker.
 * @note This does not assume the buffer is null-terminated. The host is null unless the whole string was usable, so the caller only needs to check the return.
 */
static bool
_nns_edge_parse_peer_host (const char *str, nns_size_t len, char **host,
    int *port)
{
  *host = NULL;
  *port = 0;

  if (!str || len == 0 || str[len - 1] != '\0')
    return false;

  nns_edge_parse_host_string (str, host, port);

  if (!STR_IS_VALID (*host) || !PORT_IS_VALID (*port)) {
    SAFE_FREE (*host);
    return false;
  }

  return true;
}

/**
 * @brief Receive edge command from connected device.
 * @note Before calling this function, you should initialize edge-cmd by using _nns_edge_cmd_init().
 */
static int
_nns_edge_cmd_receive (nns_edge_conn_s * conn, nns_edge_cmd_s * cmd)
{
  unsigned int n;
  int ret = NNS_EDGE_ERROR_NONE;

  if (!conn || !cmd)
    return NNS_EDGE_ERROR_INVALID_PARAMETER;

  if (!_nns_edge_check_connection (conn)) {
    nns_edge_loge ("Failed to receive command, socket has error.");
    return NNS_EDGE_ERROR_IO;
  }

  if (!_receive_raw_data (conn, &cmd->info, sizeof (nns_edge_cmd_info_s))) {
    nns_edge_loge ("Failed to receive command from socket.");
    return NNS_EDGE_ERROR_IO;
  }

  if (!_nns_edge_cmd_is_valid (cmd)) {
    nns_edge_loge ("Failed to receive command, invalid command.");
    return NNS_EDGE_ERROR_IO;
  }

  nns_edge_logd ("Received command:%d (num:%u)", cmd->info.cmd, cmd->info.num);
  if (cmd->info.num >= NNS_EDGE_DATA_LIMIT) {
    nns_edge_loge ("Invalid request, the max memories for data transfer is %d.",
        NNS_EDGE_DATA_LIMIT);
    return NNS_EDGE_ERROR_IO;
  }

  if (!_nns_edge_cmd_size_is_valid (cmd, conn->max_transfer_size)) {
    nns_edge_loge
        ("Invalid request, the max bytes for data transfer is %" PRIu64 ".",
        conn->max_transfer_size);
    return NNS_EDGE_ERROR_IO;
  }

  for (n = 0; n < cmd->info.num; n++) {
    cmd->mem[n] = nns_edge_malloc (cmd->info.mem_size[n]);
    if (!cmd->mem[n]) {
      nns_edge_loge ("Failed to allocate memory to receive data from socket.");
      ret = NNS_EDGE_ERROR_OUT_OF_MEMORY;
      goto error;
    }

    if (!_receive_raw_data (conn, cmd->mem[n], cmd->info.mem_size[n])) {
      nns_edge_loge ("Failed to receive %uth memory from socket.", n++);
      ret = NNS_EDGE_ERROR_IO;
      goto error;
    }
  }

  if (cmd->info.meta_size > 0) {
    cmd->meta = nns_edge_malloc (cmd->info.meta_size);
    if (!cmd->meta) {
      nns_edge_loge ("Failed to allocate memory to receive meta from socket.");
      ret = NNS_EDGE_ERROR_OUT_OF_MEMORY;
      goto error;
    }

    if (!_receive_raw_data (conn, cmd->meta, cmd->info.meta_size)) {
      nns_edge_loge ("Failed to receive metadata from socket.");
      ret = NNS_EDGE_ERROR_IO;
      goto error;
    }
  }

  if (!_nns_edge_cmd_string_is_valid (cmd)) {
    nns_edge_loge ("Failed to receive command, the string in command %u is "
        "invalid.", cmd->info.cmd);
    ret = NNS_EDGE_ERROR_IO;
    goto error;
  }

  return NNS_EDGE_ERROR_NONE;

error:
  _nns_edge_cmd_clear (cmd);
  return ret;
}

/**
 * @brief Internal function to send edge data.
 */
static int
_nns_edge_transfer_data (nns_edge_conn_s * conn, nns_edge_data_h data_h,
    int64_t client_id)
{
  nns_edge_cmd_s cmd;
  unsigned int i;
  int ret;

  if (!conn) {
    nns_edge_loge ("Failed to transfer data, edge connection is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_TRANSFER_DATA, client_id);

  ret = nns_edge_data_get_count (data_h, &cmd.info.num);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to get data count");
    return ret;
  }

  for (i = 0; i < cmd.info.num; i++) {
    ret = nns_edge_data_get (data_h, i, &cmd.mem[i], &cmd.info.mem_size[i]);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to get data");
      return ret;
    }
  }

  ret = nns_edge_data_serialize_meta (data_h, &cmd.meta, &cmd.info.meta_size);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to serialize meta");
    return ret;
  }

  ret = _nns_edge_cmd_send (conn, &cmd);
  SAFE_FREE (cmd.meta);

  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to send edge data to destination (%s:%d).",
        conn->host, conn->port);
  }

  return ret;
}

/**
 * @brief Check whether the message thread of the connection is the caller.
 */
static bool
_nns_edge_conn_is_self (nns_edge_conn_s * conn)
{
  return (conn && conn->msg_thread
      && pthread_equal (conn->msg_thread, pthread_self ()) != 0);
}

/**
 * @brief Send error command to the connected node and close the socket.
 */
static void
_nns_edge_close_socket (nns_edge_conn_s * conn)
{
  nns_edge_cmd_s cmd;

  if (!conn || conn->sockfd < 0)
    return;

  /* Send error before closing the socket. */
  nns_edge_logd ("Send error cmd to close connection.");
  _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_ERROR, 0);
  _nns_edge_cmd_send (conn, &cmd);

  if (close (conn->sockfd) < 0)
    nns_edge_logw ("Failed to close socket.");
  conn->sockfd = -1;
}

/**
 * @brief Close connection.
 * @return True if the connection is released, or there is nothing to release.
 * @note The message thread cannot join and release the connection it is
 *       running on, and the send thread may be transferring on it. In those
 *       cases only the socket is closed and false is returned, then the caller
 *       should keep the connection until that thread is done with it.
 */
static bool
_nns_edge_conn_in_use (nns_edge_handle_s * eh, nns_edge_conn_s * conn)
{
  bool in_use;

  nns_edge_conn_lock (eh);
  in_use = conn->in_use;
  nns_edge_conn_unlock (eh);

  return in_use;
}

/**
 * @brief Close connection.
 * @return True if the connection is released, or there is nothing to release.
 * @note The message thread cannot join and release the connection it is
 *       running on, and the send thread may be transferring on it. In those
 *       cases only the socket is closed and false is returned, then the caller
 *       should keep the connection until that thread is done with it.
 */
static bool
_nns_edge_close_connection (nns_edge_handle_s * eh, nns_edge_conn_s * conn)
{
  if (!conn)
    return true;

  if (_nns_edge_conn_is_self (conn) || _nns_edge_conn_in_use (eh, conn)) {
    _nns_edge_close_socket (conn);
    return false;
  }

  /* Stop and clear the message thread. */
  conn->running = false;
  if (conn->msg_thread) {
    /**
     * Wake the thread if it is blocked in recv(), so that the join waits for
     * the thread rather than for the peer. Only the read side is closed, which
     * keeps the descriptor reserved and still lets the error command below go
     * out on the write side.
     */
    if (conn->sockfd >= 0)
      shutdown (conn->sockfd, SHUT_RD);

    pthread_join (conn->msg_thread, NULL);
    conn->msg_thread = 0;
  }

  _nns_edge_close_socket (conn);

  SAFE_FREE (conn->host);
  SAFE_FREE (conn);
  return true;
}

/**
 * @brief Release connection data and its resources.
 * @return True if the connection data is released, or there is nothing to release.
 */
static bool
_nns_edge_release_connection_data (nns_edge_handle_s * eh,
    nns_edge_conn_data_s * cdata)
{
  bool released = true;

  if (!cdata)
    return true;

  if (!_nns_edge_close_connection (eh, cdata->src_conn))
    released = false;
  else
    cdata->src_conn = NULL;

  if (!_nns_edge_close_connection (eh, cdata->sink_conn))
    released = false;
  else
    cdata->sink_conn = NULL;

  if (released)
    SAFE_FREE (cdata);

  return released;
}

/**
 * @brief Check whether a message thread other than the calling one reads the connection.
 */
static bool
_nns_edge_conn_has_reader (nns_edge_conn_s * conn)
{
  return (conn && conn->msg_thread && !_nns_edge_conn_is_self (conn));
}

/**
 * @brief Stop the message thread of the connection data and close its sockets.
 * @note The sockets are closed after the join when a message thread of this
 *       connection data may still read one of them. Such a thread would poll a
 *       descriptor the system has given to somebody else, and closing the
 *       other socket makes the peer answer with an error command, which the
 *       thread reports as a connection lost by the peer.
 */
static void
_nns_edge_stop_connection (nns_edge_conn_data_s * cdata)
{
  bool reading = false;

  if (!cdata)
    return;

  if (_nns_edge_conn_has_reader (cdata->src_conn)) {
    cdata->src_conn->running = false;
    reading = true;
  }

  if (_nns_edge_conn_has_reader (cdata->sink_conn)) {
    cdata->sink_conn->running = false;
    reading = true;
  }

  if (!reading) {
    _nns_edge_close_socket (cdata->src_conn);
    _nns_edge_close_socket (cdata->sink_conn);
  }
}

/**
 * @brief Hold the connection data until its message thread is terminated.
 * @note The connected node is notified as soon as the connection is removed,
 *       unless the sockets have to be closed after joining a message thread.
 *       The list of closed connections has its own lock, it is also touched by
 *       the listener thread and by the message thread without the handle lock.
 */
static void
_nns_edge_hold_closed_connection (nns_edge_handle_s * eh,
    nns_edge_conn_data_s * cdata)
{
  _nns_edge_stop_connection (cdata);

  pthread_mutex_lock (&eh->closed_lock);
  cdata->next = (nns_edge_conn_data_s *) eh->closed_connections;
  eh->closed_connections = cdata;
  pthread_mutex_unlock (&eh->closed_lock);
}

/**
 * @brief Check whether the connection data is handled by the calling thread.
 */
static bool
_nns_edge_conn_data_is_self (nns_edge_conn_data_s * cdata)
{
  return (_nns_edge_conn_is_self (cdata->src_conn)
      || _nns_edge_conn_is_self (cdata->sink_conn));
}

/**
 * @brief Release the connection data of the terminated message thread.
 * @note Only the connection data that this thread can release is taken out of
 *       the list, so that two threads cannot join and free the same connection
 *       and the connection of the calling thread stays visible to the owner of
 *       the handle. The lock is not held while joining the message thread.
 *       The caller may hold the handle lock, so nothing joined here may take it.
 */
static void
_nns_edge_release_closed_connection (nns_edge_handle_s * eh)
{
  nns_edge_conn_data_s *cdata, *next, *closed = NULL, *remained = NULL;

  pthread_mutex_lock (&eh->closed_lock);
  cdata = (nns_edge_conn_data_s *) eh->closed_connections;

  while (cdata) {
    next = cdata->next;

    if (_nns_edge_conn_data_is_self (cdata)) {
      cdata->next = remained;
      remained = cdata;
    } else {
      cdata->next = closed;
      closed = cdata;
    }

    cdata = next;
  }

  eh->closed_connections = remained;
  pthread_mutex_unlock (&eh->closed_lock);

  while (closed) {
    /* Read the next one first, holding it again overwrites the link. */
    next = closed->next;

    if (!_nns_edge_release_connection_data (eh, closed))
      _nns_edge_hold_closed_connection (eh, closed);

    closed = next;
  }
}

/**
 * @brief Get nnstreamer-edge connection data.
 * @note This function should be called with connection lock.
 */
static nns_edge_conn_data_s *
_nns_edge_get_connection (nns_edge_handle_s * eh, int64_t client_id)
{
  nns_edge_conn_data_s *cdata;

  cdata = (nns_edge_conn_data_s *) eh->connections;

  while (cdata) {
    if (cdata->id == client_id)
      return cdata;

    cdata = cdata->next;
  }

  return NULL;
}

/**
 * @brief Add nnstreamer-edge connection data.
 * @note This function should be called with connection lock.
 */
static nns_edge_conn_data_s *
_nns_edge_add_connection (nns_edge_handle_s * eh, int64_t client_id)
{
  nns_edge_conn_data_s *cdata;

  cdata = _nns_edge_get_connection (eh, client_id);

  if (NULL == cdata) {
    cdata = (nns_edge_conn_data_s *) calloc (1, sizeof (nns_edge_conn_data_s));
    if (NULL == cdata) {
      nns_edge_loge ("Failed to allocate memory for connection data.");
      return NULL;
    }

    /* prepend connection data */
    cdata->id = client_id;
    cdata->next = eh->connections;
    eh->connections = cdata;
  }

  return cdata;
}

/**
 * @brief Remove nnstreamer-edge connection data.
 * @note This function takes the connection lock, do not call it with the lock held.
 */
static void
_nns_edge_remove_connection (nns_edge_handle_s * eh, int64_t client_id)
{
  nns_edge_conn_data_s *cdata, *prev;

  nns_edge_conn_lock (eh);

  cdata = (nns_edge_conn_data_s *) eh->connections;
  prev = NULL;

  while (cdata) {
    if (cdata->id == client_id) {
      if (prev)
        prev->next = cdata->next;
      else
        eh->connections = cdata->next;
      break;
    }
    prev = cdata;
    cdata = cdata->next;
  }

  nns_edge_conn_unlock (eh);

  /* The caller may be the message thread of this connection, release it later. */
  if (cdata)
    _nns_edge_hold_closed_connection (eh, cdata);
}

/**
 * @brief Check whether the handle has a connection left.
 * @note This function takes the connection lock, do not call it with the lock held.
 */
static bool
_nns_edge_has_connection (nns_edge_handle_s * eh)
{
  bool remained;

  nns_edge_conn_lock (eh);
  remained = (eh->connections != NULL);
  nns_edge_conn_unlock (eh);

  return remained;
}

/**
 * @brief Remove all connection data.
 * @note This function takes the connection lock, do not call it with the lock held.
 */
static void
_nns_edge_remove_all_connection (nns_edge_handle_s * eh)
{
  nns_edge_conn_data_s *cdata, *next;

  nns_edge_conn_lock (eh);
  cdata = (nns_edge_conn_data_s *) eh->connections;
  eh->connections = NULL;
  nns_edge_conn_unlock (eh);

  while (cdata) {
    next = cdata->next;

    /* Request the message thread to stop, it may be the calling thread. */
    if (cdata->src_conn)
      cdata->src_conn->running = false;
    if (cdata->sink_conn)
      cdata->sink_conn->running = false;

    /* Hold it before releasing, it should stay reachable from the handle. */
    _nns_edge_hold_closed_connection (eh, cdata);

    cdata = next;
  }

  _nns_edge_release_closed_connection (eh);
}

/**
 * @brief Release a connection taken out of its connection data, or park it.
 * @note A connection its message thread or the send thread is still using
 *       cannot be released here. It no longer belongs to a connection data, so
 *       it is parked with one of its own and released with the others later.
 */
static void
_nns_edge_release_old_connection (nns_edge_handle_s * eh,
    nns_edge_conn_s * conn)
{
  nns_edge_conn_data_s *cdata;

  if (_nns_edge_close_connection (eh, conn))
    return;

  cdata = (nns_edge_conn_data_s *) calloc (1, sizeof (nns_edge_conn_data_s));
  if (!cdata) {
    nns_edge_loge ("Failed to allocate memory to hold the old connection.");
    return;
  }

  cdata->sink_conn = conn;
  _nns_edge_hold_closed_connection (eh, cdata);
}

/**
 * @brief Mark every sink connection in use and return them.
 * @note Caller should release the returned list using _nns_edge_put_sink_connection().
 */
static unsigned int
_nns_edge_hold_sink_connection (nns_edge_handle_s * eh,
    nns_edge_conn_ref_s ** list)
{
  nns_edge_conn_data_s *cdata;
  nns_edge_conn_ref_s *refs = NULL;
  unsigned int i, n = 0;

  nns_edge_conn_lock (eh);

  for (cdata = (nns_edge_conn_data_s *) eh->connections; cdata;
      cdata = cdata->next)
    n++;

  if (n > 0) {
    refs = (nns_edge_conn_ref_s *) calloc (n, sizeof (nns_edge_conn_ref_s));
    if (refs) {
      cdata = (nns_edge_conn_data_s *) eh->connections;
      for (i = 0; i < n && cdata; i++, cdata = cdata->next) {
        refs[i].id = cdata->id;
        refs[i].conn = cdata->sink_conn;
        if (refs[i].conn)
          refs[i].conn->in_use = true;
      }
    } else {
      nns_edge_loge ("Failed to allocate memory for the connection list.");
      n = 0;
    }
  }

  nns_edge_conn_unlock (eh);

  *list = refs;
  return n;
}

/**
 * @brief Release the connections taken by _nns_edge_hold_sink_connection().
 */
static void
_nns_edge_put_sink_connection (nns_edge_handle_s * eh,
    nns_edge_conn_ref_s * list, unsigned int n)
{
  unsigned int i;

  if (!list)
    return;

  nns_edge_conn_lock (eh);
  for (i = 0; i < n; i++) {
    if (list[i].conn)
      list[i].conn->in_use = false;
  }
  nns_edge_conn_unlock (eh);

  SAFE_FREE (list);

  /* A connection kept while it was in use is releasable now. */
  _nns_edge_release_closed_connection (eh);
}

/**
 * @brief Connect to requested socket.
 */
static bool
_nns_edge_connect_socket (nns_edge_conn_s * conn)
{
  struct sockaddr_in saddr = { 0 };
  socklen_t saddr_len = sizeof (struct sockaddr_in);

  if (!_fill_socket_addr (&saddr, conn->host, conn->port)) {
    nns_edge_loge ("Failed to connect socket, invalid host %s.", conn->host);
    return false;
  }

  conn->sockfd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (conn->sockfd < 0) {
    nns_edge_loge ("Failed to create new socket.");
    return false;
  }

  _set_socket_option (conn);

  if (connect (conn->sockfd, (struct sockaddr *) &saddr, saddr_len) < 0) {
    nns_edge_loge ("Failed to connect host %s:%d.", conn->host, conn->port);
    return false;
  }

  return true;
}

/**
 * @brief Message thread, receive buffer from the client.
 */
static void *
_nns_edge_message_handler (void *thread_data)
{
  nns_edge_thread_data_s *_tdata = (nns_edge_thread_data_s *) thread_data;
  nns_edge_handle_s *eh;
  nns_edge_conn_s *conn;
  bool remove_connection = false;
  int64_t client_id;
  int ret;

  if (!_tdata) {
    nns_edge_loge ("Internal error, thread data is null.");
    return NULL;
  }

  eh = (nns_edge_handle_s *) _tdata->eh;
  conn = _tdata->conn;
  client_id = _tdata->client_id;
  SAFE_FREE (_tdata);

  while (conn->running) {
    struct pollfd poll_fd;

    /* Validate edge handle */
    if (!nns_edge_handle_is_valid (eh)) {
      nns_edge_loge ("The edge handle is invalid, it would be expired.");
      break;
    }

    poll_fd.fd = conn->sockfd;
    poll_fd.events = POLLIN | POLLHUP | POLLERR;
    poll_fd.revents = 0;

    /* 10 milliseconds */
    if (poll (&poll_fd, 1, 10) > 0) {
      nns_edge_cmd_s cmd;
      nns_edge_data_h data_h;
      char *val;
      unsigned int i;

      /* Receive data from the client */
      _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_ERROR, client_id);
      ret = _nns_edge_cmd_receive (conn, &cmd);
      if (ret != NNS_EDGE_ERROR_NONE) {
        /**
         * The receive also fails when this node is closing the connection and
         * shut the socket down to wake this thread. That is not the peer going
         * away, so it must not be reported as a lost connection.
         */
        if (!conn->running)
          break;

        nns_edge_loge ("Failed to receive data from the connected node.");
        remove_connection = true;
        break;
      }

      if (cmd.info.cmd == _NNS_EDGE_CMD_ERROR) {
        nns_edge_loge ("Received error, stop msg thread.");
        _nns_edge_cmd_clear (&cmd);
        remove_connection = true;
        break;
      }

      if (cmd.info.cmd != _NNS_EDGE_CMD_TRANSFER_DATA) {
        /** @todo handle other cmd later */
        _nns_edge_cmd_clear (&cmd);
        continue;
      }

      ret = nns_edge_data_create (&data_h);
      if (ret != NNS_EDGE_ERROR_NONE) {
        nns_edge_loge ("Failed to create data handle in msg thread.");
        _nns_edge_cmd_clear (&cmd);
        continue;
      }

      for (i = 0; i < cmd.info.num; i++)
        nns_edge_data_add (data_h, cmd.mem[i], cmd.info.mem_size[i], NULL);

      if (cmd.info.meta_size > 0)
        nns_edge_data_deserialize_meta (data_h, cmd.meta, cmd.info.meta_size);

      /* Set client ID in edge data */
      val = nns_edge_strdup_printf ("%lld", (long long) client_id);
      nns_edge_data_set_info (data_h, "client_id", val);
      SAFE_FREE (val);

      ret = nns_edge_event_invoke_callback (eh->event_cb, eh->user_data,
          NNS_EDGE_EVENT_NEW_DATA_RECEIVED, data_h, sizeof (nns_edge_data_h),
          NULL);
      if (ret != NNS_EDGE_ERROR_NONE) {
        /* Try to get next request if server does not accept data from client. */
        nns_edge_logw ("The server does not accept data from client.");
      }

      nns_edge_data_destroy (data_h);
      _nns_edge_cmd_clear (&cmd);
    }
  }

  /* Received error message from client, remove connection from table. */
  if (remove_connection) {
    nns_edge_loge
        ("Received error from client, remove connection of client (ID: %lld).",
        (long long) client_id);
    _nns_edge_remove_connection (eh, client_id);
    ret = NNS_EDGE_ERROR_CONNECTION_FAILURE;

    if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type) {
      struct timespec retry_delay = { RECONNECT_TIMEOUT_MS / 1000,
        (RECONNECT_TIMEOUT_MS % 1000) * 1000000
      };

      nns_edge_logi ("Connection lost! Reconnect to available node.");

      while (conn->running && nns_edge_mqtt_is_connected (eh->broker_h)) {
        ret = _mqtt_hybrid_direct_connection (eh, RECONNECT_TIMEOUT_MS);
        if (NNS_EDGE_ERROR_NONE == ret)
          break;

        /* The wait for a message may return at once, do not retry faster. */
        nanosleep (&retry_delay, NULL);
      }
    }

    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_event_invoke_callback (eh->event_cb, eh->user_data,
          NNS_EDGE_EVENT_CONNECTION_CLOSED, NULL, 0, NULL);
    }
  }

  conn->running = false;
  return NULL;
}

/**
 * @brief Create message handle thread.
 */
static int
_nns_edge_create_message_thread (nns_edge_handle_s * eh, nns_edge_conn_s * conn,
    int64_t client_id)
{
  int status;
  nns_edge_thread_data_s *thread_data = NULL;

  thread_data =
      (nns_edge_thread_data_s *) calloc (1, sizeof (nns_edge_thread_data_s));
  if (!thread_data) {
    nns_edge_loge ("Failed to allocate edge thread data.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

   /** Create message receiving thread */
  thread_data->eh = eh;
  thread_data->conn = conn;
  thread_data->client_id = client_id;

  conn->running = true;
  /* By the time the thread reaches its own teardown, this id is set. */
  status = pthread_create (&conn->msg_thread, NULL, _nns_edge_message_handler,
      thread_data);

  if (status != 0) {
    nns_edge_loge ("Failed to create message handler thread.");
    conn->running = false;
    conn->msg_thread = 0;
    SAFE_FREE (thread_data);
    return NNS_EDGE_ERROR_IO;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Thread to send data.
 */
static void *
_nns_edge_send_thread (void *thread_data)
{
  nns_edge_handle_s *eh = (nns_edge_handle_s *) thread_data;
  nns_edge_conn_data_s *conn_data;
  nns_edge_conn_ref_s *conn_list;
  nns_edge_conn_s *conn;
  nns_edge_data_h data_h;
  nns_size_t data_size;
  unsigned int i, n;
  int64_t client_id;
  char *val;
  int ret;

  nns_edge_lock (eh);
  eh->sending = true;
  nns_edge_cond_signal (eh);
  nns_edge_unlock (eh);

  while (eh->sending &&
      NNS_EDGE_ERROR_NONE == nns_edge_queue_wait_pop (eh->send_queue, 0U,
          &data_h, &data_size)) {
    if (!eh->sending) {
      nns_edge_data_destroy (data_h);
      break;
    }

    /* Send data to destination */
    switch (eh->connect_type) {
      case NNS_EDGE_CONNECT_TYPE_TCP:
      case NNS_EDGE_CONNECT_TYPE_HYBRID:
        ret = nns_edge_data_get_info (data_h, "client_id", &val);
        if (ret != NNS_EDGE_ERROR_NONE) {
          nns_edge_logd
              ("Cannot find client ID in edge data. Send to all connected nodes.");

          n = _nns_edge_hold_sink_connection (eh, &conn_list);
          for (i = 0; i < n; i++) {
            client_id = conn_list[i].id;
            ret = _nns_edge_transfer_data (conn_list[i].conn, data_h, client_id);

            if (NNS_EDGE_ERROR_NONE != ret) {
              nns_edge_loge ("Failed to transfer data. Close the connection.");
              _nns_edge_remove_connection (eh, client_id);
            }
          }
          _nns_edge_put_sink_connection (eh, conn_list, n);
        } else {
          client_id = (int64_t) strtoll (val, NULL, 10);
          SAFE_FREE (val);

          nns_edge_conn_lock (eh);
          conn_data = _nns_edge_get_connection (eh, client_id);
          conn = conn_data ? conn_data->sink_conn : NULL;
          if (conn)
            conn->in_use = true;
          nns_edge_conn_unlock (eh);

          if (conn) {
            _nns_edge_transfer_data (conn, data_h, client_id);

            nns_edge_conn_lock (eh);
            conn->in_use = false;
            nns_edge_conn_unlock (eh);

            _nns_edge_release_closed_connection (eh);
          } else {
            nns_edge_loge
                ("Cannot find connection, invalid client ID or connection closed.");
          }
        }
        break;
      case NNS_EDGE_CONNECT_TYPE_MQTT:
        ret = nns_edge_mqtt_publish_data (eh->broker_h, data_h);
        if (NNS_EDGE_ERROR_NONE != ret)
          nns_edge_loge ("Failed to send data via MQTT connection.");
        break;
      case NNS_EDGE_CONNECT_TYPE_CUSTOM:
        ret = nns_edge_custom_send_data (eh->custom_connection_h, data_h);
        if (NNS_EDGE_ERROR_NONE != ret)
          nns_edge_loge ("Failed to send data via custom connection.");
        break;
      default:
        break;
    }
    nns_edge_data_destroy (data_h);
  }
  eh->sending = false;

  return NULL;
}

/**
 * @brief Create thread to send data.
 * @note This should be called with lock.
 */
static int
_nns_edge_create_send_thread (nns_edge_handle_s * eh)
{
  int status;

  if (eh->send_thread)
    return NNS_EDGE_ERROR_NONE;

  status = pthread_create (&eh->send_thread, NULL, _nns_edge_send_thread, eh);

  if (status != 0) {
    nns_edge_loge ("Failed to create sender thread.");
    eh->send_thread = 0;
    eh->sending = false;
    return NNS_EDGE_ERROR_IO;
  }

  /* Wait for starting thread. */
  nns_edge_cond_wait (eh);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Connect to the destination node. (host:sender(sink) - dest:receiver(listener, src))
 */
static int
_nns_edge_connect_to (nns_edge_handle_s * eh, int64_t client_id,
    const char *host, int port)
{
  nns_edge_conn_s *conn = NULL;
  nns_edge_conn_s *old_conn = NULL;
  nns_edge_conn_data_s *conn_data;
  nns_edge_cmd_s cmd;
  char *host_str;
  bool done = false;
  int ret;

  conn = (nns_edge_conn_s *) calloc (1, sizeof (nns_edge_conn_s));
  if (!conn) {
    nns_edge_loge ("Failed to allocate client data.");
    goto error;
  }

  conn->host = nns_edge_strdup (host);
  conn->port = port;
  conn->sockfd = -1;
  conn->max_transfer_size = eh->max_transfer_size;
  conn->recv_timeout_ms = eh->recv_timeout_ms;

  if (!_nns_edge_connect_socket (conn)) {
    goto error;
  }

  if ((NNS_EDGE_NODE_TYPE_QUERY_CLIENT == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_SUB == eh->node_type)) {
    /* Receive capability and client ID from server. */
    _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_ERROR, client_id);
    ret = _nns_edge_cmd_receive (conn, &cmd);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to receive capability.");
      goto error;
    }

    if (cmd.info.cmd != _NNS_EDGE_CMD_CAPABILITY) {
      nns_edge_loge ("Failed to get capability.");
      _nns_edge_cmd_clear (&cmd);
      goto error;
    }

    client_id = eh->client_id = cmd.info.client_id;

    /* Check compatibility. */
    ret = nns_edge_event_invoke_callback (eh->event_cb, eh->user_data,
        NNS_EDGE_EVENT_CAPABILITY, cmd.mem[0], cmd.info.mem_size[0], NULL);
    _nns_edge_cmd_clear (&cmd);

    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("The event returns error, capability is not acceptable.");
      _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_ERROR, client_id);
    } else {
      /* Send host and port to destination. */
      _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_HOST_INFO, client_id);

      host_str = nns_edge_get_host_string (eh->host, eh->port);
      if (!host_str) {
        nns_edge_loge ("Failed to allocate the host string.");
        goto error;
      }

      cmd.info.num = 1;
      cmd.info.mem_size[0] = strlen (host_str) + 1;
      cmd.mem[0] = host_str;
    }

    ret = _nns_edge_cmd_send (conn, &cmd);
    _nns_edge_cmd_clear (&cmd);

    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to send host info.");
      goto error;
    }
  }

  nns_edge_conn_lock (eh);
  conn_data = _nns_edge_add_connection (eh, client_id);
  if (conn_data) {
    ret = NNS_EDGE_ERROR_NONE;

    /* Take old connection out and set new one. */
    old_conn = conn_data->sink_conn;
    conn_data->sink_conn = conn;

    if (NNS_EDGE_NODE_TYPE_SUB == eh->node_type) {
      /* The message thread may remove the connection data, set the connection first. */
      ret = _nns_edge_create_message_thread (eh, conn, client_id);
      if (ret != NNS_EDGE_ERROR_NONE)
        conn_data->sink_conn = NULL;
    }

    done = (ret == NNS_EDGE_ERROR_NONE);
  }
  nns_edge_conn_unlock (eh);

  _nns_edge_release_old_connection (eh, old_conn);

error:
  if (!done) {
    _nns_edge_close_connection (eh, conn);
    return NNS_EDGE_ERROR_CONNECTION_FAILURE;
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Accept socket and create message thread in socket listener thread.
 */
static void
_nns_edge_accept_socket (nns_edge_handle_s * eh)
{
  bool done = false;
  bool parsed;
  nns_edge_conn_s *conn;
  nns_edge_conn_s *old_conn = NULL;
  nns_edge_conn_data_s *conn_data = NULL;
  nns_edge_cmd_s cmd;
  int64_t client_id;
  char *caps_str = NULL;
  char *dest_host = NULL;
  int dest_port = 0;
  int ret;

  conn = (nns_edge_conn_s *) calloc (1, sizeof (nns_edge_conn_s));
  if (!conn) {
    nns_edge_loge ("Failed to allocate edge connection.");
    goto error;
  }

  conn->max_transfer_size = eh->max_transfer_size;
  conn->recv_timeout_ms = eh->recv_timeout_ms;

  conn->sockfd = accept (eh->listener_fd, NULL, NULL);
  if (conn->sockfd < 0) {
    nns_edge_loge ("Failed to accept socket.");
    goto error;
  }

  _set_socket_option (conn);

  if ((NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)) {
    client_id = nns_edge_generate_id ();
  } else {
    client_id = eh->client_id;
  }

  /* Send capability and info to check compatibility. */
  if ((NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)) {
    nns_edge_lock (eh);
    caps_str = nns_edge_strdup (eh->caps_str);
    nns_edge_unlock (eh);

    if (!caps_str) {
      nns_edge_loge ("Failed to allocate memory for capability.");
      goto error;
    }

    _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_CAPABILITY, client_id);
    cmd.info.num = 1;
    cmd.info.mem_size[0] = strlen (caps_str) + 1;
    cmd.mem[0] = caps_str;

    ret = _nns_edge_cmd_send (conn, &cmd);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to send capability.");
      goto error;
    }
  }

  if (NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type) {
    /* Receive host info from destination. */
    _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_ERROR, client_id);
    ret = _nns_edge_cmd_receive (conn, &cmd);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to receive node info.");
      goto error;
    }

    if (cmd.info.cmd != _NNS_EDGE_CMD_HOST_INFO) {
      nns_edge_loge ("Failed to get host info.");
      _nns_edge_cmd_clear (&cmd);
      goto error;
    }

    parsed = _nns_edge_parse_peer_host (cmd.mem[0], cmd.info.mem_size[0],
        &dest_host, &dest_port);
    _nns_edge_cmd_clear (&cmd);

    if (!parsed) {
      nns_edge_loge ("Failed to get host info, the client sent an invalid "
          "host string.");
      goto error;
    }

    /* Connect to client listener. */
    ret = _nns_edge_connect_to (eh, client_id, dest_host, dest_port);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to connect host %s:%d.", dest_host, dest_port);
      goto error;
    }
  }

  /* Take old connection out and set new one for each node type. */
  nns_edge_conn_lock (eh);
  conn_data = _nns_edge_add_connection (eh, client_id);
  if (!conn_data) {
    ret = NNS_EDGE_ERROR_OUT_OF_MEMORY;
  } else if (eh->node_type == NNS_EDGE_NODE_TYPE_QUERY_CLIENT ||
      eh->node_type == NNS_EDGE_NODE_TYPE_QUERY_SERVER) {
    old_conn = conn_data->src_conn;
    conn_data->src_conn = conn;

    /* The message thread may remove the connection data, set the connection first. */
    ret = _nns_edge_create_message_thread (eh, conn, client_id);
    if (ret != NNS_EDGE_ERROR_NONE)
      conn_data->src_conn = NULL;
  } else {
    ret = NNS_EDGE_ERROR_NONE;
    old_conn = conn_data->sink_conn;
    conn_data->sink_conn = conn;
  }
  nns_edge_conn_unlock (eh);

  _nns_edge_release_old_connection (eh, old_conn);

  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to set the connection of client (ID: %lld).",
        (long long) client_id);
    goto error;
  }

  ret = nns_edge_event_invoke_callback (eh->event_cb, eh->user_data,
      NNS_EDGE_EVENT_CONNECTION_COMPLETED, NULL, 0, NULL);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to send an event for new connection.");
    goto error;
  }
  done = true;

error:
  if (!done) {
    /** Detach the connection before releasing it, to avoid a double free. */
    nns_edge_conn_lock (eh);
    if (conn_data) {
      if (conn_data->src_conn == conn)
        conn_data->src_conn = NULL;
      if (conn_data->sink_conn == conn)
        conn_data->sink_conn = NULL;
    }
    nns_edge_conn_unlock (eh);

    _nns_edge_release_old_connection (eh, conn);
  }

  /* Release the connections of the nodes that are gone, this thread is not one. */
  _nns_edge_release_closed_connection (eh);

  SAFE_FREE (caps_str);
  SAFE_FREE (dest_host);
}

/**
 * @brief Socket listener thread.
 */
static void *
_nns_edge_socket_listener_thread (void *thread_data)
{
  nns_edge_handle_s *eh = (nns_edge_handle_s *) thread_data;

  nns_edge_lock (eh);
  eh->listening = true;
  nns_edge_cond_signal (eh);
  nns_edge_unlock (eh);

  while (eh->listening) {
    struct pollfd poll_fd;

    poll_fd.fd = eh->listener_fd;
    poll_fd.events = POLLIN | POLLHUP | POLLERR;
    poll_fd.revents = 0;

    /* 10 milliseconds */
    if (poll (&poll_fd, 1, 10) > 0) {
      if (!eh->listening)
        break;

      if (poll_fd.revents & (POLLERR | POLLHUP)) {
        nns_edge_loge ("Invalid state, possibly socket is closed in listener.");
        break;
      }

      if (poll_fd.revents & POLLIN)
        _nns_edge_accept_socket (eh);
    }
  }
  eh->listening = false;

  return NULL;
}

/**
 * @brief Create socket listener.
 * @note This function should be called with handle lock.
 */
static bool
_nns_edge_create_socket_listener (nns_edge_handle_s * eh)
{
  bool done = false;
  struct sockaddr_in saddr = { 0 };
  socklen_t saddr_len = sizeof (struct sockaddr_in);
  int status;

  if (eh->listener_thread)
    return true;

  if (!_fill_socket_addr (&saddr, eh->host, eh->port)) {
    nns_edge_loge ("Failed to create listener, invalid host: %s.", eh->host);
    return false;
  }

  eh->listener_fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (eh->listener_fd < 0) {
    nns_edge_loge ("Failed to create listener socket.");
    return false;
  }

  if (bind (eh->listener_fd, (struct sockaddr *) &saddr, saddr_len) < 0) {
    nns_edge_loge ("Failed to create listener, cannot bind socket.");
    goto error;
  }

  if (listen (eh->listener_fd, N_BACKLOG) < 0) {
    nns_edge_loge ("Failed to create listener, cannot listen socket.");
    goto error;
  }

  status = pthread_create (&eh->listener_thread, NULL,
      _nns_edge_socket_listener_thread, eh);

  if (status != 0) {
    nns_edge_loge ("Failed to create listener thread.");
    eh->listening = false;
    eh->listener_thread = 0;
    goto error;
  }

  /* Wait for the listener thread to be started */
  nns_edge_cond_wait (eh);

  done = true;

error:
  if (!done) {
    close (eh->listener_fd);
    eh->listener_fd = -1;
  }

  return done;
}

/**
 * @brief Internal function to create edge handle.
 */
static int
_nns_edge_create_handle (const char *id, nns_edge_node_type_e node_type,
    nns_edge_h * edge_h)
{
  int ret = NNS_EDGE_ERROR_NONE;
  nns_edge_handle_s *eh;

  eh = (nns_edge_handle_s *) calloc (1, sizeof (nns_edge_handle_s));
  if (!eh) {
    nns_edge_loge ("Failed to allocate memory for edge handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  nns_edge_lock_init (eh);
  nns_edge_conn_lock_init (eh);
  nns_edge_cond_init (eh);
  pthread_mutex_init (&eh->closed_lock, NULL);
  nns_edge_handle_set_magic (eh, NNS_EDGE_MAGIC);
  eh->id = STR_IS_VALID (id) ? nns_edge_strdup (id) :
      nns_edge_strdup_printf ("%lld", (long long) nns_edge_generate_id ());
  eh->host = nns_edge_strdup ("localhost");
  eh->port = 0;
  eh->dest_host = nns_edge_strdup ("localhost");
  eh->dest_port = 0;
  eh->node_type = node_type;
  eh->is_started = false;
  eh->broker_h = NULL;
  eh->connections = NULL;
  eh->listening = false;
  eh->sending = false;
  eh->listener_fd = -1;
  eh->caps_str = nns_edge_strdup ("");
  eh->custom_connection_h = NULL;
  eh->max_transfer_size = NNS_EDGE_MAX_TRANSFER_SIZE;
  eh->recv_timeout_ms = NNS_EDGE_RECV_TIMEOUT_MS;

  ret = nns_edge_metadata_create (&eh->metadata);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create edge metadata.");
    goto error;
  }

  ret = nns_edge_queue_create (&eh->send_queue);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to create edge queue.");
    goto error;
  }

error:
  if (ret == NNS_EDGE_ERROR_NONE)
    *edge_h = eh;
  else
    nns_edge_release_handle (eh);

  return ret;
}

/**
 * @brief Create edge custom handle.
 */
int
nns_edge_custom_create_handle (const char *id, const char *lib_path,
    nns_edge_node_type_e node_type, nns_edge_h * edge_h)
{
  int ret = NNS_EDGE_ERROR_NONE;
  nns_edge_handle_s *eh;

  if (node_type < 0 || node_type >= NNS_EDGE_NODE_TYPE_UNKNOWN) {
    nns_edge_loge ("Invalid param, set exact node type.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (lib_path)) {
    nns_edge_loge ("Invalid param, given custom lib path is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!edge_h) {
    nns_edge_loge ("Invalid param, edge_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = _nns_edge_create_handle (id, node_type, edge_h);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create edge handle.");
    return ret;
  }

  eh = (nns_edge_handle_s *) (*edge_h);
  eh->connect_type = NNS_EDGE_CONNECT_TYPE_CUSTOM;

  ret = nns_edge_custom_load (lib_path, &eh->custom_connection_h);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_release_handle (eh);
    *edge_h = NULL;
  }

  return ret;
}

/**
 * @brief Create edge handle.
 */
int
nns_edge_create_handle (const char *id, nns_edge_connect_type_e connect_type,
    nns_edge_node_type_e node_type, nns_edge_h * edge_h)
{
  int ret = NNS_EDGE_ERROR_NONE;
  nns_edge_handle_s *eh;

  if (connect_type < 0 || connect_type >= NNS_EDGE_CONNECT_TYPE_UNKNOWN ||
      connect_type == NNS_EDGE_CONNECT_TYPE_CUSTOM) {
    nns_edge_loge ("Invalid param, set valid connect type.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /**
   * @todo handle flag (receive | send)
   * e.g., send only case: listener is unnecessary.
   */
  if (node_type < 0 || node_type >= NNS_EDGE_NODE_TYPE_UNKNOWN) {
    nns_edge_loge ("Invalid param, set exact node type.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!edge_h) {
    nns_edge_loge ("Invalid param, edge_h should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = _nns_edge_create_handle (id, node_type, edge_h);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create edge handle.");
    return ret;
  }

  eh = (nns_edge_handle_s *) (*edge_h);
  eh->connect_type = connect_type;

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Internal function to connect to MQTT broker, releasing the previous broker handle if it exists.
 * @note This function should be called with handle lock.
 */
static int
_nns_edge_connect_to_broker (nns_edge_handle_s * eh, const char *topic)
{
  if (eh->broker_h) {
    if (NNS_EDGE_ERROR_NONE != nns_edge_mqtt_close (eh->broker_h))
      nns_edge_logw ("Failed to close the previous mqtt connection.");
    eh->broker_h = NULL;
  }

  return nns_edge_mqtt_connect (eh->id, topic, eh->dest_host, eh->dest_port,
      &eh->broker_h);
}

/**
 * @brief Start the nnstreamer edge.
 */
int
nns_edge_start (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (eh->is_started) {
    nns_edge_logi ("Edge is already started. Nothing to do.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_NONE;
  }

  if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    ret = nns_edge_custom_start (eh->custom_connection_h);
    if (NNS_EDGE_ERROR_NONE == ret)
      ret = _nns_edge_create_send_thread (eh);

    if (NNS_EDGE_ERROR_NONE != ret)
      nns_edge_loge ("Failed to start edge custom connection.");
    goto done;
  }

  if (eh->port <= 0) {
    eh->port = nns_edge_get_available_port ();
    if (eh->port <= 0) {
      nns_edge_loge ("Failed to start edge. Cannot get available port.");
      nns_edge_unlock (eh);
      return NNS_EDGE_ERROR_CONNECTION_FAILURE;
    }
  }

  if ((NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)) {
    if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type
        || NNS_EDGE_CONNECT_TYPE_MQTT == eh->connect_type) {
      char *topic;

      /** @todo Set unique device name.
       * Device name should be unique. Consider using MAC address later.
       * Now, use ID received from the user.
       */
      topic = nns_edge_strdup_printf ("edge/inference/device-%s/%s/",
          eh->id, eh->topic);

      ret = _nns_edge_connect_to_broker (eh, topic);
      SAFE_FREE (topic);

      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge
            ("Failed to start nnstreamer-edge, cannot connect to broker.");
        goto done;
      }

      if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type) {
        char *msg;
        msg = nns_edge_get_host_string (eh->host, eh->port);
        if (!msg) {
          nns_edge_loge ("Failed to allocate the host string.");
          ret = NNS_EDGE_ERROR_OUT_OF_MEMORY;
          goto done;
        }

        ret = nns_edge_mqtt_publish (eh->broker_h, msg, strlen (msg) + 1);
        SAFE_FREE (msg);

        if (NNS_EDGE_ERROR_NONE != ret) {
          nns_edge_loge ("Failed to publish the message to broker.");
          goto done;
        }
      } else {
        ret = nns_edge_mqtt_set_event_callback (eh->broker_h, eh->event_cb,
            eh->user_data);
        if (NNS_EDGE_ERROR_NONE != ret) {
          nns_edge_loge ("Failed to set event callback to MQTT broker.");
          goto done;
        }
      }
    }
  }

  if ((NNS_EDGE_NODE_TYPE_QUERY_CLIENT == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)) {
    /* Start listener thread to accept socket. */
    if (!_nns_edge_create_socket_listener (eh)) {
      nns_edge_loge ("Failed to create socket listener.");
      ret = NNS_EDGE_ERROR_IO;
      goto done;
    }

    ret = _nns_edge_create_send_thread (eh);
  }

done:
  eh->is_started = (ret == NNS_EDGE_ERROR_NONE);
  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Stop the nnstreamer edge.
 */
int
nns_edge_stop (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);
  if (!eh->is_started) {
    nns_edge_logi ("Edge is not started yet. Nothing to stop.");
    goto done;
  }

  if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    ret = nns_edge_custom_stop (eh->custom_connection_h);
  }

  if (NNS_EDGE_ERROR_NONE == ret)
    eh->is_started = FALSE;

done:
  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Release the given handle.
 */
int
nns_edge_release_handle (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_stop (eh);

  nns_edge_lock (eh);

  /* Clear message queue and stop thread first */
  nns_edge_queue_clear (eh->send_queue);

  eh->sending = false;
  nns_edge_queue_stop_wait (eh->send_queue);
  if (eh->send_thread) {
    pthread_join (eh->send_thread, NULL);
    eh->send_thread = 0;
  }

  eh->listening = false;
  nns_edge_unlock (eh);

  /* The listener thread takes the handle lock, do not join it while holding it. */
  if (eh->listener_thread) {
    pthread_join (eh->listener_thread, NULL);
    eh->listener_thread = 0;
  }

  nns_edge_lock (eh);

  if (eh->listener_fd >= 0) {
    close (eh->listener_fd);
    eh->listener_fd = -1;
  }

  /* A message thread being joined may connect again, drain until it cannot. */
  do {
    _nns_edge_remove_all_connection (eh);
  } while (_nns_edge_has_connection (eh));

  pthread_mutex_lock (&eh->closed_lock);
  if (eh->closed_connections) {
    nns_edge_loge
        ("Cannot release the connection of the calling thread, the handle is freed while its message thread runs.");
  }
  pthread_mutex_unlock (&eh->closed_lock);

  switch (eh->connect_type) {
    case NNS_EDGE_CONNECT_TYPE_HYBRID:
    case NNS_EDGE_CONNECT_TYPE_MQTT:
      if (NNS_EDGE_ERROR_NONE != nns_edge_mqtt_close (eh->broker_h)) {
        nns_edge_logw ("Failed to close mqtt connection.");
      }
      break;
    case NNS_EDGE_CONNECT_TYPE_CUSTOM:
      if (eh->custom_connection_h
          && nns_edge_custom_release (eh->custom_connection_h) !=
          NNS_EDGE_ERROR_NONE) {
        nns_edge_logw ("Failed to close custom connection.");
      }
      break;
    default:
      break;
  }

  /* Clear event callback and handles */
  nns_edge_handle_set_magic (eh, NNS_EDGE_MAGIC_DEAD);
  eh->event_cb = NULL;
  eh->user_data = NULL;
  eh->broker_h = NULL;
  eh->custom_connection_h = NULL;

  nns_edge_queue_destroy (eh->send_queue);
  eh->send_queue = NULL;
  nns_edge_metadata_destroy (eh->metadata);
  eh->metadata = NULL;
  SAFE_FREE (eh->id);
  SAFE_FREE (eh->topic);
  SAFE_FREE (eh->host);
  SAFE_FREE (eh->dest_host);
  SAFE_FREE (eh->caps_str);

  nns_edge_unlock (eh);
  nns_edge_cond_destroy (eh);
  nns_edge_lock_destroy (eh);
  pthread_mutex_destroy (&eh->closed_lock);
  nns_edge_conn_lock_destroy (eh);
  SAFE_FREE (eh);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Set the event callback.
 */
int
nns_edge_set_event_callback (nns_edge_h edge_h, nns_edge_event_cb cb,
    void *user_data)
{
  nns_edge_handle_s *eh;
  int ret;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  ret = nns_edge_event_invoke_callback (eh->event_cb, eh->user_data,
      NNS_EDGE_EVENT_CALLBACK_RELEASED, NULL, 0, NULL);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to set new event callback.");
    goto error;
  }

  if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    ret = nns_edge_custom_set_event_callback (eh->custom_connection_h,
        cb, user_data);
    if (NNS_EDGE_ERROR_NONE != ret) {
      goto error;
    }
  }

  eh->event_cb = cb;
  eh->user_data = user_data;

error:
  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Parse the message received from the MQTT broker and connect to the server directly.
 * @note Set timeout (in milliseconds) to wait for the message, 0 for infinite timeout.
 */
static int
_mqtt_hybrid_direct_connection (nns_edge_handle_s * eh, unsigned int timeout)
{
  int ret;

  do {
    char *msg = NULL;
    char *server_ip = NULL;
    int server_port = 0;
    bool parsed;
    nns_size_t msg_len = 0;

    ret =
        nns_edge_mqtt_get_message (eh->broker_h, (void **) &msg, &msg_len,
        timeout);
    if (ret != NNS_EDGE_ERROR_NONE || !msg || msg_len == 0)
      break;

    parsed = _nns_edge_parse_peer_host (msg, msg_len, &server_ip, &server_port);
    SAFE_FREE (msg);

    if (!parsed) {
      nns_edge_loge ("Failed to parse the server info from the broker.");
      continue;
    }

    nns_edge_logd ("Parsed server info: Server [%s:%d] ", server_ip,
        server_port);

    ret = _nns_edge_connect_to (eh, eh->client_id, server_ip, server_port);
    SAFE_FREE (server_ip);

    if (NNS_EDGE_ERROR_NONE == ret)
      break;
  } while (TRUE);

  return ret;
}

/**
 * @brief Start subscription to MQTT message
 */
static int
_nns_edge_start_mqtt_sub (nns_edge_handle_s * eh)
{
  char *topic;
  int ret;

  if (!nns_edge_mqtt_is_connected (eh->broker_h)) {
    topic = nns_edge_strdup_printf ("edge/inference/+/%s/#", eh->topic);

    ret = _nns_edge_connect_to_broker (eh, topic);
    SAFE_FREE (topic);

    if (NNS_EDGE_ERROR_NONE != ret) {
      return NNS_EDGE_ERROR_CONNECTION_FAILURE;
    }

    ret = nns_edge_mqtt_subscribe (eh->broker_h);
    if (NNS_EDGE_ERROR_NONE != ret) {
      nns_edge_loge ("Failed to subscribe to topic: %s.", eh->topic);
      return ret;
    }
  }

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Connect to the destination node.
 */
int
nns_edge_connect (nns_edge_h edge_h, const char *dest_host, int dest_port)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (dest_host)) {
    nns_edge_loge ("Invalid param, given host is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!PORT_IS_VALID (dest_port)) {
    nns_edge_loge ("Invalid port number %d.", dest_port);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  /* Release the connections of the nodes that are gone, this thread is not one. */
  _nns_edge_release_closed_connection (eh);

  if (!eh->is_started) {
    nns_edge_loge ("Invalid state, the edge handle is not started.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_IO;
  }

  if (!eh->event_cb) {
    nns_edge_loge ("NNStreamer-edge event callback is not registered.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_CONNECTION_FAILURE;
  }

  if (NNS_EDGE_ERROR_NONE == nns_edge_is_connected (eh)) {
    nns_edge_logi ("NNStreamer-edge is already connected.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_NONE;
  }

  SAFE_FREE (eh->dest_host);
  eh->dest_host = nns_edge_strdup (dest_host);
  eh->dest_port = dest_port;

  if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type
      || NNS_EDGE_CONNECT_TYPE_MQTT == eh->connect_type) {
    if (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)
      goto done;
    ret = _nns_edge_start_mqtt_sub (eh);
    if (NNS_EDGE_ERROR_NONE != ret)
      goto done;

    if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type) {
      ret = _mqtt_hybrid_direct_connection (eh, 0U);
    } else {
      ret = nns_edge_mqtt_set_event_callback (eh->broker_h, eh->event_cb,
          eh->user_data);
      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Failed to set event callback to MQTT broker.");
        goto done;
      }
    }
  } else if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    ret = nns_edge_custom_connect (eh->custom_connection_h);
    if (ret != NNS_EDGE_ERROR_NONE) {
      goto done;
    }
  } else {
    if (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)
      goto done;
    ret = _nns_edge_connect_to (eh, eh->client_id, dest_host, dest_port);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to connect to %s:%d", dest_host, dest_port);
    }
  }

done:
  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Disconnect from the destination node.
 */
int
nns_edge_disconnect (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);
  if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    ret = nns_edge_custom_disconnect (eh->custom_connection_h);
  } else {
    _nns_edge_remove_all_connection (eh);
  }
  nns_edge_unlock (eh);

  return ret;
}

/**
 * @brief Check whether edge is connected or not.
 */
int
nns_edge_is_connected (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh = (nns_edge_handle_s *) edge_h;
  nns_edge_conn_data_s *conn_data;
  nns_edge_conn_s *conn;
  int ret;

  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (NNS_EDGE_CONNECT_TYPE_MQTT == eh->connect_type &&
      nns_edge_mqtt_is_connected (eh->broker_h))
    return NNS_EDGE_ERROR_NONE;

  if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    return nns_edge_custom_is_connected (eh->custom_connection_h);
  }

  ret = NNS_EDGE_ERROR_CONNECTION_FAILURE;

  nns_edge_conn_lock (eh);
  conn_data = (nns_edge_conn_data_s *) eh->connections;
  while (conn_data) {
    conn = conn_data->sink_conn;
    if (_nns_edge_check_connection (conn)) {
      ret = NNS_EDGE_ERROR_NONE;
      break;
    }
    conn_data = conn_data->next;
  }
  nns_edge_conn_unlock (eh);

  return ret;
}

/**
 * @brief Send data to destination (broker or connected node), asynchronously.
 */
int
nns_edge_send (nns_edge_h edge_h, nns_edge_data_h data_h)
{
  int ret = NNS_EDGE_ERROR_NONE;
  nns_edge_handle_s *eh;
  nns_edge_data_h new_data_h;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (nns_edge_data_is_valid (data_h) != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (NNS_EDGE_ERROR_NONE != nns_edge_is_connected (eh)) {
    nns_edge_loge ("There is no available connection.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_IO;
  }

  if (!eh->send_thread) {
    nns_edge_loge ("Invalid state, start edge before sending a data.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_IO;
  }

  /* Create new data handle and push it into send-queue. */
  ret = nns_edge_data_copy (data_h, &new_data_h);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to send data, cannot copy data.");
    nns_edge_unlock (eh);
    return ret;
  }

  ret = nns_edge_queue_push (eh->send_queue, new_data_h,
      sizeof (nns_edge_data_h), nns_edge_data_release_handle);
  if (NNS_EDGE_ERROR_NONE != ret) {
    nns_edge_loge ("Failed to send data, cannot push data into queue.");
    nns_edge_data_destroy (new_data_h);
  }

  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Set nnstreamer edge info.
 */
int
nns_edge_set_info (nns_edge_h edge_h, const char *key, const char *value)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (key)) {
    nns_edge_loge ("Invalid param, given key is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (value)) {
    nns_edge_loge ("Invalid param, given value is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (0 == strcasecmp (key, "CAPS") || 0 == strcasecmp (key, "CAPABILITY")) {
    SAFE_FREE (eh->caps_str);
    eh->caps_str = nns_edge_strdup (value);
  } else if (0 == strcasecmp (key, "IP") || 0 == strcasecmp (key, "HOST")) {
    SAFE_FREE (eh->host);
    eh->host = nns_edge_strdup (value);
  } else if (0 == strcasecmp (key, "PORT")) {
    int port = nns_edge_parse_port_number (value);

    if (port < 0) {
      ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    } else {
      eh->port = port;
    }
  } else if (0 == strcasecmp (key, "DEST_IP")
      || 0 == strcasecmp (key, "DEST_HOST")) {
    SAFE_FREE (eh->dest_host);
    eh->dest_host = nns_edge_strdup (value);
  } else if (0 == strcasecmp (key, "DEST_PORT")) {
    int port = nns_edge_parse_port_number (value);

    if (port < 0) {
      ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    } else {
      eh->dest_port = port;
    }
  } else if (0 == strcasecmp (key, "TOPIC")) {
    SAFE_FREE (eh->topic);
    eh->topic = nns_edge_strdup (value);
  } else if (0 == strcasecmp (key, "ID") || 0 == strcasecmp (key, "CLIENT_ID")) {
    /* Not allowed key */
    nns_edge_loge ("Cannot update %s.", key);
    ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
  } else if (0 == strcasecmp (key, "QUEUE_SIZE")) {
    const char *s;
    unsigned int limit;
    nns_edge_queue_leak_e leaky = NNS_EDGE_QUEUE_LEAK_NEW;

    s = strstr (value, ":");
    if (s) {
      char *v = nns_edge_strndup (value, s - value);

      limit = (unsigned int) strtoull (v, NULL, 10);
      SAFE_FREE (v);

      if (strcasecmp (s + 1, "NEW") == 0) {
        leaky = NNS_EDGE_QUEUE_LEAK_NEW;
      } else if (strcasecmp (s + 1, "OLD") == 0) {
        leaky = NNS_EDGE_QUEUE_LEAK_OLD;
      } else {
        nns_edge_loge ("Cannot set queue leaky option (%s).", s + 1);
        ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
      }
    } else {
      limit = (unsigned int) strtoull (value, NULL, 10);
    }

    if (ret == NNS_EDGE_ERROR_NONE)
      nns_edge_queue_set_limit (eh->send_queue, limit, leaky);
  } else if (0 == strcasecmp (key, "RECV_TIMEOUT")) {
    unsigned long long timeout;

    errno = 0;
    timeout = strtoull (value, NULL, 10);

    if (errno != 0 || value[strspn (value, "0123456789")] != '\0'
        || timeout > UINT_MAX) {
      nns_edge_loge ("Cannot set the receive timeout (%s).", value);
      ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    } else {
      eh->recv_timeout_ms = (unsigned int) timeout;
    }
  } else if (0 == strcasecmp (key, "MAX_TRANSFER_SIZE")) {
    unsigned long long size;

    errno = 0;
    size = strtoull (value, NULL, 10);

    if (errno != 0 || value[strspn (value, "0123456789")] != '\0') {
      nns_edge_loge ("Cannot set the max transfer size (%s).", value);
      ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    } else {
      eh->max_transfer_size = (nns_size_t) size;
    }
  } else {
    ret = nns_edge_metadata_set (eh->metadata, key, value);
  }

  if (ret == NNS_EDGE_ERROR_NONE &&
      NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    /* Pass value to custom library and ignore error. */
    if (nns_edge_custom_set_info (eh->custom_connection_h, key, value) !=
        NNS_EDGE_ERROR_NONE) {
      nns_edge_logw ("Failed to set info '%s' in custom connection.", key);
    }
  }

  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Get nnstreamer edge info.
 */
int
nns_edge_get_info (nns_edge_h edge_h, const char *key, char **value)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!STR_IS_VALID (key)) {
    nns_edge_loge ("Invalid param, given key is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!value) {
    nns_edge_loge ("Invalid param, value should not be null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /* Init null */
  *value = NULL;

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (0 == strcasecmp (key, "CAPS") || 0 == strcasecmp (key, "CAPABILITY")) {
    *value = nns_edge_strdup (eh->caps_str);
  } else if (0 == strcasecmp (key, "IP") || 0 == strcasecmp (key, "HOST")) {
    *value = nns_edge_strdup (eh->host);
  } else if (0 == strcasecmp (key, "PORT")) {
    *value = nns_edge_strdup_printf ("%d", eh->port);
  } else if (0 == strcasecmp (key, "TOPIC")) {
    *value = nns_edge_strdup (eh->topic);
  } else if (0 == strcasecmp (key, "ID")) {
    *value = nns_edge_strdup (eh->id);
  } else if (0 == strcasecmp (key, "DEST_IP")
      || 0 == strcasecmp (key, "DEST_HOST")) {
    *value = nns_edge_strdup (eh->dest_host);
  } else if (0 == strcasecmp (key, "DEST_PORT")) {
    *value = nns_edge_strdup_printf ("%d", eh->dest_port);
  } else if (0 == strcasecmp (key, "CLIENT_ID")) {
    if ((NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type)
        || (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)) {
      nns_edge_loge ("Cannot get the client ID, it was started as a server.");
      ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    } else {
      *value = nns_edge_strdup_printf ("%lld", (long long) eh->client_id);
    }
  } else {
    ret = nns_edge_metadata_get (eh->metadata, key, value);
  }

  if (ret == NNS_EDGE_ERROR_NONE &&
      NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    char *val = NULL;

    if (nns_edge_custom_get_info (eh->custom_connection_h, key, &val) ==
        NNS_EDGE_ERROR_NONE) {
      /* Replace value from custom library. */
      SAFE_FREE (*value);
      *value = val;
    }
  }

  nns_edge_unlock (eh);

  if (ret != NNS_EDGE_ERROR_NONE)
    SAFE_FREE (*value);

  return ret;
}

/**
 * @brief Start discovery connectable devices within the network.
 */
int nns_edge_start_discovery (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);
  if (!eh->event_cb) {
    nns_edge_loge ("NNStreamer-edge event callback is not registered.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_CONNECTION_FAILURE;
  }

  if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    ret = nns_edge_custom_start_discovery (eh->custom_connection_h);
  }

  nns_edge_unlock (eh);

  return ret;
}

/**
 * @brief Stop discovery connectable devices within the network.
 */
int nns_edge_stop_discovery (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!nns_edge_handle_is_valid (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (NNS_EDGE_CONNECT_TYPE_CUSTOM == eh->connect_type) {
    ret = nns_edge_custom_stop_discovery (eh->custom_connection_h);
  }

  nns_edge_unlock (eh);

  return ret;
}

