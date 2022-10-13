/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-queue.c
 * @date   24 August 2022
 * @brief  Thread-safe queue.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Jaeyun Jung <jy1210.jung@samsung.com>
 */

#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-queue.h"
#include "nnstreamer-edge-util.h"

/**
 * @brief Internal structure for queue data.
 */
typedef struct _nns_edge_queue_data_s nns_edge_queue_data_s;

/**
 * @brief Internal structure for queue data.
 */
struct _nns_edge_queue_data_s
{
  void *data;
  nns_edge_queue_data_destroy_cb destroy;
  nns_edge_queue_data_s *next;
};

/**
 * @brief Internal structure for queue.
 */
typedef struct
{
  pthread_mutex_t lock;
  pthread_cond_t cond;

  unsigned int max_data; /**< Max data in queue (default 0 means unlimited) */
  unsigned int length;
  nns_edge_queue_data_s *head;
  nns_edge_queue_data_s *tail;
} nns_edge_queue_s;

/**
 * @brief Pop data from queue.
 * @note This function should be called with lock.
 */
static void *
_pop_data (nns_edge_queue_s * q)
{
  nns_edge_queue_data_s *qdata;
  void *data = NULL;

  qdata = q->head;
  if (qdata) {
    data = qdata->data;

    q->head = qdata->next;
    if ((--q->length) == 0U)
      q->head = q->tail = NULL;

    free (qdata);
  }

  return data;
}

/**
 * @brief Create queue.
 */
bool
nns_edge_queue_create (nns_edge_queue_h * handle)
{
  nns_edge_queue_s *q;

  if (!handle) {
    nns_edge_loge ("[Queue] Invalid param, handle is null.");
    return false;
  }

  q = calloc (1, sizeof (nns_edge_queue_s));
  if (!q) {
    nns_edge_loge ("[Queue] Failed to allocate new memory.");
    return false;
  }

  nns_edge_lock_init (q);
  nns_edge_cond_init (q);

  *handle = q;
  return true;
}

/**
 * @brief Destroy queue.
 */
bool
nns_edge_queue_destroy (nns_edge_queue_h handle)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;
  nns_edge_queue_data_s *qdata;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  nns_edge_lock (q);
  nns_edge_cond_signal (q);
  while ((qdata = q->head) != NULL) {
    if (qdata->destroy)
      qdata->destroy (qdata->data);

    q->head = qdata->next;
    free (qdata);
  }

  q->head = q->tail = NULL;
  q->length = 0U;
  nns_edge_unlock (q);

  nns_edge_cond_destroy (q);
  nns_edge_lock_destroy (q);
  free (q);

  return true;
}

/**
 * @brief Get the length of the queue.
 */
unsigned int
nns_edge_queue_get_length (nns_edge_queue_h handle)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;
  unsigned int len;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return 0;
  }

  nns_edge_lock (q);
  len = q->length;
  nns_edge_unlock (q);

  return len;
}

/**
 * @brief Set the max length of the queue.
 */
bool
nns_edge_queue_set_limit (nns_edge_queue_h handle, unsigned int limit)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  nns_edge_lock (q);
  q->max_data = limit;
  nns_edge_unlock (q);

  return true;
}

/**
 * @brief Add new data into queue.
 */
bool
nns_edge_queue_push (nns_edge_queue_h handle, void *data,
    nns_edge_queue_data_destroy_cb destroy)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;
  nns_edge_queue_data_s *qdata;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  if (!data) {
    nns_edge_loge ("[Queue] Invalid param, data is null.");
    return false;
  }

  qdata = calloc (1, sizeof (nns_edge_queue_data_s));
  if (!qdata) {
    nns_edge_loge ("[Queue] Failed to allocate new memory for data.");
    return false;
  }

  qdata->data = data;
  qdata->destroy = destroy;

  nns_edge_lock (q);
  if (q->max_data > 0U && q->length >= q->max_data) {
    nns_edge_logw ("[Queue] Cannot push new data, max data in queue is %u.",
        q->max_data);
  } else {
    if (!q->head)
      q->head = qdata;
    if (q->tail)
      q->tail->next = qdata;
    q->tail = qdata;
    q->length++;
  }
  nns_edge_cond_signal (q);
  nns_edge_unlock (q);

  return true;
}

/**
 * @brief Remove and return the first data in queue.
 */
bool
nns_edge_queue_pop (nns_edge_queue_h handle, void **data)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  if (!data) {
    nns_edge_loge ("[Queue] Invalid param, data is null.");
    return false;
  }

  nns_edge_lock (q);
  *data = _pop_data (q);
  nns_edge_unlock (q);

  return (*data != NULL);
}

/**
 * @brief Remove and return the first data in queue. If queue is empty, wait until new data is added in the queue.
 */
bool
nns_edge_queue_wait_pop (nns_edge_queue_h handle, unsigned int timeout,
    void **data)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  if (!data) {
    nns_edge_loge ("[Queue] Invalid param, data is null.");
    return false;
  }

  nns_edge_lock (q);
  if (q->length == 0U) {
    if (timeout > 0) {
      struct timespec ts;
      struct timeval now;

      gettimeofday (&now, NULL);

      ts.tv_sec = now.tv_sec + timeout / 1000;
      ts.tv_nsec = now.tv_usec * 1000 + (timeout % 1000) * 1000000;

      nns_edge_cond_timedwait (q, &ts);
    } else {
      nns_edge_cond_wait (q);
    }
  }

  *data = _pop_data (q);
  nns_edge_unlock (q);

  return (*data != NULL);
}
