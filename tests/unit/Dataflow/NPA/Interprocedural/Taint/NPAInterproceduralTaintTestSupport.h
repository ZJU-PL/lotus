/**
 * @file NPAInterproceduralTaintTestSupport.h
 * @brief Shared helpers for interprocedural NPA taint tests.
 *
 * Each NPAInterproceduralTaint*Test.cpp translation unit includes this
 * header and defines a group of TEST(NPA, ...) cases. Helpers that used to
 * be textually shared by including .inc fragments into a single wrapper TU
 * are now inline helpers here so every test TU is self-contained. Mirrors
 * tests/unit/Checker/Pulse/PulseCheckerFixture.h.
 */

#pragma once

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/NPA/Analyses/Inter/Taint.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

#include <string>

namespace {

using lotus::unittest::findBlock;
using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

inline std::string writeTempTaintConfig(llvm::StringRef content) {
  llvm::SmallString<128> tempPath;
  if (auto ec =
          llvm::sys::fs::createTemporaryFile("npa-taint", ".spec", tempPath)) {
    ADD_FAILURE() << "failed to create temp taint spec: " << ec.message();
    return "";
  }

  std::error_code ec;
  llvm::raw_fd_ostream os(tempPath, ec);
  if (ec) {
    ADD_FAILURE() << "failed to open temp taint spec: " << ec.message();
    return "";
  }
  os << content;
  os.close();
  return tempPath.str().str();
}

} // namespace
