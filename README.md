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
    - Primary Targets: TCP, UDP, User Plugins.
    - Secondary Targets: Flatbuffers

## License
- Apache 2.0
