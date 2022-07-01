# nnstreamer-edge test
ADD_EXECUTABLE(unittest_nnstreamer-edge unittest_nnstreamer-edge.cc)
TARGET_INCLUDE_DIRECTORIES(unittest_nnstreamer-edge PRIVATE ${EDGE_REQUIRE_PKGS_INCLUDE_DIRS} ${INCLUDE_DIR} ${NNS_EDGE_SRC_DIR})
TARGET_LINK_LIBRARIES(unittest_nnstreamer-edge ${TEST_REQUIRE_PKGS_LDFLAGS} nnstreamer-edge)
INSTALL (TARGETS unittest_nnstreamer-edge DESTINATION ${BIN_INSTALL_DIR})
