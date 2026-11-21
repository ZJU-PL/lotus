/**
 * @file PulseCheckerFixture.h
 * @brief Shared fixture and helpers for Pulse checker behavioral tests.
 *
 * Each Pulse*Test.cpp translation unit includes this header and defines a
 * group of TEST_F(PulseCheckerTest, ...) cases. Helpers that used to live in
 * an anonymous namespace in PulseCheckerTest.cpp are now protected members
 * so every test TU can use them without textual inclusion of .inc fragments.
 */

#pragma once

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"
#include "Checker/Pulse/Domain/PulseDisjunctiveDomain.h"
#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseReport.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>

class PulseCheckerTest : public ::testing::Test {
protected:
  llvm::LLVMContext context;

  void SetUp() override {
    pulse::DiagnosticManager::getInstance().clear();
    BugReportMgr::get_instance().clear_all_reports();
  }

  pulse::ExecutionDomain
  executeEntryBlock(pulse::PulseChecker &checker, llvm::Function *F,
                    const llvm::Instruction *stop_after = nullptr) {
    pulse::ExecutionDomain state = checker.initializeFunction(F);
    for (auto &I : F->getEntryBlock()) {
      auto states =
          checker.executeInstruction(&I, std::move(state), nullptr, 0);
      EXPECT_FALSE(states.empty());
      if (states.empty()) {
        return pulse::ExecutionDomain();
      }
      state = std::move(states.front());
      if (&I == stop_after) {
        break;
      }
    }
    return state;
  }

  static size_t getReportCountForType(BugReportMgr &mgr,
                                      llvm::StringRef bugTypeName) {
    int bugTypeId = mgr.find_bug_type(bugTypeName);
    if (bugTypeId < 0) {
      return 0;
    }
    const auto *reports = mgr.get_reports_for_type(bugTypeId);
    return reports ? reports->size() : 0;
  }

  static const BugReport *
  getLastReportForType(BugReportMgr &mgr, llvm::StringRef bugTypeName) {
    int bugTypeId = mgr.find_bug_type(bugTypeName);
    if (bugTypeId < 0) {
      return nullptr;
    }
    const auto *reports = mgr.get_reports_for_type(bugTypeId);
    if (!reports || reports->empty()) {
      return nullptr;
    }
    return reports->back();
  }

  static bool reportContainsTip(const BugReport *report,
                                llvm::StringRef tipSubstring) {
    if (!report) {
      return false;
    }
    for (const BugDiagStep *step : report->get_steps()) {
      if (step && llvm::StringRef(step->tip).contains(tipSubstring)) {
        return true;
      }
    }
    return false;
  }

  static const BugDiagStep *getLastStep(const BugReport *report) {
    if (!report || report->get_steps().empty()) {
      return nullptr;
    }
    return report->get_steps().back();
  }

  template <typename InstT>
  static InstT *findNthInstruction(llvm::Function *F, unsigned ordinal) {
    unsigned seen = 0;
    for (auto &BB : *F) {
      for (auto &I : BB) {
        if (auto *inst = llvm::dyn_cast<InstT>(&I)) {
          if (seen == ordinal) {
            return inst;
          }
          ++seen;
        }
      }
    }
    return nullptr;
  }
};
