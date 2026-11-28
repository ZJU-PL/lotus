/**
 * @file Analyzer.cpp
 * @brief Base analyzer class for fixpoint computation in SymAbsAI.
 *
 * This file contains the base Analyzer class implementation, which provides
 * the core fixpoint iteration logic and factory methods. The concrete analyzer
 * variants are implemented in separate files:
 * - UnilateralAnalyzer.cpp
 * - BilateralAnalyzer.cpp

 * Author: rainoftime
 */
#include "Verification/SymAbsAI/Core/Engine/Analyzer.h"

#include "Verification/SymAbsAI/Core/Foundation/DomainConstructor.h"
#include "Verification/SymAbsAI/Core/Foundation/Repr.h"
#include "Verification/SymAbsAI/Core/Foundation/ValueMapping.h"
#include "Verification/SymAbsAI/Core/Integration/ModuleContext.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Utils.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <atomic>
#include <queue>
#include <unordered_set>

#include <llvm/IR/CFG.h>
#include <llvm/Support/Timer.h>

namespace symabs_ai {

namespace {
std::atomic<std::uint64_t> g_bestTransformerCalls{0};
std::atomic<std::uint64_t> g_smtSolverCalls{0};
} // namespace

void Analyzer::resetBestTransformerCallCount() { g_bestTransformerCalls = 0; }
std::uint64_t Analyzer::getBestTransformerCallCount() {
  return g_bestTransformerCalls.load();
}
void Analyzer::countBestTransformerCall() { ++g_bestTransformerCalls; }

void Analyzer::resetSmtSolverCallCount() { g_smtSolverCalls = 0; }
std::uint64_t Analyzer::getSmtSolverCallCount() {
  return g_smtSolverCalls.load();
}
void Analyzer::countSmtSolverCall() { ++g_smtSolverCalls; }
Analyzer::Analyzer(const FunctionContext &fctx, const FragmentDecomposition &fd,
                   const DomainConstructor &dom, mode_t mode)
    : FunctionContext_(fctx), Fragments_(fd), Domain_(dom), Mode_(mode),
      CurrentFragment_(nullptr) {
  // initialize AbstractionPoints_ and FragMap_
  AbstractionPoints_.insert(Fragment::EXIT);
  for (auto &frag : Fragments_) {
    AbstractionPoints_.insert(frag.getStart());
    AbstractionPoints_.insert(frag.getEnd());

    for (auto *loc : frag.locations())
      FragMap_[loc].insert(&frag);
  }

  // initialize Results_ and Stable_
  auto *entry = &fctx.getFunction()->getEntryBlock();
  Stable_.insert(entry);
  Results_[entry] = createInitialValue(Domain_, entry, false);

  // abstract value associated with the entry node has to be top, except if
  // we only deliver dynamic results
  if (mode == FULL && Results_[entry])
    Results_[entry]->havoc();

  // Emit a CSV header in verbose output. Must match rows printed in
  // checkWithStats(). First column always contains "STATS" to make it
  // easy to filter out statistics using grep.
  vout << "STATS,function,fragment,result,time,conflicts,added_eqs\n";
}

std::unique_ptr<Analyzer> Analyzer::New(const FunctionContext &fctx,
                                        const FragmentDecomposition &frag,
                                        const DomainConstructor &domain,
                                        mode_t mode) {
  std::string variant = fctx.getConfig().get<std::string>("Analyzer", "Variant",
                                                          "UnilateralAnalyzer");

  if (variant == "UnilateralAnalyzer") {
    return std::make_unique<UnilateralAnalyzer>(fctx, frag, domain, mode);
  } else if (variant == "BilateralAnalyzer") {
    return std::make_unique<BilateralAnalyzer>(fctx, frag, domain, mode);
  } else if (variant == "OMTAnalyzer") {
    return std::make_unique<OMTAnalyzer>(fctx, frag, domain, mode);
  } else {
    llvm_unreachable("unknown analyzer variant");
  }
}

std::unique_ptr<Analyzer> Analyzer::New(const FunctionContext &fctx,
                                        const FragmentDecomposition &frag,
                                        mode_t mode) {
  return Analyzer::New(fctx, frag, DomainConstructor(fctx.getConfig()), mode);
}

bool Analyzer::bestTransformer(const AbstractValue *input,
                               const Fragment &fragment,
                               AbstractValue *result) const {
  assert(Mode_ != ONLY_DYNAMIC);
  if (!input || input->isBottom())
    return false;
  countBestTransformerCall();

  VOutBlock vout_block("best transformer for " + repr(fragment));
  CurrentFragment_ = &fragment;
  {
    VOutBlock vb("input");
    vout << *input;
  }

  z3::expr formula = FunctionContext_.formulaFor(fragment);
  ValueMapping vm_before =
      ValueMapping::atBeginning(FunctionContext_, fragment);

  z3::expr av_formula = input->toFormula(vm_before, FunctionContext_.getZ3());

#ifndef NDEBUG
  vout << "Analyzer::bestTransformer input->toFormula {{{\n"
       << av_formula << "\n"
       << "}}}\n";

  if (is_unsat(formula && av_formula)) {
    vout << "Analyzer::bestTransformer input->toFormula is UNSATISFIABLE\n";
  }
#endif

  ValueMapping vm_after = ValueMapping::atEnd(FunctionContext_, fragment);
  bool res = strongestConsequence(result, formula && av_formula, vm_after);

  {
    VOutBlock vb("result");
    vout << *result;
  }

  CurrentFragment_ = nullptr;
  return res;
}

/**
 * Lazily computes the abstract state at the beginning of a basic block.
 *
 * For non-abstraction points, this derives the state by composing a
 * sub-fragment from the closest abstraction point. For abstraction points,
 * it iterates a global fixpoint over all incoming fragments, using the
 * influence relation `Infl_` to invalidate and recompute dependents when
 * a point’s state gets refined. Dynamic results from `ResultStore` are
 * merged in when available.
 */
const Fragment *Analyzer::findSingleEdgeFragment(llvm::BasicBlock *src,
                                                 llvm::BasicBlock *dst) const {
  if (!src)
    return nullptr;
  auto it = FragMap_.find(src);
  if (it == FragMap_.end())
    return nullptr;
  for (const Fragment *f : it->second) {
    if (f->getStart() == src && f->getEnd() == dst && !f->includesEndBody() &&
        f->edges().size() == 1 && f->edges().count({src, dst})) {
      return f;
    }
  }
  return nullptr;
}

void Analyzer::stabilizeAll() {
  if (FixpointComputed_)
    return;
  FixpointComputed_ = true;

  if (Mode_ != FULL)
    return;

  std::unordered_map<llvm::BasicBlock *, std::vector<const Fragment *>>
      incomingByEnd;
  std::unordered_map<llvm::BasicBlock *, std::vector<const Fragment *>>
      outgoingByStart;
  for (const auto &frag : Fragments_) {
    incomingByEnd[frag.getEnd()].push_back(&frag);
    outgoingByStart[frag.getStart()].push_back(&frag);
  }

  for (llvm::BasicBlock *ap : AbstractionPoints_) {
    if (!Results_[ap])
      Results_[ap] = createInitialValue(Domain_, ap, false);
  }

  std::queue<llvm::BasicBlock *> work;
  std::unordered_set<llvm::BasicBlock *> inWork;
  auto enqueueSuccs = [&](llvm::BasicBlock *start) {
    auto it = outgoingByStart.find(start);
    if (it == outgoingByStart.end())
      return;
    for (const Fragment *frag : it->second) {
      llvm::BasicBlock *dst = frag->getEnd();
      if (inWork.insert(dst).second)
        work.push(dst);
    }
  };

  auto *entry = &FunctionContext_.getFunction()->getEntryBlock();
  if (Results_[entry] && !Results_[entry]->isBottom())
    enqueueSuccs(entry);

  std::unordered_map<llvm::BasicBlock *, unsigned> updateCount;
  constexpr unsigned kOuterWideningThreshold = 3;
  constexpr unsigned kOuterHavocCap = 16;

  while (!work.empty()) {
    llvm::BasicBlock *loc = work.front();
    work.pop();
    inWork.erase(loc);

    AbstractValue *output = Results_[loc].get();
    if (!output)
      continue;

    bool updated = false;
    auto inIt = incomingByEnd.find(loc);
    if (inIt != incomingByEnd.end()) {
      for (const Fragment *frag : inIt->second) {
        AbstractValue *input = Results_[frag->getStart()].get();
        if (!input || input->isBottom())
          continue;
        if (bestTransformer(input, *frag, output))
          updated = true;
      }
    }

    if (updated) {
      unsigned &cnt = updateCount[loc];
      ++cnt;
      if (cnt >= kOuterHavocCap) {
        if (!output->isTop())
          output->havoc();
        else
          continue;
      } else if (cnt >= kOuterWideningThreshold) {
        output->widen();
      }
      enqueueSuccs(loc);
    }
  }

  for (llvm::BasicBlock *ap : AbstractionPoints_)
    Stable_.insert(ap);
}

AbstractValue *Analyzer::at(llvm::BasicBlock *location) {
  auto *store = FunctionContext_.getModuleContext().getResultStore();
  if (store) {
    ResultStore::Key key(0); // Dummy key since dynamic analysis is disabled
    unique_ptr<AbstractValue> res =
        store->get(key, const_cast<FunctionContext *>(&FunctionContext_));
    if (res) {
      if (!Results_[location]) {
        Results_[location] = createInitialValue(Domain_, location, false);
      }
      Results_[location]->joinWith(*res.get());
      Stable_.insert(location);
      goto end;
    }
  }

  {
    if (AbstractionPoints_.find(location) == AbstractionPoints_.end()) {
      // for non-abstraction-points, only fixpoints are stored in Results_
      if (Results_[location])
        goto end;
      else
        Results_[location] = createInitialValue(Domain_, location, false);

      if (Mode_ == ONLY_DYNAMIC) {
        vout << "Results for non-abstraction point " << repr(location)
             << " are not being computed in the ONLY_DYNAMIC mode\n";
        goto end;
      }

      stabilizeAll();
      for (auto *parent_frag : FragMap_[location]) {
        auto sub_frag = FragmentDecomposition::SubFragment(
            *parent_frag, parent_frag->getStart(), location);

        VOutBlock vb("Computing result for non-abstraction point: " +
                     repr(sub_frag));

        AbstractValue *input = Results_[sub_frag.getStart()].get();
        bestTransformer(input, sub_frag, Results_[location].get());
      }

      goto end;
    }

    if (Stable_.find(location) != Stable_.end())
      goto end;

    if (!Results_[location])
      Results_[location] = createInitialValue(Domain_, location, false);

    if (Mode_ != FULL) {
      vout << "Result at abstraction point " << repr(location)
           << " will not be computed in unsound mode.\n";
      {
        VOutBlock vo("Already-present result");
        vout << repr(*Results_[location].get()) << "\n";
      }
      goto end;
    }

    stabilizeAll();
  }

end:

  if (store && Mode_ != ONLY_DYNAMIC) {
    ResultStore::Key key(0); // Dummy key since dynamic analysis is disabled
    store->put(key, *Results_[location].get());
  }
  return Results_[location].get();
}

/**
 * Returns the abstract state after executing a basic block.
 *
 * If the block is an abstraction point, this applies a single “body-only”
 * transformer to the already stabilized entry state. Otherwise it composes
 * an appropriate sub-fragment ending after the block and applies the best
 * transformer starting from the nearest abstraction point.
 */
AbstractValue *Analyzer::after(llvm::BasicBlock *location) {
  auto itr = BBEndResults_.find(location);
  if (itr != BBEndResults_.end())
    return itr->second.get(); // return cached result

  BBEndResults_[location] = createInitialValue(Domain_, location, true);
  AbstractValue *output = BBEndResults_[location].get();

  if (Mode_ == ONLY_DYNAMIC)
    return output;

  if (AbstractionPoints_.find(location) != AbstractionPoints_.end()) {
    // compute a single transformer just for the body of this BB
    auto frag =
        FragmentDecomposition::FragmentForBody(FunctionContext_, location);
    VOutBlock vb("Computing result for the body of " + repr(location));
    AbstractValue *input = at(location);
    bestTransformer(input, frag, output);
  } else {
    for (auto *parent_frag : FragMap_[location]) {
      auto sub_frag = FragmentDecomposition::SubFragment(
          *parent_frag, parent_frag->getStart(), location, true);

      VOutBlock vb("Computing result for BB end: " + repr(sub_frag));
      AbstractValue *input = at(sub_frag.getStart());
      bestTransformer(input, sub_frag, output);
    }
  }

  return output;
}

z3::check_result
Analyzer::checkWithStats(z3::solver *solver,
                         std::vector<z3::expr> *assumptions) const {
  countSmtSolverCall();
  z3::check_result z3_answer;

  // wrap the solver inside time measurements
  llvm::TimeRecord time_rec;
  time_rec -= llvm::TimeRecord::getCurrentTime(true);
  if (assumptions == nullptr || assumptions->size() == 0)
    z3_answer = solver->check();
  else
    z3_answer = solver->check(assumptions->size(), &(*assumptions)[0]);
  time_rec += llvm::TimeRecord::getCurrentTime(false);

  // extract some of the Z3 statistics
  auto stats = solver->statistics();
  unsigned conflicts = 0, added_eqs = 0;
  for (unsigned i = 0; i < stats.size(); ++i) {
    if (stats.key(i) == "conflicts")
      conflicts = stats.uint_value(i);

    if (stats.key(i) == "added eqs")
      added_eqs = stats.uint_value(i);
  }

  // emit a CSV record
  vout << "STATS," << repr(FunctionContext_.getFunction()) << ","
       << (uintptr_t)CurrentFragment_ << "," << repr(z3_answer) << ","
       << time_rec.getWallTime() << "," << conflicts << "," << added_eqs
       << "\n";

  return z3_answer;
}

std::unique_ptr<AbstractValue>
Analyzer::createInitialValue(DomainConstructor &domain, llvm::BasicBlock *bb,
                             bool after) const {
  if (after)
    return domain.makeBottom(FunctionContext_, bb, after);

  auto *store = FunctionContext_.getModuleContext().getResultStore();

  if (!store)
    return domain.makeBottom(FunctionContext_, bb, after);

  ResultStore::Key key(0); // Dummy key since dynamic analysis is disabled
  unique_ptr<AbstractValue> res =
      store->get(key, const_cast<FunctionContext *>(&FunctionContext_));
  if (!res)
    res = domain.makeBottom(FunctionContext_, bb, after);

  return res;
}

} // namespace symabs_ai
