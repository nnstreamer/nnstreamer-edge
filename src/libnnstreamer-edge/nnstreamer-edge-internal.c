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
#include <netdb.h>
#include <poll.h>

#include "nnstreamer-edge-data.h"
#include "nnstreamer-edge-event.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"
#include "nnstreamer-edge-queue.h"
#include "nnstreamer-edge-aitt.h"
#include "nnstreamer-edge-mqtt.h"

#ifndef PTHREAD_CREATE_JOINABLE
#define PTHREAD_CREATE_JOINABLE 0
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define N_BACKLOG 10
#define DEFAULT_TIMEOUT_SEC 10

/**
 * @brief Data structure for edge handle.
 */
typedef struct
{
  uint32_t magic;
  pthread_mutex_t lock;
  char *id;
  char *topic;
  nns_edge_connect_type_e connect_type;
  char *host; /**< host name or IP address */
  int port; /**< port number (0~65535, default 0 to get available port.) */
  char *dest_host; /**< destination IP address (broker or target device) */
  int dest_port; /**< destination port number (broker or target device) */
  nns_edge_node_type_e node_type;
  nns_edge_metadata_h metadata;

  /* Edge event callback and user data */
  nns_edge_event_cb event_cb;
  void *user_data;

  int64_t client_id;
  char *caps_str;

  /* list of connection data */
  void *connections;

  /* socket listener */
  bool listening;
  int listener_fd;
  pthread_t listener_thread;

  /* thread and queue to send data */
  bool sending;
  nns_edge_queue_h send_queue;
  pthread_t send_thread;

  /* MQTT or AITT handle */
  void *broker_h;
} nns_edge_handle_s;

/**
 * @brief enum for nnstreamer edge query commands.
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
  nns_edge_cmd_e cmd;
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
  pthread_t msg_thread;
  int sockfd;
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
 * @brief Structures for thread data of message handling.
 */
typedef struct
{
  nns_edge_handle_s *eh;
  int64_t client_id;
  nns_edge_conn_s *conn;
} nns_edge_thread_data_s;

/**
 * @brief Set socket option.
 * @todo handle connection type (TCP/UDP).
 */
static void
_set_socket_option (int fd)
{
  int nodelay = 1;

  /* setting TCP_NODELAY to true in order to avoid packet batching as known as Nagle's algorithm */
  if (setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof (int)) < 0)
    nns_edge_logw ("Failed to set TCP delay option.");
}

/**
 * @brief Fill socket address struct from host name and port number.
 */
static bool
_fill_socket_addr (struct sockaddr_in *saddr, const char *host, const int port)
{
  /**
   * @todo handle protocol
   * 1. support edge connection type (TCP/UDP)
   * 2. ipv4 and ipv6
   */
  saddr->sin_family = AF_INET;
  saddr->sin_port = htons (port);

  if ((saddr->sin_addr.s_addr = inet_addr (host)) == INADDR_NONE) {
    struct hostent *ent = gethostbyname (host);

    if (!ent)
      return false;

    memmove (&saddr->sin_addr, ent->h_addr, ent->h_length);
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

  cmd->info.num = 0U;
  cmd->info.meta_size = 0U;
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

  return true;
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

  _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_TRANSFER_DATA, client_id);

  nns_edge_data_get_count (data_h, &cmd.info.num);
  for (i = 0; i < cmd.info.num; i++)
    nns_edge_data_get (data_h, i, &cmd.mem[i], &cmd.info.mem_size[i]);

  nns_edge_data_serialize_meta (data_h, &cmd.meta, &cmd.info.meta_size);

  ret = _nns_edge_cmd_send (conn, &cmd);
  SAFE_FREE (cmd.meta);

  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to send edge data to destination (%s:%d).",
        conn->host, conn->port);
  }

  return ret;
}

/**
 * @brief Close connection
 */
