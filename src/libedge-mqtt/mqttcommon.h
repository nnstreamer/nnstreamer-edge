/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file    mqttcommon.h
 * @date    08 Mar 2021
 * @brief   Common macros and utility functions for GStreamer MQTT plugins
 * @see     https://github.com/nnstreamer/nnstreamer
 * @see     https://github.com/nnstreamer/nnstreamer-edge
 * @author  Wook Song <wook16.song@samsung.com>
 *          Sangjung Woo <sangjung.woo@samsung.com>
 * @bug     No known bugs except for NYI items
 * @note    To remove glib dependency, this file uses fixed width integer types (i.e. stdint.h) instead of GLib types.
 */
#ifndef __EDGE_MQTT_COMMON_H__
#define __EDGE_MQTT_COMMON_H__

#include <stdint.h>

#define GST_MQTT_MAX_NUM_MEMS         16
#define GST_MQTT_LEN_MSG_HDR          1024
#define GST_MQTT_MAX_LEN_GST_CPAS_STR 512

#define GST_US_TO_NS_MULTIPLIER 1000

typedef struct _GstMQTTMessageHdr {
  union {
    struct {
      uint32_t num_mems;
      uint64_t size_mems[GST_MQTT_MAX_NUM_MEMS];

      int64_t base_time_epoch;
      int64_t sent_time_epoch;
     
      uint64_t duration;
      uint64_t dts;
      uint64_t pts;
      char gst_caps_str[GST_MQTT_MAX_LEN_GST_CPAS_STR];
    };
    uint8_t _reserved_hdr[GST_MQTT_LEN_MSG_HDR];
  };
} GstMQTTMessageHdr;

#endif /* __EDGE_MQTT_COMMON_H__ */
