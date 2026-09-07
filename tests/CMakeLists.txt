# nnstreamer-edge test
ADD_EXECUTABLE(unittest_nnstreamer-edge unittest_nnstreamer-edge.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_nnstreamer-edge PRIVATE ${TEST_REQUIRE_PKGS_INCLUDE_DIRS} ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(unittest_nnstreamer-edge ${TEST_REQUIRE_PKGS_LDFLAGS} ${NNS_EDGE_LIB_NAME})
INSTALL (TARGETS unittest_nnstreamer-edge DESTINATION ${BIN_INSTALL_DIR})
ADD_TEST(NAME unittest_nnstreamer-edge COMMAND unittest_nnstreamer-edge)
# A handle that is left with an unjoinable thread hangs the run rather than
# failing it, so bound the tests well above the few seconds they need.
SET_TESTS_PROPERTIES(unittest_nnstreamer-edge PROPERTIES TIMEOUT 300)

# Custom connection lib for unit test
IF(ENABLE_CUSTOM_CONNECTION)
SET(NNS_EDGE_CUSTOM_TEST_LIB_NAME nnstreamer-edge-custom-test)
SET(NNS_EDGE_CUSTOM_SRCS ${NNS_EDGE_SRCS} nnstreamer-edge-custom-test.c)
ADD_LIBRARY(${NNS_EDGE_CUSTOM_TEST_LIB_NAME} SHARED ${NNS_EDGE_CUSTOM_SRCS})
TARGET_INCLUDE_DIRECTORIES(${NNS_EDGE_CUSTOM_TEST_LIB_NAME} PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(${NNS_EDGE_CUSTOM_TEST_LIB_NAME} ${TEST_REQUIRE_PKGS_LDFLAGS} ${NNS_EDGE_LIB_NAME})
INSTALL (TARGETS ${NNS_EDGE_CUSTOM_TEST_LIB_NAME} DESTINATION ${CMAKE_INSTALL_LIBDIR})

ADD_EXECUTABLE(unittest_nnstreamer-edge-custom unittest_nnstreamer-edge-custom.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_nnstreamer-edge-custom PRIVATE ${TEST_REQUIRE_PKGS_INCLUDE_DIRS} ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(unittest_nnstreamer-edge-custom ${TEST_REQUIRE_PKGS_LDFLAGS} ${NNS_EDGE_LIB_NAME} ${NNS_EDGE_CUSTOM_TEST_LIB_NAME} ${CMAKE_DL_LIBS})
INSTALL (TARGETS unittest_nnstreamer-edge-custom DESTINATION ${BIN_INSTALL_DIR})
ADD_TEST(NAME unittest_nnstreamer-edge-custom COMMAND unittest_nnstreamer-edge-custom)
# The custom test loads the test connection library with dlopen at runtime.
SET_TESTS_PROPERTIES(unittest_nnstreamer-edge-custom PROPERTIES
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_CURRENT_BINARY_DIR}"
    TIMEOUT 300)
ENDIF()

# MQTT test
# Not registered with ADD_TEST(): this test requires a running MQTT broker,
# which the ubuntu cmake workflow does not provide (the GBS/TestHub path
# runs it directly with a broker).
IF(MQTT_SUPPORT)
ADD_EXECUTABLE(unittest_nnstreamer-edge-mqtt unittest_nnstreamer-edge-mqtt.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_nnstreamer-edge-mqtt PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(unittest_nnstreamer-edge-mqtt ${TEST_REQUIRE_PKGS_LDFLAGS} ${NNS_EDGE_LIB_NAME})
INSTALL (TARGETS unittest_nnstreamer-edge-mqtt DESTINATION ${BIN_INSTALL_DIR})
ENDIF()
