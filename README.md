[![Code Coverage](https://nnstreamer.github.io/testresult/nnstreamer-edge/coverage_badge.svg)](https://nnstreamer.github.io/testresult/nnstreamer-edge/index.html)

# nnstreamer-edge
Remote source nodes for NNStreamer pipelines without GStreamer dependencies

Developers may provide stream inputs to NNStreamer pipelines from remote nodes with nnstreamer-edge.
NNStreamer-edge is designed not to depend on GStreamer and targets to be adopted by general RTOS as well.

If your system can afford running nnstreamer (gstreamer), don't use nnstreamer-edge. Use nnstreamer and gstreamer.

## Requirements
- This should not depend on glib, gstreamer, or nnstreamer
- This should not depend on other heavy-weighted libraries.
- This should be compatible not only with Linux, but also with RTOS and other OS; thus, this should try to stick with C basic libraries and POSIX libraries.
    - However, it may leave empty spaces that can be filled with "wrapper/plugin" for the portability. E.g., I may get too lazy to write communication layers; then, I'll leave it to the users to write down their own communication plugins.
    - This should not rely on Tizen, either. However, Tizen-RT might be considered as the primary target.
- Communication Layers includes:
    - Primary Targets: TCP, User Plugins.
    - Secondary Targets: Flatbuffers

# Quick start guide for NNStreamer-edge (Ubuntu 20.04 LTS or 22.04 LTS)
 - Note: This simple guide explains how to use NNStreamer-edge on the Ubuntu, but NNStreamer-edge is also available on Tizen, Android and TizenRT.
## Install via PPA
### Download nnstreamer-edge
```
$ sudo add-apt-repository ppa:nnstreamer/ppa
$ sudo apt-get install nnstreamer-edge
# Download nnstreamer for sample application
(optional) $ sudo apt-get install nnstreamer
```
### Run nnstreamer and nnstreamer-edge sample application
 - See pub/sub example: [Link](https://nnstreamer.github.io/tutorial3_pubsub_mqtt.html)
 - See query example: [Link](https://nnstreamer.github.io/tutorial4_query.html)

## How to build (Ubuntu 20.04 LTS or 22.04 LTS)

### Download prerequisite
```
$ sudo apt-get install cmake systemctl build-essential pkg-config mosquitto-dev libmosquitto-dev libgtest-dev libgmock-dev
# Run MQTT broker
$ sudo systemctl start mosquitto
```

### Build and Run test
```
# cd $NNST_EDGE_ROOT
$ cmake -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DENABLE_TEST=ON -DMQTT_SUPPORT=ON
$ make -C build install

# Run test
$ cd /usr/bin
$ ./unittest_nnstreamer-edge
$ ./unittest_nnstreamer-edge-mqtt
```

## License
- Apache 2.0
