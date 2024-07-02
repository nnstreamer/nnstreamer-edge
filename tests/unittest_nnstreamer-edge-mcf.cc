/**
 * @file        unittest_nnstreamer-edge-mcf.cc
 * @date        02 July 2024
 * @brief       Unittest for nnstreamer-edge MCF support.
 * @see         https://github.com/nnstreamer/nnstreamer-edge
 * @author      Gichan Jang <gichan2.jang@samsung.com>
 * @bug         No known bugs
 */

#include <gtest/gtest.h>
#include "nnstreamer-edge.h"
#include "nnstreamer-edge-mcf.h"
#include "nnstreamer-edge-log.h"
#include "nnstreamer-edge-util.h"

/**
 * @brief Connect to local host, multiple clients.
 */
TEST(edgeMCF, createHandle)
{
  GTEST_SKIP();
}

/**
 * @brief Main gtest
 */
int
main (int argc, char **argv)
{
  int result = -1;

  try {
    testing::InitGoogleTest (&argc, argv);
  } catch (...) {
    nns_edge_loge ("Caught exception, failed to init google test.");
  }

  try {
    result = RUN_ALL_TESTS ();
  } catch (...) {
    nns_edge_loge ("Caught exception, failed to run the unittest.");
  }

  return result;
}
