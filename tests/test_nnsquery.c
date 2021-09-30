/**
 * @file	test_nnsquery.c
 * @date	24 Aug 2021
 * @brief Publish and subscribe the specific topic message for MQTT-hybrid
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "nnsquery.h"

static void
raw_msg_received_cb (const char *topic_name,
    msg_data *msg, int msg_len, void *user_data)
{
  char *buf;
  int size = msg_len - sizeof(msg->type);
  buf = (char *)malloc(size + 1);
  memcpy (buf, msg->payload, size);
  buf[size] = '\0';

  printf ("Received Topic: %s (Size: %d)\n", topic_name, msg_len);
  printf (" - type: %d\n", msg->type);
  printf (" - payload: %s\n", buf);
}

static void
tcp_msg_received_cb (const char *topic_name,
    msg_data *msg, int msg_len, void *user_data)
{
  char buf[MAX_TCP_CUSTOM_SIZE] = { 0 };
  int custom_size = msg_len - TCP_BASIC_SIZE;
  if (custom_size > 0 && msg->custom) {
    memcpy (buf, msg->custom, custom_size);
    buf[custom_size] = '\0';
  }

  printf ("Received Topic: %s (Size: %d)\n", topic_name, msg_len);
  printf (" - type: %d\n", msg->type);
  printf (" - server hostname: %s\n", msg->srv_host);
  printf (" - port: %u\n", msg->port);
  printf (" - state: %d\n", msg->state);
  printf (" - custom: %s\n", buf);
}

int
tc_open_close_connection ()
{
  query_h handle;
  int ret = 0;

  ret = query_open_connection (&handle, NULL, NULL, NULL, NULL);
  if (ret != 0) {
      printf ("Error: query_open_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(1);

  ret = query_close_connection (handle);
  if (ret != 0) {
      printf ("Error: query_close_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(1);

  return 0;
}

int
tc_send_raw_message ()
{
  query_h handle;
  int ret = 0;
  char msg[] = "test custom message";

  ret = query_open_connection (&handle, NULL, NULL, NULL, NULL);
  if (ret != 0) {
      printf ("Error: query_open_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  ret = query_publish_raw_data (handle, "nns_query/tcp/info", msg, strlen(msg), true);
  if (ret != 0) {
    printf ("Error: query_publish_raw_data() ret: %d\n", ret);
    return -1;
  }
  sleep(2);

  ret = query_close_connection (handle);
  if (ret != 0) {
      printf ("Error: query_close_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  return 0;
}

int
tc_subscribe_raw_message ()
{
  query_h handle;
  int ret = 0;

  ret = query_open_connection (&handle, NULL, NULL, NULL, NULL);
  if (ret != 0) {
      printf ("Error: query_open_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  ret = query_subscribe_topic (handle, "nns_query/tcp/info", raw_msg_received_cb, NULL);
  if (ret != 0) {
      printf ("Error: query_subscribe_topic() ret: %d\n", ret);
      return -1;
  }

  sleep (10);

  ret = query_close_connection (handle);
  if (ret != 0) {
      printf ("Error: query_close_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  return 0;
}

int
tc_send_tcp_message ()
{
  query_h handle;
  int ret = 0;
  char msg[] = "test custom message";

  ret = query_open_connection (&handle, NULL, NULL, NULL, NULL);
  if (ret != 0) {
      printf ("Error: query_open_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  ret = query_publish_tcp_data (handle, "nns_query/tcp/info",
      "localhost", 12345, TCP_BUSY, msg, strlen(msg), true);

  if (ret != 0) {
    printf ("Error: query_publish_tcp_data() ret: %d\n", ret);
    return -1;
  }
  sleep(2);

  ret = query_close_connection (handle);
  if (ret != 0) {
      printf ("Error: query_close_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  return 0;
}

int
tc_subscribe_tcp_message ()
{
  query_h handle;
  int ret = 0;

  ret = query_open_connection (&handle, NULL, NULL, NULL, NULL);
  if (ret != 0) {
      printf ("Error: query_open_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  ret = query_subscribe_topic (handle, "nns_query/tcp/info", tcp_msg_received_cb, NULL);
  if (ret != 0) {
      printf ("Error: query_subscribe_topic() ret: %d\n", ret);
      return -1;
  }
  sleep (10);

  ret = query_close_connection (handle);
  if (ret != 0) {
      printf ("Error: query_close_connection() ret: %d\n", ret);
      return -1;
  }
  sleep (2);

  return 0;
}

int
tc_subscribe_raw_message_with_wild_option ()
{
  query_h handle;
  int ret = 0;

  ret = query_open_connection (&handle, NULL, NULL, NULL, NULL);
  if (ret != 0) {
      printf ("Error: query_open_connection() ret: %d\n", ret);
      return -1;
  }
  sleep (2);

  ret = query_subscribe_topic (handle, "edge/inference/+/object_detection/#", raw_msg_received_cb, NULL);
  if (ret != 0) {
      printf ("Error: query_subscribe_topic() ret: %d\n", ret);
      return -1;
  }

  sleep (10);

  ret = query_close_connection (handle);
  if (ret != 0) {
      printf ("Error: query_close_connection() ret: %d\n", ret);
      return -1;
  }
  sleep (2);

  return 0;
}

int
tc_clear_retained_topic ()
{
  query_h handle;
  int ret = 0;

  ret = query_open_connection (&handle, NULL, NULL, NULL, NULL);
  if (ret != 0) {
      printf ("Error: query_open_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  ret = query_clear_retained_topic (handle, "edge/inference/galaxy_00/object_detection/mobilev3");
  if (ret != 0) {
    printf ("Error: tc_clear_retained_topic() ret: %d\n", ret);
    return -1;
  }
  sleep(2);

  ret = query_close_connection (handle);
  if (ret != 0) {
      printf ("Error: query_close_connection() ret: %d\n", ret);
      return -1;
  }
  sleep(2);

  return 0;
}

int main(int argc, char* argv[])
{
  int ret = 0;
  int cmd;

  cmd = atoi (argv[1]);
  switch (cmd) {
    case 0:
      ret = tc_open_close_connection();
      break;

    case 1:
      ret = tc_send_raw_message();
      break;

    case 2:
      ret = tc_subscribe_raw_message();
      break;

    case 3:
      ret = tc_send_tcp_message();
      break;

    case 4:
      ret = tc_subscribe_tcp_message();
      break;

    case 5:
      tc_subscribe_raw_message_with_wild_option();
      break;

    case 6:
      tc_clear_retained_topic();
      break;

    default:
      break;
  };

  if (ret != 0) {
    printf ("Test is failed!");
    return -1;
  }
  return 0;
}
