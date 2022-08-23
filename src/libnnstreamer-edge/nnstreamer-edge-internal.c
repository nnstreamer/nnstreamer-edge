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
#include <sys/poll.h>

#include "nnstreamer-edge-common.h"
#include "nnstreamer-edge-internal.h"

#define N_BACKLOG 10
#define DEFAULT_TIMEOUT_SEC 10

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
  unsigned int magic;
  nns_edge_cmd_e cmd;
  int64_t client_id;

  /* memory info */
  uint32_t num;
  size_t mem_size[NNS_EDGE_DATA_LIMIT];
  size_t meta_size;
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
 * @brief Data structure for edge connection.
 */
typedef struct
{
  char *host;
  int port;
  int8_t running;
  pthread_t msg_thread;
  int sockfd;
} nns_edge_conn_s;

/**
 * @brief Data structure for connection data.
 */
typedef struct
{
  nns_edge_conn_s *src_conn;
  nns_edge_conn_s *sink_conn;
  int64_t id;
} nns_edge_conn_data_s;

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
 * @brief Send data to connected socket.
 */
static bool
_send_raw_data (nns_edge_conn_s * conn, void *data, size_t size)
{
  size_t sent = 0;
  ssize_t rret;

  while (sent < size) {
    rret = send (conn->sockfd, (char *) data + sent, size - sent, 0);

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
_receive_raw_data (nns_edge_conn_s * conn, void *data, size_t size)
{
  size_t received = 0;
  ssize_t rret;

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

  n = poll (&poll_fd, 1, 0);
  if (n <= 0 || poll_fd.revents & (POLLERR | POLLHUP)) {
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
  cmd->info.magic = NNS_EDGE_MAGIC;
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

  cmd->info.magic = NNS_EDGE_MAGIC_DEAD;

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

  if (!NNS_EDGE_MAGIC_IS_VALID (&cmd->info) ||
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
    cmd->mem[n] = malloc (cmd->info.mem_size[n]);
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
    cmd->meta = malloc (cmd->info.meta_size);
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

  return ret;
}

/**
 * @brief Internal function to invoke event callback.
 * @note This function should be called with handle lock.
 */
static int
_nns_edge_invoke_event_cb (nns_edge_handle_s * eh, nns_edge_event_e event,
    void *data, size_t data_len, nns_edge_data_destroy_cb destroy_cb)
{
  nns_edge_event_h event_h;
  int ret;

  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /* If event callback is null, return ok. */
  if (!eh->event_cb) {
    nns_edge_logw ("The event callback is null, do nothing!");
    return NNS_EDGE_ERROR_NONE;
  }

  ret = nns_edge_event_create (event, &event_h);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create new edge event.");
    return ret;
  }

  if (data) {
    ret = nns_edge_event_set_data (event_h, data, data_len, destroy_cb);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to handle edge event due to invalid event data.");
      goto error;
    }
  }

  ret = eh->event_cb (event_h, eh->user_data);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("The event callback returns error.");
  }

error:
  nns_edge_event_destroy (event_h);
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
    if (0 != conn->running) {
      pthread_cancel (conn->msg_thread);
      conn->running = 0;
    }
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
 * @brief Get nnstreamer-edge connection data.
 * @note This function should be called with handle lock.
 */
static nns_edge_conn_data_s *
_nns_edge_get_connection (nns_edge_handle_s * eh, int64_t client_id)
{
  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NULL;
  }

  return g_hash_table_lookup (eh->conn_table, GUINT_TO_POINTER (client_id));
}

/**
 * @brief Get nnstreamer-edge connection data.
 * @note This function should be called with handle lock.
 */
static nns_edge_conn_data_s *
_nns_edge_add_connection (nns_edge_handle_s * eh, int64_t client_id)
{
  nns_edge_conn_data_s *data = NULL;

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    return NULL;
  }

  data = g_hash_table_lookup (eh->conn_table, GUINT_TO_POINTER (client_id));

  if (NULL == data) {
    data = (nns_edge_conn_data_s *) calloc (1, sizeof (nns_edge_conn_data_s));
    if (NULL == data) {
      nns_edge_loge ("Failed to allocate memory for connection data.");
      return NULL;
    }

    data->id = client_id;

    g_hash_table_insert (eh->conn_table, GUINT_TO_POINTER (client_id), data);
  }

  return data;
}

