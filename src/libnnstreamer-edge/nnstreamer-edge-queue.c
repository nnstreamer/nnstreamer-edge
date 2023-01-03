/* SPDX-License-Identifier: Apache-2.0 */
/**
 * Copyright (C) 2022 Samsung Electronics Co., Ltd. All Rights Reserved.
 *
 * @file   nnstreamer-edge-queue.c
 * @date   24 August 2022
 * @brief  Thread-safe queue.
 * @see    https://github.com/nnstreamer/nnstreamer-edge
 * @author Jaeyun Jung <jy1210.jung@samsung.com>
 * @bug    No known bugs except for NYI items.
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
  nns_edge_raw_data_s data;
  nns_edge_queue_data_s *next;
};

/**
 * @brief Internal structure for queue.
 */
typedef struct
{
  pthread_mutex_t lock;
  pthread_cond_t cond;

  nns_edge_queue_leak_e leaky;
  unsigned int max_data; /**< Max data in queue (default 0 means unlimited) */
  unsigned int length;
  nns_edge_queue_data_s *head;
  nns_edge_queue_data_s *tail;
} nns_edge_queue_s;

/**
 * @brief Pop data from queue. If the param 'clear' is true, release old data and return null.
 * @note This function should be called with lock.
 */
static bool
_pop_data (nns_edge_queue_s * q, bool clear, void **data, nns_size_t * size)
{
  nns_edge_queue_data_s *qdata;
  bool popped = false;

  qdata = q->head;
  if (qdata) {
    q->head = qdata->next;
    if ((--q->length) == 0U)
      q->head = q->tail = NULL;

    if (clear) {
      if (qdata->data.destroy_cb)
        qdata->data.destroy_cb (qdata->data.data);
    } else {
      if (data)
        *data = qdata->data.data;
      if (size)
        *size = qdata->data.data_len;
      popped = true;
    }

    free (qdata);
  }

  return popped;
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
  q->leaky = NNS_EDGE_QUEUE_LEAK_NEW;

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

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  nns_edge_lock (q);
  nns_edge_cond_signal (q);

  while (q->length > 0U)
    _pop_data (q, true, NULL, NULL);

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
nns_edge_queue_set_limit (nns_edge_queue_h handle, unsigned int limit,
    nns_edge_queue_leak_e leaky)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  nns_edge_lock (q);
  q->max_data = limit;
  if (leaky != NNS_EDGE_QUEUE_LEAK_UNKNOWN)
    q->leaky = leaky;
  nns_edge_unlock (q);

  return true;
}

/**
 * @brief Add new data into queue.
 */
bool
nns_edge_queue_push (nns_edge_queue_h handle, void *data, nns_size_t size,
    nns_edge_data_destroy_cb destroy)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;
  nns_edge_queue_data_s *qdata;
  bool pushed = false;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  if (!data) {
    nns_edge_loge ("[Queue] Invalid param, data is null.");
    return false;
  }

  if (size == 0U) {
    nns_edge_loge ("[Queue] Invalid param, size should be larger than zero.");
    return false;
  }

  nns_edge_lock (q);
  if (q->max_data > 0U && q->length >= q->max_data) {
    /* Clear old data in queue if leaky option is 'old'. */
    if (q->leaky == NNS_EDGE_QUEUE_LEAK_OLD) {
      _pop_data (q, true, NULL, NULL);
    } else {
      nns_edge_logw ("[Queue] Cannot push new data, max data in queue is %u.",
          q->max_data);
      goto done;
    }
  }

  qdata = calloc (1, sizeof (nns_edge_queue_data_s));
  if (!qdata) {
    nns_edge_loge ("[Queue] Failed to allocate new memory for data.");
    goto done;
  }

  qdata->data.data = data;
  qdata->data.data_len = size;
  qdata->data.destroy_cb = destroy;

  if (!q->head)
    q->head = qdata;
  if (q->tail)
    q->tail->next = qdata;
  q->tail = qdata;
  q->length++;
  pushed = true;

done:
  nns_edge_cond_signal (q);
  nns_edge_unlock (q);

  return pushed;
}

/**
 * @brief Remove and return the first data in queue.
 */
bool
nns_edge_queue_pop (nns_edge_queue_h handle, void **data, nns_size_t * size)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;
  bool popped = false;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  if (!data) {
    nns_edge_loge ("[Queue] Invalid param, data is null.");
    return false;
  }

  if (!size) {
    nns_edge_loge ("[Queue] Invalid param, size is null.");
    return false;
  }

  /* init data */
  *data = NULL;
  *size = 0U;

  nns_edge_lock (q);
  popped = _pop_data (q, false, data, size);
  nns_edge_unlock (q);

  return (popped && *data != NULL);
}

/**
 * @brief Remove and return the first data in queue. If queue is empty, wait until new data is added in the queue.
 */
bool
nns_edge_queue_wait_pop (nns_edge_queue_h handle, unsigned int timeout,
    void **data, nns_size_t * size)
{
  nns_edge_queue_s *q = (nns_edge_queue_s *) handle;
  bool popped = false;

  if (!q) {
    nns_edge_loge ("[Queue] Invalid param, queue is null.");
    return false;
  }

  if (!data) {
    nns_edge_loge ("[Queue] Invalid param, data is null.");
    return false;
  }

  if (!size) {
    nns_edge_loge ("[Queue] Invalid param, size is null.");
    return false;
  }

  /* init data */
  *data = NULL;
  *size = 0U;

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

  popped = _pop_data (q, false, data, size);
  nns_edge_unlock (q);

  return (popped && *data != NULL);
}