static bool
_nns_edge_close_connection (nns_edge_conn_s * conn)
{
  if (!conn)
    return false;

  /* Stop and clear the message thread. */
  if (conn->msg_thread) {
    conn->running = false;
    pthread_join (conn->msg_thread, NULL);
    conn->msg_thread = 0;
  }

  if (conn->sockfd >= 0) {
    nns_edge_cmd_s cmd;

    /* Send error before closing the socket. */
    nns_edge_logd ("Send error cmd to close connection.");
    _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_ERROR, 0);
    _nns_edge_cmd_send (conn, &cmd);

    if (close (conn->sockfd) < 0)
      nns_edge_logw ("Failed to close socket.");
    conn->sockfd = -1;
  }

  SAFE_FREE (conn->host);
  SAFE_FREE (conn);
  return true;
}

/**
 * @brief Release connection data and its resources.
 */
static void
_nns_edge_release_connection_data (nns_edge_conn_data_s * cdata)
{
  if (cdata) {
    _nns_edge_close_connection (cdata->src_conn);
    _nns_edge_close_connection (cdata->sink_conn);
    free (cdata);
  }
}

/**
 * @brief Get nnstreamer-edge connection data.
 * @note This function should be called with handle lock.
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
 * @brief Get nnstreamer-edge connection data.
 * @note This function should be called with handle lock.
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
 * @note This function should be called with handle lock.
 */
static void
_nns_edge_remove_connection (nns_edge_handle_s * eh, int64_t client_id)
{
  nns_edge_conn_data_s *cdata, *prev;

  cdata = (nns_edge_conn_data_s *) eh->connections;
  prev = NULL;

  while (cdata) {
    if (cdata->id == client_id) {
      if (prev)
        prev->next = cdata->next;
      else
        eh->connections = cdata->next;

      _nns_edge_release_connection_data (cdata);
      return;
    }
    prev = cdata;
    cdata = cdata->next;
  }
}

/**
 * @brief Remove all connection data.
 * @note This function should be called with handle lock.
 */
static void
_nns_edge_remove_all_connection (nns_edge_handle_s * eh)
{
  nns_edge_conn_data_s *cdata, *next;

  cdata = (nns_edge_conn_data_s *) eh->connections;
  eh->connections = NULL;

  while (cdata) {
    next = cdata->next;

    _nns_edge_release_connection_data (cdata);

    cdata = next;
  }
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

  _set_socket_option (conn->sockfd);

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

  conn->running = true;
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
  conn->running = false;

  /* Received error message from client, remove connection from table. */
  if (remove_connection) {
    nns_edge_loge
        ("Received error from client, remove connection of client (ID: %lld).",
        (long long) client_id);
    _nns_edge_remove_connection (eh, client_id);
    nns_edge_event_invoke_callback (eh->event_cb, eh->user_data,
        NNS_EDGE_EVENT_CONNECTION_CLOSED, NULL, 0, NULL);
  }

  return NULL;
}

/**
 * @brief Create message handle thread.
 */
