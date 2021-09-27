# GoogleTest requires at least C++11 and match the nnstreamer cpp version.
SET(CMAKE_CXX_STANDARD 14)

# Edge Sensor test
ADD_EXECUTABLE(test_edge_sensor test_edge_sensor.c)
TARGET_INCLUDE_DIRECTORIES(test_edge_sensor PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR})
TARGET_LINK_LIBRARIES(test_edge_sensor -L${PROJECT_BINARY_DIR}/src edge-sensor)
INSTALL (TARGETS test_edge_sensor DESTINATION ${BIN_INSTALL_DIR})

# NNStreamer Query test
ADD_EXECUTABLE(test_nnsquery test_nnsquery.c)
TARGET_INCLUDE_DIRECTORIES(test_nnsquery PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR})
TARGET_LINK_LIBRARIES(test_nnsquery -L${PROJECT_BINARY_DIR}/src nnsquery)

# Unit testcase
ADD_EXECUTABLE(unittest_edge_sensor unittest_edge_sensor.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_edge_sensor PRIVATE ${INCLUDE_DIR})
TARGET_LINK_LIBRARIES(unittest_edge_sensor ${TEST_REQUIRE_PKGS_LDFLAGS} edge-sensor)
