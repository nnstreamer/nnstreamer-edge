# ML Edge MQTT test
ADD_EXECUTABLE(test_edge_mqtt test_edge_mqtt.c)
TARGET_INCLUDE_DIRECTORIES(test_edge_mqtt PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR})
TARGET_LINK_LIBRARIES(test_edge_mqtt -L${PROJECT_BINARY_DIR}/src capi-edge-mqtt)

INSTALL (TARGETS test_edge_mqtt DESTINATION ${BIN_INSTALL_DIR})

# GoogleTest requires at least C++11 and match the nnstreamer cpp version.
SET(CMAKE_CXX_STANDARD 14)

ADD_EXECUTABLE(unittest_edge_mqtt unittest_edge_mqtt.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_edge_mqtt PRIVATE ${INCLUDE_DIR})
TARGET_LINK_LIBRARIES(unittest_edge_mqtt ${TEST_REQUIRE_PKGS_LDFLAGS} capi-edge-mqtt)
