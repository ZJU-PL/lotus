/**
 * @file NPAInterproceduralClientTestSupport.h
 * @brief Shared helpers for interprocedural NPA client tests.
 *
 * Each NPAInterproceduralClient*Test.cpp translation unit includes this
 * header and defines a group of TEST(NPAInterproceduralClients, ...) cases.
 * Helpers that used to be textually shared by including .inc fragments into
 * a single wrapper TU are now inline helpers here so every test TU is
 * self-contained. Mirrors tests/unit/Checker/Pulse/PulseCheckerFixture.h.
 */

#pragma once

#include "Dataflow/NPA/Analyses/Inter/ConstantPropagation.h"
#include "Dataflow/NPA/Analyses/Inter/Interval.h"
#include "Dataflow/NPA/Analyses/Inter/LiveVariables.h"
#include "Dataflow/NPA/Analyses/Inter/MaybeUninitialized.h"
#include "Dataflow/NPA/Analyses/Inter/ReachingDefinitions.h"
#include "Dataflow/NPA/Domains/PredicateRelationDomain.h"
#include "Dataflow/NPA/Domains/TransformerSummary.h"
#include "Dataflow/NPA/LLVM/BackwardInterEngine.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

template <typename T>
std::vector<const T *> statesForBlock(const std::map<npa::BlockKey, T> &facts,
                                      const llvm::BasicBlock *block) {
  std::vector<const T *> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(&entry.second);
  }
  return out;
}

inline npa::TaintTransformer::fact_type unionFactForBlock(
    const std::map<npa::BlockKey, npa::TaintTransformer::fact_type> &facts,
    const llvm::BasicBlock *block) {
  bool found = false;
  npa::TaintTransformer::fact_type fact;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    if (!found) {
      fact = entry.second;
      found = true;
    } else {
      fact |= entry.second;
    }
  }
  EXPECT_TRUE(found);
  return fact;
}

template <typename Op, typename OpLess>
bool containsPath(const npa::TransformerSummaryValue<Op, OpLess> &transfer,
                  std::initializer_list<Op> expected) {
  typename npa::TransformerSummaryValue<Op, OpLess>::transformer_type path(
      expected);
  return transfer.transformers().count(path) != 0;
}

inline llvm::APInt signedAPInt(unsigned bitWidth, int64_t value) {
  return llvm::APInt(bitWidth, static_cast<uint64_t>(value), true);
}

inline llvm::APInt unsignedAPInt(unsigned bitWidth, uint64_t value) {
  return llvm::APInt(bitWidth, value, false);
}

inline void expectConstValue(const npa::ConstantPropagationValue &value,
                      const llvm::APInt &expected) {
  EXPECT_EQ(value.tag, npa::ConstantPropagationTag::Const);
  EXPECT_EQ(value.constant.getBitWidth(), expected.getBitWidth());
  EXPECT_TRUE(value.constant.eq(expected));
}

inline std::vector<std::pair<std::uint64_t, std::uint64_t>> sortedPredicateTransitions(
    const npa::PredicateRelationDomain::value_type &relation) {
  auto transitions = npa::PredicateRelationDomain::materialize(relation);
  std::sort(transitions.begin(), transitions.end());
  return transitions;
}

using EntryHookDomain = npa::TransformerSummary<char>;

struct ProjectedStringDomain {
  using value_type = std::set<std::string>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool project_newton_safe = true;

  static value_type zero() { return {}; }
  static value_type one() { return {""}; }

  static bool equal(const value_type &lhs, const value_type &rhs) {
    return lhs == rhs;
  }

  static value_type combine(const value_type &lhs, const value_type &rhs) {
    value_type out = lhs;
    out.insert(rhs.begin(), rhs.end());
    return out;
  }

  static value_type ndetCombine(const value_type &lhs, const value_type &rhs) {
    return combine(lhs, rhs);
  }

  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }

  static value_type extend(const value_type &lhs, const value_type &rhs) {
    value_type out;
    for (const auto &x : lhs) {
      for (const auto &y : rhs)
        out.insert(x + y);
    }
    return out;
  }

  static value_type extend_lin(const value_type &lhs, const value_type &rhs) {
    return extend(lhs, rhs);
  }

  static value_type subtract(const value_type &lhs, const value_type &rhs) {
    value_type out;
    for (const auto &item : lhs) {
      if (!rhs.count(item))
        out.insert(item);
    }
    return out;
  }

  static value_type project(const value_type &value) {
    value_type out;
    for (const auto &path : value) {
      std::string projected;
      projected.reserve(path.size());
      for (char ch : path) {
        if (ch != 'l')
          projected.push_back(ch);
      }
      out.insert(std::move(projected));
    }
    return out;
  }
};

class EntryHookAnalysis {
public:
  using FactType = EntryHookDomain::value_type;
  using E = npa::E0<EntryHookDomain>;
  using Exp = npa::Exp0<EntryHookDomain>;

  FactType getEntryValue() const { return EntryHookDomain::one(); }

