CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(nnstreamer-edge)

INCLUDE(FindPkgConfig)

OPTION(ENABLE_TEST      "Enable Test case" OFF)
OPTION(ENABLE_DEBUG     "Enable Debug" OFF)
OPTION(ENABLE_TIZEN     "Enable Tizen build" OFF)

# Default features. You may change the features according to your needs.
OPTION(MQTT_SUPPORT     "Enable MQTT" OFF)
OPTION(AITT_SUPPORT     "Enable AITT" OFF)

IF (NOT DEFINED VERSION)
    SET(VERSION    0.2.3)
ENDIF()

# GoogleTest requires at least C++11 and match the nnstreamer cpp version.
SET(CMAKE_CXX_STANDARD 14)

# Set CFLAGS
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Werror")
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} -pthread -fPIE -fPIC -g")

IF (ENABLE_DEBUG)
    SET(NNS_EDGE_FLAGS "-DDEBUG=1")
ELSE()
    SET(NNS_EDGE_FLAGS "-DDEBUG=0")
ENDIF()

# Set as-needed option
SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--as-needed")
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed")

SET(SO_VERSION    ${VERSION})
SET(PREFIX        ${CMAKE_INSTALL_PREFIX})
SET(EXEC_PREFIX   ${PREFIX}/bin)
SET(LIB_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR})
SET(INCLUDE_INSTALL_DIR "${PREFIX}/include/nnstreamer")

SET(SOURCE_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/src)
SET(INCLUDE_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/include)
SET(NNS_EDGE_SRC_DIR  ${SOURCE_DIR}/libnnstreamer-edge)
SET(NNS_EDGE_LIB_NAME nnstreamer-edge)

# Build options and required packages
SET(REQUIRE_PKGS "")

IF(ENABLE_TIZEN)
    ADD_DEFINITIONS(-D__TIZEN__=1)
    SET(REQUIRE_PKGS "${REQUIRE_PKGS} dlog")
ENDIF()

IF(NOT ${REQUIRE_PKGS} STREQUAL "")
    PKG_CHECK_MODULES(EDGE_REQUIRE_PKGS REQUIRED ${REQUIRE_PKGS})
ENDIF()

# MQTT Library
IF(MQTT_SUPPORT)
    FIND_LIBRARY(MOSQUITTO_LIB NAMES mosquitto)
    IF(NOT MOSQUITTO_LIB)
        PKG_SEARCH_MODULE(PAHO_MQTT_PKG "paho-mqtt-c")
        IF(PAHO_MQTT_PKG_FOUND)
            MESSAGE("Found Paho MQTT pkgconfig.")
            SET(PAHO_MQTT_LIB ${PAHO_MQTT_PKG_LDFLAGS})
        ELSE()
            FIND_LIBRARY(PAHO_MQTT_LIB NAMES paho-mqtt3a paho-mqtt3c paho-mqtt3as paho-mqtt3cs)

            IF(NOT PAHO_MQTT_LIB)
                MESSAGE(FATAL_ERROR "Cannot find paho-mqtt-c and mosquitto library.")
            ELSE()
                MESSAGE("Found Paho MQTT library.")
            ENDIF()
        ENDIF()
    ELSE()
        MESSAGE("FOUND MOSQUITTO LIB.")
    ENDIF()

    SET(NNS_EDGE_FLAGS "${NNS_EDGE_FLAGS} -DENABLE_MQTT=1")
ENDIF()

# AITT Library
IF(AITT_SUPPORT)
    FIND_LIBRARY(AITT_LIB NAMES aitt)
    IF(NOT AITT_LIB)
        MESSAGE(FATAL_ERROR "Cannot find AITT library.")
    ELSE()
        MESSAGE("Found AITT library.")
        SET(NNS_EDGE_FLAGS "${NNS_EDGE_FLAGS} -DENABLE_AITT=1")
        SET(AITT_INCLUDE_DIR  ${PREFIX}/include/aitt)
    ENDIF()
ENDIF()

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${NNS_EDGE_FLAGS}")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${NNS_EDGE_FLAGS}")

ADD_SUBDIRECTORY(src)

IF (ENABLE_TEST)
    PKG_CHECK_MODULES(TEST_REQUIRE_PKGS REQUIRED "gtest")
    ADD_SUBDIRECTORY(tests)
ENDIF()

# pkgconfig file
CONFIGURE_FILE(nnstreamer-edge.pc.in nnstreamer-edge.pc @ONLY)
INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/nnstreamer-edge.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
