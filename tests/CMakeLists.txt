# Edge Sensor test
ADD_EXECUTABLE(test_edge_sensor test_edge_sensor.c)
TARGET_INCLUDE_DIRECTORIES(test_edge_sensor PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR})
TARGET_LINK_LIBRARIES(test_edge_sensor -L${PROJECT_BINARY_DIR}/src edge-sensor)

INSTALL (TARGETS test_edge_sensor DESTINATION ${BIN_INSTALL_DIR})