static int
_nns_edge_create_message_thread (nns_edge_handle_s * eh, nns_edge_conn_s * conn,
    int64_t client_id)
{
  pthread_attr_t attr;
  int status;
  nns_edge_thread_data_s *thread_data = NULL;

  thread_data =
      (nns_edge_thread_data_s *) calloc (1, sizeof (nns_edge_thread_data_s));
  if (!thread_data) {
    nns_edge_loge ("Failed to allocate edge thread data.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

   /** Create message receving thread */
  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

  thread_data->eh = eh;
  thread_data->conn = conn;
  thread_data->client_id = client_id;

  status = pthread_create (&conn->msg_thread, &attr, _nns_edge_message_handler,
      thread_data);
  pthread_attr_destroy (&attr);

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
  nns_edge_conn_s *conn;
  nns_edge_data_h data_h;
  nns_size_t data_size;
  int64_t client_id;
  char *val;
  int ret;

  eh->sending = true;
  while (eh->sending &&
      nns_edge_queue_wait_pop (eh->send_queue, 0U, &data_h, &data_size)) {
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

          conn_data = (nns_edge_conn_data_s *) eh->connections;
          while (conn_data) {
            client_id = conn_data->id;
            conn = conn_data->sink_conn;
            ret = _nns_edge_transfer_data (conn, data_h, client_id);
            conn_data = conn_data->next;

            if (NNS_EDGE_ERROR_NONE != ret) {
              nns_edge_loge ("Failed to transfer data. Close the connection.");
              _nns_edge_remove_connection (eh, client_id);
            }
          }
        } else {
          client_id = (int64_t) strtoll (val, NULL, 10);
          SAFE_FREE (val);

          conn_data = _nns_edge_get_connection (eh, client_id);
          if (conn_data) {
            conn = conn_data->sink_conn;
            _nns_edge_transfer_data (conn, data_h, client_id);
          } else {
            nns_edge_loge
                ("Cannot find connection, invalid client ID or connection closed.");
          }
        }
        break;
      case NNS_EDGE_CONNECT_TYPE_AITT:
        ret = nns_edge_aitt_send_data (eh->broker_h, data_h);
        if (NNS_EDGE_ERROR_NONE != ret)
          nns_edge_loge ("Failed to send data via AITT connection.");
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
 */
static int
_nns_edge_create_send_thread (nns_edge_handle_s * eh)
{
  pthread_attr_t attr;
  int status;

  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);

  status = pthread_create (&eh->send_thread, &attr, _nns_edge_send_thread, eh);
  pthread_attr_destroy (&attr);

  if (status != 0) {
    nns_edge_loge ("Failed to create sender thread.");
    eh->send_thread = 0;
    return NNS_EDGE_ERROR_IO;
  }

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

  if (NNS_EDGE_NODE_TYPE_SUB == eh->node_type) {
    ret = _nns_edge_create_message_thread (eh, conn, client_id);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to create message handle thread.");
      goto error;
    }
  }

  conn_data = _nns_edge_add_connection (eh, client_id);
  if (conn_data) {
    /* Close old connection and set new one. */
    _nns_edge_close_connection (conn_data->sink_conn);
    conn_data->sink_conn = conn;
    done = true;
  }

error:
  if (!done) {
    _nns_edge_close_connection (conn);
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
  nns_edge_conn_s *conn;
  nns_edge_conn_data_s *conn_data;
  nns_edge_cmd_s cmd;
  int64_t client_id;
  char *dest_host = NULL;
  int dest_port, ret;

  conn = (nns_edge_conn_s *) calloc (1, sizeof (nns_edge_conn_s));
  if (!conn) {
    nns_edge_loge ("Failed to allocate edge connection.");
    goto error;
  }

  conn->sockfd = accept (eh->listener_fd, NULL, NULL);
  if (conn->sockfd < 0) {
    nns_edge_loge ("Failed to accept socket.");
    goto error;
  }

  _set_socket_option (conn->sockfd);

  if ((NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)) {
    client_id = nns_edge_generate_id ();
  } else {
    client_id = eh->client_id;
  }

  /* Send capability and info to check compatibility. */
  if ((NNS_EDGE_NODE_TYPE_QUERY_SERVER == eh->node_type)
      || (NNS_EDGE_NODE_TYPE_PUB == eh->node_type)) {
    if (!STR_IS_VALID (eh->caps_str)) {
      nns_edge_loge ("Cannot accept socket, invalid server capability.");
      goto error;
    }

    _nns_edge_cmd_init (&cmd, _NNS_EDGE_CMD_CAPABILITY, client_id);
    cmd.info.num = 1;
    cmd.info.mem_size[0] = strlen (eh->caps_str) + 1;
    cmd.mem[0] = eh->caps_str;

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

    nns_edge_parse_host_string (cmd.mem[0], &dest_host, &dest_port);
    _nns_edge_cmd_clear (&cmd);

    /* Connect to client listener. */
    ret = _nns_edge_connect_to (eh, client_id, dest_host, dest_port);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to connect host %s:%d.", dest_host, dest_port);
      goto error;
    }
  }

  conn_data = _nns_edge_add_connection (eh, client_id);
  if (!conn_data) {
    nns_edge_loge ("Failed to add client connection.");
    goto error;
  }

  /* Close old connection and set new one for each node type. */
  if (eh->node_type == NNS_EDGE_NODE_TYPE_QUERY_CLIENT ||
      eh->node_type == NNS_EDGE_NODE_TYPE_QUERY_SERVER) {
    ret = _nns_edge_create_message_thread (eh, conn, client_id);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to create message handle thread.");
      goto error;
    }
    _nns_edge_close_connection (conn_data->src_conn);
    conn_data->src_conn = conn;
  } else {
    _nns_edge_close_connection (conn_data->sink_conn);
    conn_data->sink_conn = conn;
  }

  done = true;

error:
  if (!done)
    _nns_edge_close_connection (conn);

  SAFE_FREE (dest_host);
}

