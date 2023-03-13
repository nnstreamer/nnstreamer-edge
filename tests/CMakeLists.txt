# nnstreamer-edge test
ADD_EXECUTABLE(unittest_nnstreamer-edge unittest_nnstreamer-edge.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_nnstreamer-edge PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(unittest_nnstreamer-edge ${TEST_REQUIRE_PKGS_LDFLAGS} nnstreamer-edge)
INSTALL (TARGETS unittest_nnstreamer-edge DESTINATION ${BIN_INSTALL_DIR})

# AITT test
IF(AITT_SUPPORT)
ADD_EXECUTABLE(unittest_nnstreamer-edge-aitt unittest_nnstreamer-edge-aitt.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_nnstreamer-edge-aitt PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(unittest_nnstreamer-edge-aitt ${TEST_REQUIRE_PKGS_LDFLAGS} nnstreamer-edge)
INSTALL (TARGETS unittest_nnstreamer-edge-aitt DESTINATION ${BIN_INSTALL_DIR})
ENDIF()

# MQTT test
IF(MQTT_SUPPORT)
ADD_EXECUTABLE(unittest_nnstreamer-edge-mqtt unittest_nnstreamer-edge-mqtt.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_nnstreamer-edge-mqtt PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(unittest_nnstreamer-edge-mqtt ${TEST_REQUIRE_PKGS_LDFLAGS} nnstreamer-edge)
INSTALL (TARGETS unittest_nnstreamer-edge-mqtt DESTINATION ${BIN_INSTALL_DIR})
ENDIF()
