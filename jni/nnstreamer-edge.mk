# This mk file defines common features to build NNStreamer library for Android.

ifndef NNSTREAMER_EDGE_ROOT
$(error NNSTREAMER_EDGE_ROOT is not defined!)
endif

# nnstreamer-edge headers
NNSTREAMER_EDGE_INCLUDES := \
    $(NNSTREAMER_EDGE_ROOT)/include

# nnstreamer-edge sources
NNSTREAMER_EDGE_SRCS := \
    $(NNSTREAMER_EDGE_ROOT)/src/libnnstreamer-edge/nnstreamer-edge-data.c \
    $(NNSTREAMER_EDGE_ROOT)/src/libnnstreamer-edge/nnstreamer-edge-event.c \
    $(NNSTREAMER_EDGE_ROOT)/src/libnnstreamer-edge/nnstreamer-edge-internal.c \
    $(NNSTREAMER_EDGE_ROOT)/src/libnnstreamer-edge/nnstreamer-edge-metadata.c \
    $(NNSTREAMER_EDGE_ROOT)/src/libnnstreamer-edge/nnstreamer-edge-queue.c \
    $(NNSTREAMER_EDGE_ROOT)/src/libnnstreamer-edge/nnstreamer-edge-util.c

# TODO: Add mqtt and aitt

