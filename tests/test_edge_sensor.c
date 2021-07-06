
/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file	test_edge_sensor.c
 * @date	05 July 2021
 * @brief Publish the jpeg data as "TestTopic" topic name 10 times
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
 *     test_edge_sensor [Image path]
 *     (e.g. ./tests/test_edge_sensor ../tests/res in build directory)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "edge_sensor.h"

/**
 * @brief Get jpeg data and its size from the file path
 * @param[in] path jpeg file path
 * @param[out] buf image data of the input jpeg file
 * @param[out] buf_size image size of the input jpeg file
 */
int
read_jpeg_file (const char *path, uint8_t **buf, uint64_t *buf_size)
{
  uint8_t *out_buf;
  uint64_t curr_size = 0;
  uint64_t total_size = 0;

  FILE *fr = fopen(path,"rb");

  /* get file size */
  fseek(fr, 0, SEEK_END);
  *buf_size = (uint64_t)ftell(fr);

  /* allocate the memory space */
  out_buf = (uint8_t*)malloc(*buf_size);
  if (!out_buf) {
    printf ("Fail to malloc()\n");
    return -1;
  }
  
  /* get the data from file */
  fseek(fr, 0, SEEK_SET);
  while ((curr_size = fread (&out_buf[total_size], sizeof(uint8_t), *buf_size - total_size, fr)) > 0) {
    total_size += curr_size;
  }

  if (total_size != *buf_size) {
    printf ("Fail to fread(): different size\n");
    return -1;
  }

  fclose (fr);
  *buf = out_buf;
  return 0;
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
  uint8_t *buf;
  uint64_t buf_size;
  edge_h handle;
  edge_mqtt_state_t state = MQTT_CONNECTION_LOST;
  char path[512] = { 0 };
  int count  = 0;

  if (argc != 2) {
    printf ("Error!: Omit the image directory.\n");
    return -1;
  }
  
  int ret = edge_open_connection (&handle, 
      NULL, NULL, "TestTopic",
      0, 0, "", state_change_cb, (void*)&state);
  if (ret != 0) {
      printf ("Error: edge_open_connection() ret: %d\n", ret);
      return -1;
  }
  
  while (1) {
    usleep(200000L);

    if (state != MQTT_CONNECTED)
      continue;

    snprintf (path, 512, "%s/%d.jpg", argv[1], count++%3);
    read_jpeg_file (path, &buf, &buf_size);

    ret = edge_publish_single_msg (handle, buf, buf_size);
    if (ret != 0) {
      printf ("Error: edge_publish_single_msg() ret: %d\n", ret);
      return -1;
    }
    free(buf);

    usleep(200000L);
    if (count > 10)
      break;
  }

  ret = edge_close_connection (handle);
  if (ret != 0) {
      printf ("Error: edge_close_connection() ret: %d\n", ret);
      return -1;
  }

  printf ("Success: All %d test topics are sent successfully!\n", count-1);
  return 0;
}