/**
 * @brief Socket listener thread.
 */
static void *
_nns_edge_socket_listener_thread (void *thread_data)
{
  nns_edge_handle_s *eh = (nns_edge_handle_s *) thread_data;

  eh->listening = true;
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
  pthread_attr_t attr;
  int status;

  if (!_fill_socket_addr (&saddr, eh->host, eh->port)) {
    nns_edge_loge ("Failed to create listener, invalid host: %s.", eh->host);
    return false;
  }

  eh->listener_fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (eh->listener_fd < 0) {
    nns_edge_loge ("Failed to create listener socket.");
    return false;
  }

  if (bind (eh->listener_fd, (struct sockaddr *) &saddr, saddr_len) < 0 ||
      listen (eh->listener_fd, N_BACKLOG) < 0) {
    nns_edge_loge ("Failed to create listener, cannot bind socket.");
    goto error;
  }

  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);
  status = pthread_create (&eh->listener_thread, &attr,
      _nns_edge_socket_listener_thread, eh);
  pthread_attr_destroy (&attr);

  if (status != 0) {
    nns_edge_loge ("Failed to create listener thread.");
    eh->listening = false;
    eh->listener_thread = 0;
    goto error;
  }

  done = true;

error:
  if (!done) {
    close (eh->listener_fd);
    eh->listener_fd = -1;
  }

  return done;
}

/**
 * @brief Create edge handle.
 */
