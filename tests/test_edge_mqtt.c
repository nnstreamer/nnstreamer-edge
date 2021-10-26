
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file    test_edge_mqtt.c
 * @date    05 July 2021
 * @brief   Publish the jpeg data as "TestTopic" topic name 10 times
 * @see     https://github.com/nnstreamer/nnstreamer-edge
 * @author  Sangjung Woo <sangjung.woo@samsung.com>
 *
 * @details Usage examples
 *   This test case publishes the jpeg data as "TestTopic" topic name 10 times.
 *   If data is successfully received, then the image is shown on the server-side.
 *
 *   Server Side:
 *   gst-launch-1.0 \
 *     mqttsrc debug=1 sub-topic=TestTopic host=localhost sub-timeout=9223372036854775807 ! \
 *     jpegdec ! video/x-raw,framerate=0/1 ! \
 *     videoscale ! videoconvert ! ximagesink qos=0
 *
 *   Client Side:
 *     $ ./test_edge_mqtt
 *     Usage: ./tests/test_edge_mqtt [-f jpeg_file] [-d directory] [-c count]
 *     # Publish single image
 *     $ ./test_edge_mqtt -f ./0.jpg
 *     # Publish multiple images 10 times
 *     $ ./tests/test_edge_mqtt -d ../tests/res -c 10
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "ml-edge-mqtt.h"

/**
 * @brief Get jpeg data and its size from the file path
 * @param[in] path jpeg file path
 * @param[out] buf image data of the input jpeg file
 * @param[out] buf_size image size of the input jpeg file
 */
static int
read_jpeg_file (const char *path, uint8_t **buf, uint64_t *buf_size)
{
  uint8_t *out_buf;
  uint64_t curr_size = 0;
  uint64_t total_size = 0;
  int ret = 0;

  FILE *fr = fopen(path,"rb");
  if (!fr) {
    printf ("Fail to fopen()\n");
    return -1;
  }

  /* get file size */
  fseek(fr, 0, SEEK_END);
  *buf_size = (uint64_t)ftell(fr);

  /* allocate the memory space */
  out_buf = (uint8_t*)malloc(*buf_size);
  if (!out_buf) {
    printf ("Fail to malloc()\n");
    ret = -1;
    goto cleanup;
  }
  
  /* get the data from file */
  fseek(fr, 0, SEEK_SET);
  while ((curr_size = fread (&out_buf[total_size], sizeof(uint8_t), *buf_size - total_size, fr)) > 0) {
    total_size += curr_size;
  }

  if (total_size != *buf_size) {
    printf ("Fail to fread(): different size\n");
    free (out_buf);
    out_buf = NULL;
    ret = -1;
    goto cleanup;
  }
  *buf = out_buf;

cleanup:
  fclose (fr);
  return ret;
}

/**
 * @brief Callback function to be called the connection state is changed.
 * @param[in,out] user_data User data that provided when calling edge_open_connection()
 * @param[in] state The new state of MQTT connection
 */
void
state_change_cb (void *user_data, edge_mqtt_state_t state)
{
  edge_mqtt_state_t *s = (edge_mqtt_state_t*)user_data;
  switch (state) {
    case MQTT_CONNECTED:
      *s = MQTT_CONNECTED;
      printf(" - User: State change: MQTT_CONNECTED\n");
      break;

    case MQTT_DISCONNECTED:
      *s = MQTT_DISCONNECTED;
      printf(" - User: State change: MQTT_DISCONNECTED\n");
      break;
    
    default:
      printf(" - User: State change: Default\n");
      break;
  }
  return ;
}

int
main(int argc, char* argv[])
{
  int opt;
  edge_h handle;
  edge_mqtt_state_t state = MQTT_CONNECTION_LOST;
  char path[512] = { 0 };
  uint8_t *buf;
  uint64_t buf_size;
  int curr_count = 1;
  int max_count = 0;
  bool flag_single_file = false;

  if (argc == 1) {
    fprintf(stderr, "Usage: %s [-f jpeg_file] [-d directory] [-c count]\n", argv[0]);
    exit (0);
  }

  while ((opt = getopt(argc, argv, "f:c:d:")) != -1) {
    switch (opt) {
      case 'f':
        /* Single image file */
        flag_single_file = true;
        memcpy (path, optarg, strlen (optarg));
        break;
      case 'c':
        /* Max Count */
        max_count = atoi (optarg);
        break;
      case 'd':
        flag_single_file = false;
        memcpy (path, optarg, strlen (optarg));
        break;

      default:
        fprintf(stderr, "Usage: %s [-f jpeg_file] [-d directory] [-c count]\n", argv[0]);
        exit (0);
    }
  }

  int ret = edge_open_connection (&handle, 
      NULL, NULL,
      0, state_change_cb, (void*)&state);
  if (ret != 0) {
      printf ("Error: edge_open_connection() ret: %d\n", ret);
      return -1;
  }
  usleep(200000L);

  while (1) {
    if (flag_single_file) {
      read_jpeg_file (path, &buf, &buf_size);
    } else {
      char local_path[1024] = { 0 };
      snprintf (local_path, 1024, "%s/%d.jpg", path, curr_count%3);
      read_jpeg_file (local_path, &buf, &buf_size);
    }
    ret = edge_publish_msg_for_mqttsrc (handle, "TestTopic", buf, buf_size);
    if (ret != 0) {
      printf ("Error: edge_publish_msg_for_mqttsrc() ret: %d\n", ret);
      return -1;
    }

    free(buf);
    usleep(200000L);
    if (++curr_count > max_count)
      break;
  }

  ret = edge_close_connection (handle);
  if (ret != 0) {
      printf ("Error: edge_close_connection() ret: %d\n", ret);
      return -1;
  }
  return 0;
}