/**
 * @brief Remove nnstreamer-edge connection data. This will be called when removing connection data from hash table.
 */
static void
_nns_edge_remove_connection (gpointer data)
{
  nns_edge_conn_data_s *cdata = (nns_edge_conn_data_s *) data;

  if (cdata) {
    _nns_edge_close_connection (cdata->src_conn);
    _nns_edge_close_connection (cdata->sink_conn);
    cdata->src_conn = cdata->sink_conn = NULL;

    SAFE_FREE (cdata);
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

  /**
   * @todo handle protocol
   * 1. support edge connection type (TCP/UDP)
   * 2. ipv4 and ipv6
   */
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons (conn->port);

  if ((saddr.sin_addr.s_addr = inet_addr (conn->host)) == -1) {
    struct hostent *ent = gethostbyname (conn->host);
    if (!ent) {
      nns_edge_loge ("Failed to connect socket, invalid host %s.", conn->host);
      return false;
    }

    memmove (&saddr.sin_addr, ent->h_addr, ent->h_length);
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

  if (!(eh->flags & NNS_EDGE_FLAG_SERVER)) {
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
    ret = _nns_edge_invoke_event_cb (eh, NNS_EDGE_EVENT_CAPABILITY,
        cmd.mem[0], cmd.info.mem_size[0], NULL);
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
 * @brief Message thread, receive buffer from the client.
 */
static void *
_nns_edge_message_handler (void *thread_data)
{
  nns_edge_thread_data_s *_tdata = (nns_edge_thread_data_s *) thread_data;
  nns_edge_handle_s *eh;
  nns_edge_conn_s *conn;
  nns_edge_cmd_s cmd;
  bool remove_connection = false;
  int64_t client_id;
  char *val;
  int ret;

  if (!_tdata) {
    nns_edge_loge ("Internal error, thread data is null.");
    return NULL;
  }

  eh = (nns_edge_handle_s *) _tdata->eh;
  conn = _tdata->conn;
  client_id = _tdata->client_id;
  SAFE_FREE (_tdata);

  conn->running = 1;
  while (conn->running) {
    nns_edge_data_h data_h;
    unsigned int i;

    /* Validate edge handle */
    if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
      nns_edge_loge ("The edge handle is invalid, it would be expired.");
      break;
    }

    /** Receive data from the client */
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
    val = nns_edge_strdup_printf ("%ld", (long int) client_id);
    nns_edge_data_set_info (data_h, "client_id", val);
    SAFE_FREE (val);

    ret = _nns_edge_invoke_event_cb (eh, NNS_EDGE_EVENT_NEW_DATA_RECEIVED,
        data_h, sizeof (nns_edge_data_h), NULL);
    if (ret != NNS_EDGE_ERROR_NONE) {
      /* Try to get next request if server does not accept data from client. */
      nns_edge_logw ("The server does not accept data from client.");
    }

    nns_edge_data_destroy (data_h);
    _nns_edge_cmd_clear (&cmd);
  }
  conn->running = 0;

  /* Received error message from client, remove connection from table. */
  if (remove_connection) {
    nns_edge_logd
        ("Received error from client, remove connection of client (ID: %ld).",
        (long int) client_id);
    g_hash_table_remove (eh->conn_table, GUINT_TO_POINTER (client_id));
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
  int tid;
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

  tid = pthread_create (&conn->msg_thread, &attr, _nns_edge_message_handler,
      thread_data);
  pthread_attr_destroy (&attr);

  if (tid < 0) {
    nns_edge_loge ("Failed to create message handler thread.");
    conn->running = 0;
    conn->msg_thread = 0;
    SAFE_FREE (thread_data);
    return NNS_EDGE_ERROR_IO;
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

  if (eh->flags & NNS_EDGE_FLAG_SERVER)
    client_id = g_get_monotonic_time ();
  else
    client_id = eh->client_id;

  /* Send capability and info to check compatibility. */
  if (eh->flags & NNS_EDGE_FLAG_SERVER) {
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

    /* Receive host info from destination. */
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

  ret = _nns_edge_create_message_thread (eh, conn, client_id);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Failed to create message handle thread.");
    goto error;
  }

  conn_data = _nns_edge_add_connection (eh, client_id);
  if (conn_data) {
    /* Close old connection and set new one. */
    _nns_edge_close_connection (conn_data->src_conn);
    conn_data->src_conn = conn;
    done = true;
  }

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
  int tid;

  eh->listener_fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (eh->listener_fd < 0) {
    nns_edge_loge ("Failed to create listener socket.");
    return false;
  }

  saddr.sin_family = AF_INET;

  if ((saddr.sin_addr.s_addr = inet_addr (eh->host)) == -1) {
    struct hostent *ent = gethostbyname (eh->host);
    if (!ent) {
      nns_edge_loge ("Failed to create listener, invalid host: %s.", eh->host);
      goto error;
    }

    memmove (&saddr.sin_addr, ent->h_addr, ent->h_length);
  }

  saddr.sin_port = htons (eh->port);

  if (bind (eh->listener_fd, (struct sockaddr *) &saddr, saddr_len) < 0 ||
      listen (eh->listener_fd, N_BACKLOG) < 0) {
    nns_edge_loge ("Failed to create listener, cannot bind socket.");
    goto error;
  }

  pthread_attr_init (&attr);
  pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);
  tid = pthread_create (&eh->listener_thread, &attr,
      _nns_edge_socket_listener_thread, eh);
  pthread_attr_destroy (&attr);

  if (tid < 0) {
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
    int flags, nns_edge_h * edge_h)
{
  nns_edge_handle_s *eh;

  if (!STR_IS_VALID (id)) {
    nns_edge_loge ("Invalid param, given ID is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (connect_type < 0 || connect_type >= NNS_EDGE_CONNECT_TYPE_UNKNOWN) {
    nns_edge_loge ("Invalid param, set valid connect type.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /**
   * @todo handle flag (receive | send)
   * e.g., send only case: listener is unnecessary.
   */
  if (flags <= 0 || !(flags & NNS_EDGE_FLAG_ALL)) {
    nns_edge_loge ("Invalid param, set exact edge flags.");
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
  eh->magic = NNS_EDGE_MAGIC;
  eh->id = nns_edge_strdup (id);
  eh->connect_type = connect_type;
  eh->host = nns_edge_strdup ("localhost");
  eh->port = 0;
  eh->dest_host = nns_edge_strdup ("localhost");
  eh->dest_port = 0;
  eh->flags = flags;
  eh->broker_h = NULL;
  nns_edge_metadata_init (&eh->meta);
  eh->listening = false;
  eh->listener_fd = -1;

  /* Connection data for each client ID. */
  eh->conn_table = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      _nns_edge_remove_connection);

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

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (eh->port <= 0) {
    eh->port = nns_edge_get_available_port ();
    if (eh->port <= 0) {
      nns_edge_loge ("Failed to start edge. Cannot get available port.");
      nns_edge_unlock (eh);
      return NNS_EDGE_ERROR_CONNECTION_FAILURE;
    }
  }

  if (eh->flags & NNS_EDGE_FLAG_SERVER) {
    if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type) {
      char *topic, *msg;

      /** @todo Set unique device name.
       * Device name should be unique. Consider using MAC address later.
       * Now, use ID received from the user.
       */
      topic = nns_edge_strdup_printf ("edge/inference/device-%s/%s/",
          eh->id, eh->topic);

      ret = nns_edge_mqtt_connect (eh, topic);
      SAFE_FREE (topic);

      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge
            ("Failed to start nnstreamer-edge. Connection failure to broker.");
        goto error;
      }

      msg = nns_edge_get_host_string (eh->host, eh->port);

      ret = nns_edge_mqtt_publish (eh, msg, strlen (msg) + 1);
      SAFE_FREE (msg);

      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Failed to publish the meesage to broker.");
        goto error;
      }
    }
  }

  /* Start listener thread to accept socket. */
  if (!_nns_edge_create_socket_listener (eh)) {
    nns_edge_loge ("Failed to create socket listener.");
    ret = NNS_EDGE_ERROR_IO;
    goto error;
  }

error:
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

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (nns_edge_mqtt_is_connected (eh)) {
    if (NNS_EDGE_ERROR_NONE != nns_edge_mqtt_close (eh)) {
      nns_edge_logw ("Failed to close mqtt connection.");
    }
  }

  eh->magic = NNS_EDGE_MAGIC_DEAD;
  eh->event_cb = NULL;
  eh->user_data = NULL;

  if (eh->listener_thread) {
    eh->listening = false;
    pthread_cancel (eh->listener_thread);
    pthread_join (eh->listener_thread, NULL);
    eh->listener_thread = 0;
  }

  if (eh->listener_fd >= 0) {
    close (eh->listener_fd);
    eh->listener_fd = -1;
  }

  g_hash_table_destroy (eh->conn_table);
  eh->conn_table = NULL;

  nns_edge_metadata_free (&eh->meta);
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

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = _nns_edge_invoke_event_cb (eh, NNS_EDGE_EVENT_CALLBACK_RELEASED,
      NULL, 0, NULL);
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

  if (dest_port <= 0 || dest_port > 65535) {
    nns_edge_loge ("Invalid port number %d.", dest_port);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (!eh->event_cb) {
    nns_edge_loge ("NNStreamer-edge event callback is not registered.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_CONNECTION_FAILURE;
  }

  SAFE_FREE (eh->dest_host);
  eh->dest_host = nns_edge_strdup (dest_host);
  eh->dest_port = dest_port;

  if (NNS_EDGE_CONNECT_TYPE_HYBRID == eh->connect_type) {
    char *topic, *msg = NULL;
    char *server_ip = NULL;
    int server_port;

    if (!nns_edge_mqtt_is_connected (eh)) {
      topic = nns_edge_strdup_printf ("edge/inference/+/%s/#", eh->topic);

      ret = nns_edge_mqtt_connect (eh, topic);
      SAFE_FREE (topic);

      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Connection failure to broker.");
        nns_edge_unlock (eh);
        return ret;
      }

      ret = nns_edge_mqtt_subscribe (eh);
      if (NNS_EDGE_ERROR_NONE != ret) {
        nns_edge_loge ("Failed to subscribe to topic: %s.", eh->topic);
        nns_edge_unlock (eh);
        return ret;
      }
    }

    while ((ret = nns_edge_mqtt_get_message (eh, &msg)) == NNS_EDGE_ERROR_NONE) {
      nns_edge_parse_host_string (msg, &server_ip, &server_port);
      SAFE_FREE (msg);

      nns_edge_logd ("[DEBUG] Parsed server info: Server [%s:%d] ", server_ip,
          server_port);

      ret = _nns_edge_connect_to (eh, eh->client_id, server_ip, server_port);
      SAFE_FREE (server_ip);

      if (NNS_EDGE_ERROR_NONE == ret) {
        break;
      }
    }
  } else {
    ret = _nns_edge_connect_to (eh, eh->client_id, dest_host, dest_port);
    if (ret != NNS_EDGE_ERROR_NONE) {
      nns_edge_loge ("Failed to connect to %s:%d", dest_host, dest_port);
    }
  }

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

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  g_hash_table_remove_all (eh->conn_table);

  nns_edge_unlock (eh);
  return NNS_EDGE_ERROR_NONE;
}

/**
 * @brief Publish a message to desination (broker or connected node).
 */
int
nns_edge_publish (nns_edge_h edge_h, nns_edge_data_h data_h)
{
  nns_edge_handle_s *eh;
  nns_edge_conn_data_s *conn_data;
  int64_t client_id;
  char *val;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  if (nns_edge_data_is_valid (data_h) != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Invalid param, given edge data is invalid.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  ret = nns_edge_data_get_info (data_h, "client_id", &val);
  if (ret != NNS_EDGE_ERROR_NONE) {
    nns_edge_loge ("Cannot find client ID in edge data.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  client_id = strtoll (val, NULL, 10);
  SAFE_FREE (val);

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /** @todo update code for each connect type */
  switch (eh->connect_type) {
    case NNS_EDGE_CONNECT_TYPE_TCP:
    case NNS_EDGE_CONNECT_TYPE_HYBRID:
      conn_data = _nns_edge_get_connection (eh, client_id);
      if (!conn_data) {
        nns_edge_loge
            ("Cannot find connection, invalid client ID or connection closed.");
        ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
        break;
      }

      ret = _nns_edge_transfer_data (conn_data->sink_conn, data_h, client_id);
      if (ret != NNS_EDGE_ERROR_NONE)
        nns_edge_loge ("Failed to send edge data.");
      break;
    default:
      break;
  }

  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Subscribe a message from broker.
 */
int
nns_edge_subscribe (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    goto done;
  }

  if (eh->connect_type != NNS_EDGE_CONNECT_TYPE_MQTT) {
    nns_edge_loge ("Invalid connect type, cannot subscribe a message.");
    ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    goto done;
  }

  if (!STR_IS_VALID (eh->topic)) {
    nns_edge_loge ("Invalid topic, cannot subscribe a message.");
    ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    goto done;
  }

  /** @todo update code (subscribe) */

done:
  nns_edge_unlock (eh);
  return ret;
}

/**
 * @brief Unsubscribe a message.
 */
int
nns_edge_unsubscribe (nns_edge_h edge_h)
{
  nns_edge_handle_s *eh;
  int ret = NNS_EDGE_ERROR_NONE;

  eh = (nns_edge_handle_s *) edge_h;
  if (!eh) {
    nns_edge_loge ("Invalid param, given edge handle is null.");
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    goto done;
  }

  if (eh->connect_type != NNS_EDGE_CONNECT_TYPE_MQTT) {
    nns_edge_loge ("Invalid connect type, cannot subscribe a message.");
    ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    goto done;
  }

  if (!STR_IS_VALID (eh->topic)) {
    nns_edge_loge ("Invalid topic, cannot subscribe a message.");
    ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    goto done;
  }

  /** @todo update code (unsubscribe) */

done:
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

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /**
   * @todo User handles (replace or append) the capability of edge handle.
   * @todo Change key-value set as json or hash table.
   */
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
  } else {
    ret = nns_edge_metadata_set (&eh->meta, key, value);
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

  nns_edge_lock (eh);

  if (!NNS_EDGE_MAGIC_IS_VALID (eh)) {
    nns_edge_loge ("Invalid param, given edge handle is invalid.");
    nns_edge_unlock (eh);
    return NNS_EDGE_ERROR_INVALID_PARAMETER;
  }

  /**
   * @todo User handles (replace or append) the capability of edge handle.
   * @todo Change key-value set as json or hash table.
   */
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
    if (eh->flags & NNS_EDGE_FLAG_SERVER) {
      nns_edge_loge ("Cannot get the client ID, it was started as a server.");
      ret = NNS_EDGE_ERROR_INVALID_PARAMETER;
    } else {
      *value = nns_edge_strdup_printf ("%ld", (long int) eh->client_id);
    }
  } else {
    ret = nns_edge_metadata_get (&eh->meta, key, value);
  }

  nns_edge_unlock (eh);
  return ret;
}
