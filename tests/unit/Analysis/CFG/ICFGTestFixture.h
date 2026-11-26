/**
 * @file ICFGTestFixture.h
 * @brief Shared fixture for ICFG unit tests.
 *
 * Each ICFG*Test.cpp translation unit includes this header and defines a
 * group of TEST_F(ICFGTest, ...) cases. The fixture itself adds nothing
 * beyond LlvmModuleTest; it exists so test cases can live in separate
 * translation units without textual inclusion of .inc fragments.
 */

#pragma once

#include "IR/ICFG/GraphAnalysis.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <map>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

class ICFGTest : public lotus::unittest::LlvmModuleTest {};
