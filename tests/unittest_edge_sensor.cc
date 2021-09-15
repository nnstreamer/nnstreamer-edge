/**
 * @file        unittest_edge_sensor.cc
 * @date        15 Sep 2021
 * @brief       Unit test for edge sensor.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Gichan Jang <gichan2.jang@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include "edge_sensor.h"
#include <string.h>
#include <stdlib.h>

/**
 * @brief Callback function to be called the connection state is changed.
 * @param[in,out] user_data User data that provided when calling edge_open_connection()
 * @param[in] state The new state of MQTT connection
 */
static void
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

/**
 * @brief Publish single message.
 */
TEST(edgeSensor, publishSingleMessage0) {
  edge_h handle;
  edge_mqtt_state_t state = MQTT_CONNECTION_LOST;
  char * topic_str = strdup ("TestTopic");
  char * caps_str = strdup ("");
  uint64_t buf_size = 10;
  uint8_t * buf = (uint8_t *) malloc (buf_size * sizeof(uint8_t));

  ASSERT_EQ (0, edge_open_connection (&handle, NULL, NULL, topic_str,
      0, 0, caps_str, state_change_cb, (void*) &state));
  usleep(200000L);

  EXPECT_EQ (0, edge_publish_single_msg (handle, buf, buf_size));
  usleep(200000L);

  EXPECT_EQ (0, edge_close_connection (handle));

  free (buf);
  free (topic_str);
  free (caps_str);
}

/**
 * @brief Publish single message with invalid param.
 */
TEST(edgeSensor, publishSingleMessage1_n) {
  uint64_t buf_size = 10;
  uint8_t * buf = (uint8_t *) malloc (buf_size * sizeof(uint8_t));

  EXPECT_NE (0, edge_publish_single_msg (NULL, buf, buf_size));
  usleep(200000L);

  free (buf);
}

/**
 * @brief Open connection with invalud param.
 */
TEST(edgeSensor, openConnection0_n) {
  edge_mqtt_state_t state = MQTT_CONNECTION_LOST;
  char * topic_str = strdup ("TestTopic");
  char * caps_str = strdup ("");

  ASSERT_NE (0, edge_open_connection (NULL, NULL, NULL, topic_str,
      0, 0, caps_str, state_change_cb, (void*) &state));
  usleep(200000L);

  free (topic_str);
  free (caps_str);
}

/**
 * @brief Publish single message with invalid param.
 */
TEST(edgeSensor, publish2_n) {
  edge_h handle;
  edge_mqtt_state_t state = MQTT_CONNECTION_LOST;
  char * topic_str = strdup ("TestTopic");
  char * caps_str = strdup ("");
  uint64_t buf_size = 10;

  ASSERT_EQ (0, edge_open_connection (&handle, NULL, NULL, topic_str,
      0, 0, caps_str, state_change_cb, (void*) &state));
  usleep(200000L);

  EXPECT_NE (0, edge_publish_single_msg (handle, NULL, buf_size));
  usleep(200000L);

  EXPECT_EQ (0, edge_close_connection (handle));

  free (topic_str);
  free (caps_str);
}

/**
 * @brief Publish single message with invalid param.
 */
TEST(edgeSensor, publish3_n) {
  edge_h handle;
  edge_mqtt_state_t state = MQTT_CONNECTION_LOST;
  char * topic_str = strdup ("TestTopic");
  char * caps_str = strdup ("");
  uint64_t buf_size = 0;
  uint8_t * buf = (uint8_t *) malloc (buf_size * sizeof(uint8_t));

  ASSERT_EQ (0, edge_open_connection (&handle, NULL, NULL, topic_str,
      0, 0, caps_str, state_change_cb, (void*) &state));
  usleep(200000L);

  EXPECT_NE (0, edge_publish_single_msg (handle, buf, buf_size));
  usleep(200000L);

  EXPECT_EQ (0, edge_close_connection (handle));

  free (buf);
  free (topic_str);
  free (caps_str);
}

/**
 * @brief Close connection with invalid param.
 */
TEST(edgeSensor, closeConnection0_n) {
    EXPECT_NE (0, edge_close_connection (NULL));
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
    fprintf(stderr, "catch 'testing::internal::<unnamed>::ClassUniqueToAlwaysTrue'");
  }

  try {
    result = RUN_ALL_TESTS ();
  } catch (...) {
    fprintf(stderr, "catch `testing::internal::GoogleTestFailureException`");
  }

  return result;
}