int
nns_edge_create_handle (const char *id, nns_edge_connect_type_e connect_type,
    nns_edge_node_type_e node_type, nns_edge_h * edge_h)
{
  nns_edge_handle_s *eh;

  if (connect_type < 0 || connect_type >= NNS_EDGE_CONNECT_TYPE_UNKNOWN) {
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

  eh = (nns_edge_handle_s *) calloc (1, sizeof (nns_edge_handle_s));
  if (!eh) {
    nns_edge_loge ("Failed to allocate memory for edge handle.");
    return NNS_EDGE_ERROR_OUT_OF_MEMORY;
  }

  nns_edge_lock_init (eh);
  nns_edge_handle_set_magic (eh, NNS_EDGE_MAGIC);
  eh->id = STR_IS_VALID (id) ? nns_edge_strdup (id) :
      nns_edge_strdup_printf ("%lld", (long long) nns_edge_generate_id ());
  eh->connect_type = connect_type;
  eh->host = nns_edge_strdup ("localhost");
  eh->port = 0;
  eh->dest_host = nns_edge_strdup ("localhost");
  eh->dest_port = 0;
  eh->node_type = node_type;
  eh->broker_h = NULL;
  eh->connections = NULL;
  eh->listening = false;
  eh->sending = false;
  eh->listener_fd = -1;
  nns_edge_metadata_create (&eh->metadata);
  nns_edge_queue_create (&eh->send_queue);

  *edge_h = eh;
  return NNS_EDGE_ERROR_NONE;
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
    if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type) {
      char *topic, *msg;

      /** @todo Set unique device name.
       * Device name should be unique. Consider using MAC address later.
       * Now, use ID received from the user.
       */
      topic = nns_edge_strdup_printf ("edge/inference/device-%s/%s/",
          eh->id, eh->topic);

      ret = nns_edge_mqtt_connect (eh->id, topic, eh->dest_host, eh->dest_port,
          &eh->broker_h);
      SAFE_FREE (topic);

      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge
            ("Failed to start nnstreamer-edge, cannot connect to broker.");
        goto done;
      }

      msg = nns_edge_get_host_string (eh->host, eh->port);

      ret = nns_edge_mqtt_publish (eh->broker_h, msg, strlen (msg) + 1);
      SAFE_FREE (msg);

      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Failed to publish the meesage to broker.");
        goto done;
      }
    } else if (NNS_EDGE_CONNECT_TYPE_AITT == eh->connect_type) {
      ret = nns_edge_aitt_connect (eh->id, eh->topic, eh->dest_host,
          eh->dest_port, &eh->broker_h);
      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Failed to connect to AITT broker.");
        goto done;
      }

      ret = nns_edge_aitt_set_event_callback (eh->broker_h, eh->event_cb,
          eh->user_data);
      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Failed to set event callback to AITT broker.");
        goto done;
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

  nns_edge_lock (eh);

  switch (eh->connect_type) {
    case NNS_EDGE_CONNECT_TYPE_HYBRID:
      if (NNS_EDGE_ERROR_NONE != nns_edge_mqtt_close (eh->broker_h)) {
        nns_edge_logw ("Failed to close mqtt connection.");
      }
      break;
    case NNS_EDGE_CONNECT_TYPE_AITT:
      if (NNS_EDGE_ERROR_NONE != nns_edge_aitt_close (eh->broker_h)) {
        nns_edge_logw ("Failed to close AITT connection.");
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

  nns_edge_queue_destroy (eh->send_queue);
  eh->send_queue = NULL;

  if (eh->send_thread) {
    eh->sending = false;
    pthread_join (eh->send_thread, NULL);
    eh->send_thread = 0;
  }

  if (eh->listener_thread) {
    eh->listening = false;
    pthread_join (eh->listener_thread, NULL);
    eh->listener_thread = 0;
  }

  if (eh->listener_fd >= 0) {
    close (eh->listener_fd);
    eh->listener_fd = -1;
  }

  _nns_edge_remove_all_connection (eh);

  nns_edge_metadata_destroy (eh->metadata);
  eh->metadata = NULL;
  SAFE_FREE (eh->id);
  SAFE_FREE (eh->topic);
  SAFE_FREE (eh->host);
  SAFE_FREE (eh->dest_host);
  SAFE_FREE (eh->caps_str);

  nns_edge_unlock (eh);
  nns_edge_lock_destroy (eh);
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
    nns_edge_unlock (eh);
    return ret;
  }

  eh->event_cb = cb;
  eh->user_data = user_data;

  nns_edge_unlock (eh);
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Connect to the destination node.
 */
int
nns_edge_connect (nns_edge_h edge_h, const char *dest_host, int dest_port)
{
  nns_edge_handle_s *eh;
  int ret;

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

  if (!eh->event_cb) {
    nns_edge_loge ("NNStreamer-edge event callback is not registered.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_CONNECTION_FAILURE;
  }

  SAFE_FREE (eh->dest_host);
  eh->dest_host = nns_edge_strdup (dest_host);
  eh->dest_port = dest_port;

  if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type) {
    char *topic;

    if (!nns_edge_mqtt_is_connected (eh->broker_h)) {
      topic = nns_edge_strdup_printf ("edge/inference/+/%s/#", eh->topic);

      ret = nns_edge_mqtt_connect (eh->id, topic, dest_host, dest_port,
          &eh->broker_h);
      SAFE_FREE (topic);

      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Connection failure to broker.");
        goto done;
      }

      ret = nns_edge_mqtt_subscribe (eh->broker_h);
      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Failed to subscribe to topic: %s.", eh->topic);
        goto done;
      }
    }

    do {
      char *msg = NULL;
      char *server_ip = NULL;
      int server_port = 0;
      nns_size_t msg_len = 0;

      ret = nns_edge_mqtt_get_message (eh->broker_h, (void **) &msg, &msg_len);
      if (ret != NNS_EDGE_ERROR_NONE || !msg || msg_len == 0)
        break;

      nns_edge_parse_host_string (msg, &server_ip, &server_port);
      SAFE_FREE (msg);

      nns_edge_logd ("[DEBUG] Parsed server info: Server [%s:%d] ", server_ip,
          server_port);

      ret = _nns_edge_connect_to (eh, eh->client_id, server_ip, server_port);
      SAFE_FREE (server_ip);

      if (NNS_EDGE_ERROR_NONE == ret) {
        break;
      }
    } while (TRUE);
  } else if (NNS_EDGE_CONNECT_TYPE_AITT == eh->connect_type) {
    ret = nns_edge_aitt_connect (eh->id, eh->topic, dest_host, dest_port,
        &eh->broker_h);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to connect to AITT broker. %s:%d", dest_host,
          dest_port);
      goto done;
    }

    ret = nns_edge_aitt_set_event_callback (eh->broker_h, eh->event_cb,
        eh->user_data);
    if (NNS_EDGE_ERROR_NONE != ret) {
      nns_edge_loge ("Failed to set event callback to AITT broker.");
      goto done;
    }

    ret = nns_edge_aitt_subscribe (eh->broker_h);
    if (NNS_EDGE_ERROR_NONE != ret) {
      nns_edge_loge ("Failed to subscribe the topic using AITT: %s", eh->topic);
      goto done;
    }
  } else {
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
  _nns_edge_remove_all_connection (eh);
  nns_edge_unlock (eh);

  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Check whether edge is connected or not.
 */
static bool
_nns_edge_is_connected (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh = (nns_edge_handle_s *) edge_h;
  nns_edge_conn_data_s *conn_data;
  nns_edge_conn_s *conn;

  if (NNS_EDGE_CONNECT_TYPE_AITT == eh->connect_type &&
      NNS_EDGE_ERROR_NONE == nns_edge_aitt_is_connected (eh->broker_h))
    return true;

  conn_data = (nns_edge_conn_data_s *) eh->connections;
  while (conn_data) {
    conn = conn_data->sink_conn;
    if (_nns_edge_check_connection (conn)) {
      return true;
    }
    conn_data = conn_data->next;
  }

  return false;
}

/**
 * @brief Send data to desination (broker or connected node), asynchronously.
 */
int
nns_edge_send (nns_edge_h edge_h, nns_edge_data_h data_h)
{
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

  if (!_nns_edge_is_connected (eh)) {
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
  nns_edge_data_copy (data_h, &new_data_h);

  if (!nns_edge_queue_push (eh->send_queue, new_data_h,
          sizeof (nns_edge_data_h), nns_edge_data_release_handle)) {
    nns_edge_loge ("Failed to send data, cannot push data into queue.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_IO;
  }

  nns_edge_unlock (eh);
  return NNS_EDGE_ERROR_NONE;
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
    char *s;
    unsigned int limit;
    nns_edge_queue_leak_e leaky = NNS_EDGE_QUEUE_LEAK_UNKNOWN;

    s = strstr (value, ":");
    if (s) {
      char *v = nns_edge_strndup (value, s - value);

      limit = (unsigned int) strtoull (v, NULL, 10);
      nns_edge_free (v);

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

    nns_edge_queue_set_limit (eh->send_queue, limit, leaky);
  } else {
    ret = nns_edge_metadata_set (eh->metadata, key, value);
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

  nns_edge_unlock (eh);
  return ret;
}
