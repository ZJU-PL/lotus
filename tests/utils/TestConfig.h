#ifndef LOTUS_TESTS_UTILS_TESTCONFIG_H_
#define LOTUS_TESTS_UTILS_TESTCONFIG_H_

#include <llvm/ADT/StringRef.h>
#include <gtest/gtest.h>
#include <string>

namespace lotus {
namespace testing {

// Test configuration for Lotus testing infrastructure
static constexpr llvm::StringLiteral PathToLLTestFiles =
    LOTUS_BUILD_DIR "/tests/test_data/";

static constexpr llvm::StringLiteral PathToTxtTestFiles =
    LOTUS_BUILD_DIR "/tests/test_data/";

static constexpr llvm::StringLiteral PathToJSONTestFiles =
    LOTUS_SRC_DIR "/tests/test_data/";

#define LOTUS_BUILD_SUBFOLDER(SUB)                                            \
  llvm::StringLiteral(LOTUS_BUILD_DIR "/tests/test_data/" SUB)

// Remove wrapped tests in case GTEST_SKIP is not available
#ifdef GTEST_SKIP
#define LOTUS_SKIP_TEST(...) __VA_ARGS__
#else
#define LOTUS_SKIP_TEST(...)
#endif

#ifdef _LIBCPP_VERSION
#define LIBCPP_GTEST_SKIP GTEST_SKIP();
#else
#define LIBCPP_GTEST_SKIP
#endif

} // namespace testing
} // namespace lotus

#endif // LOTUS_TESTS_UTILS_TESTCONFIG_H_