  E buildBlockEntryExpr(llvm::BasicBlock &BB, E inExpr) {
    char label = 'B';
    if (BB.hasName() && !BB.getName().empty())
      label = static_cast<char>(std::toupper(BB.getName().front()));
    return Exp::seq(EntryHookDomain::singleton(label), inExpr);
  }

  E getTransfer(llvm::Instruction &, E currentPath) {
    ++transferBuilds;
    return currentPath;
  }

  unsigned getTransferBuilds() const { return transferBuilds; }

  FactType applySummary(const FactType &summary, const FactType &fact) {
    return EntryHookDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return EntryHookDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return EntryHookDomain::equal(lhs, rhs);
  }

private:
  unsigned transferBuilds = 0;
};

struct LimitedBoolDomain {
  using value_type = bool;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr long max_linear_steps = 0;

  static value_type zero() { return false; }
  static value_type one() { return true; }
  static bool equal(value_type lhs, value_type rhs) { return lhs == rhs; }
  static value_type combine(value_type lhs, value_type rhs) {
    return lhs || rhs;
  }
  static value_type ndetCombine(value_type lhs, value_type rhs) {
    return combine(lhs, rhs);
  }
  static value_type condCombine(bool phi, value_type t, value_type e) {
    return phi ? t : e;
  }
  static value_type extend(value_type lhs, value_type rhs) {
    return lhs && rhs;
  }
  static value_type extend_lin(value_type lhs, value_type rhs) {
    return extend(lhs, rhs);
  }
  static value_type subtract(value_type lhs, value_type rhs) {
    return lhs && !rhs;
  }
};

class LimitedBoolAnalysis {
public:
  using FactType = bool;
  using E = npa::E0<LimitedBoolDomain>;

  FactType getEntryValue() const { return true; }
  E getTransfer(llvm::Instruction &, E currentPath) const {
    return currentPath;
  }
  FactType applySummary(bool summary, bool fact) const {
    return summary && fact;
  }
  FactType joinFacts(bool lhs, bool rhs) const { return lhs || rhs; }
  bool factsEqual(bool lhs, bool rhs) const { return lhs == rhs; }
};

class ProjectedSummaryAnalysis {
public:
  using FactType = ProjectedStringDomain::value_type;
  using E = npa::E0<ProjectedStringDomain>;
  using Exp = npa::Exp0<ProjectedStringDomain>;

  FactType getEntryValue() const { return ProjectedStringDomain::one(); }

  E getTransfer(llvm::Instruction &inst, E currentPath) const {
    if (inst.getFunction() && inst.getFunction()->getName() == "callee" &&
        llvm::isa<llvm::ReturnInst>(&inst)) {
      return Exp::seq(ProjectedStringDomain::value_type{"l"}, currentPath);
    }
    return currentPath;
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return ProjectedStringDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return ProjectedStringDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return ProjectedStringDomain::equal(lhs, rhs);
  }
};

class PredicateProjectedLoopAnalysis {
public:
  using Domain = npa::PredicateRelationDomain;
  using FactType = Domain::value_type;
  using E = npa::E0<Domain>;
  using Exp = npa::Exp0<Domain>;

  FactType getEntryValue() const { return Domain::one(); }

  E getTransfer(llvm::Instruction &inst, E currentPath) const {
    if (inst.hasName() && inst.getName() == "set_local")
      return Exp::seq(Domain::assignConst(1, true), currentPath);
    return currentPath;
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return Domain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return Domain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return Domain::equal(lhs, rhs);
  }
};

using RecursiveSummaryDomain = npa::TransformerSummary<char>;

class RecursivePropagationLimitedForwardAnalysis {
public:
  using FactType = RecursiveSummaryDomain::value_type;
  using E = npa::E0<RecursiveSummaryDomain>;

  FactType getEntryValue() const { return RecursiveSummaryDomain::one(); }

  E getTransfer(llvm::Instruction &, E currentPath) const {
    return currentPath;
  }

  FactType getCallEntryTransfer(const llvm::CallBase &,
                                const llvm::Function &) const {
    return RecursiveSummaryDomain::zero();
  }

  FactType getCallReturnTransfer(const llvm::CallBase &,
                                 const llvm::Function &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType getCallToReturnTransfer(const llvm::CallBase &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return RecursiveSummaryDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::equal(lhs, rhs);
  }

  long getMaxPropagationSteps() const { return 0; }
};

class RecursivePropagationLimitedBackwardAnalysis {
public:
  using FactType = RecursiveSummaryDomain::value_type;
  using E = npa::E0<RecursiveSummaryDomain>;

  FactType getExitValue(const llvm::Function &) const {
    return RecursiveSummaryDomain::one();
  }

  E getTransfer(llvm::Instruction &, E currentPath) const {
    return currentPath;
  }

  FactType getCallReturnTransfer(const llvm::CallBase &,
                                 const llvm::Function &) const {
    return RecursiveSummaryDomain::zero();
  }

  FactType getCallEntryTransfer(const llvm::CallBase &,
                                const llvm::Function &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType getCallToReturnTransfer(const llvm::CallBase &) const {
    return RecursiveSummaryDomain::one();
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return RecursiveSummaryDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return RecursiveSummaryDomain::equal(lhs, rhs);
  }

  long getMaxPropagationSteps() const { return 0; }
};

inline void expectIntervalPoint(const npa::Interval &value,
                         const llvm::APInt &expected,
                         npa::IntervalOrdering ordering) {
  EXPECT_FALSE(value.bottom);
  EXPECT_TRUE(value.hasLower);
  EXPECT_TRUE(value.hasUpper);
  EXPECT_EQ(value.ordering, ordering);
  ASSERT_EQ(value.lower.getBitWidth(), expected.getBitWidth());
  ASSERT_EQ(value.upper.getBitWidth(), expected.getBitWidth());
  EXPECT_TRUE(value.lower.eq(expected));
  EXPECT_TRUE(value.upper.eq(expected));
}

inline void expectIntervalRange(const npa::Interval &value, const llvm::APInt &lower,
                         const llvm::APInt &upper,
                         npa::IntervalOrdering ordering) {
  EXPECT_FALSE(value.bottom);
  EXPECT_TRUE(value.hasLower);
  EXPECT_TRUE(value.hasUpper);
  EXPECT_EQ(value.ordering, ordering);
  ASSERT_EQ(value.lower.getBitWidth(), lower.getBitWidth());
  ASSERT_EQ(value.upper.getBitWidth(), upper.getBitWidth());
  EXPECT_TRUE(value.lower.eq(lower));
  EXPECT_TRUE(value.upper.eq(upper));
}

} // namespace

namespace {

using TraceTransferDomain = npa::TransformerSummary<char>;

struct BackwardTensorLangDomain {
  using value_type = std::set<std::string>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = false;
  static constexpr std::size_t MaxLen = 8;

  static value_type zero() { return {}; }
  static value_type one() { return {""}; }
  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    value_type out = a;
    out.insert(b.begin(), b.end());
    return out;
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type extend(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &lhs : a) {
      for (const auto &rhs : b) {
        std::string s = lhs + rhs;
        if (s.size() <= MaxLen)
          out.insert(std::move(s));
      }
    }
    return out;
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type subtract(const value_type &a, const value_type &) {
    return a;
  }
};

class BackwardTensorAnalysis {
public:
  using FactType = BackwardTensorLangDomain::value_type;
  using E = npa::E0<BackwardTensorLangDomain>;

  FactType getExitValue(const llvm::Function &) const {
    return BackwardTensorLangDomain::one();
  }

  E getTransfer(llvm::Instruction &, E current) const { return current; }

  FactType getCallReturnTransfer(const llvm::CallBase &,
                                 const llvm::Function &) const {
    return {std::string("r")};
  }

  FactType getCallEntryTransfer(const llvm::CallBase &,
                                const llvm::Function &) const {
    return {std::string("e")};
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return BackwardTensorLangDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return BackwardTensorLangDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return BackwardTensorLangDomain::equal(lhs, rhs);
  }
};

class BackwardEdgeTransferAnalysis {
public:
  using FactType = TraceTransferDomain::value_type;
  using E = npa::E0<TraceTransferDomain>;

  FactType getExitValue(const llvm::Function &) const {
    return TraceTransferDomain::one();
  }

  E getTransfer(llvm::Instruction &, E current) const { return current; }

  FactType getEdgeTransfer(const llvm::Instruction &term,
                           const llvm::BasicBlock &succ) const {
    auto *Branch = llvm::dyn_cast<llvm::BranchInst>(&term);
    if (!Branch || !Branch->isConditional())
      return TraceTransferDomain::one();
    return Branch->getSuccessor(0) == &succ
               ? TraceTransferDomain::singleton('T')
               : TraceTransferDomain::singleton('F');
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return TraceTransferDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return TraceTransferDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return TraceTransferDomain::equal(lhs, rhs);
  }
};

} // namespace

namespace npa {
template <> struct TensorSemiringTraits<BackwardTensorLangDomain> {
  using tensor_domain = ExactTensorProductLift<BackwardTensorLangDomain>;

  static bool available() { return true; }
  static bool paper_admissible() { return false; }

  static tensor_domain::value_type
  right_constant(const BackwardTensorLangDomain::value_type &v) {
    return domain_equal<BackwardTensorLangDomain>(
               v, BackwardTensorLangDomain::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(BackwardTensorLangDomain::one(), v);
  }

  static tensor_domain::value_type
  left_constant(const BackwardTensorLangDomain::value_type &v) {
    return domain_equal<BackwardTensorLangDomain>(
               v, BackwardTensorLangDomain::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(v, BackwardTensorLangDomain::one());
  }

  static tensor_domain::value_type
  constant(const BackwardTensorLangDomain::value_type &v) {
    return right_constant(v);
  }

  static tensor_domain::value_type
  couple(const BackwardTensorLangDomain::value_type &lhs,
         const BackwardTensorLangDomain::value_type &rhs) {
    return tensor_domain::singleton(lhs, rhs);
  }

  static BackwardTensorLangDomain::value_type
  readout(const tensor_domain::value_type &v) {
    return tensor_domain::project(v);
  }
};
} // namespace npa
