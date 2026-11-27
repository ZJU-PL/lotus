/**
 * @file SMTSolver.cpp
 * @brief Implementation of the SMTSolver class that provides the core SMT
 * solving interface
 *
 * This file implements SMTSolver, which serves as the main interface for
 * interacting with the underlying Z3 solver. The class supports:
 * - Adding and managing constraints
 * - Checking satisfiability with optional timeouts
 * - Fast approximation checking
 * - N-to-N query solving with under/over approximation
 * - Model generation for satisfiable formulas
 * - Push/pop for managing solver scopes
 *
 * The implementation uses Z3 for the actual solving, but provides a clean
 * abstraction layer for the rest of the system.
 */

#include "Solvers/SMT/LIBSMT/SMTSolver.h"

#include "Solvers/SMT/LIBSMT/SMTConfigure.h"
#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"
#include "Solvers/SMT/LIBSMT/SMTModel.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>

#include <llvm/Support/CommandLine.h>
#include <time.h>

#define DEBUG_TYPE "solver"

using namespace llvm;

static llvm::cl::opt<std::string>
    UsingSimplify("solver-simplify", llvm::cl::init(""),
                  llvm::cl::desc("Using online simplification technique. "
                                 "Candidates are local and dillig."));

static llvm::cl::opt<std::string> DumpingConstraintsDst(
    "dump-cnts-dst", llvm::cl::init(""),
    llvm::cl::desc(
        "If solving time is larger than the time that -dump-cnts-timeout, the "
        "constraints will be output the destination."));

static llvm::cl::opt<int> DumpingConstraintsTimeout(
    "dump-cnts-timeout",
    llvm::cl::desc("If solving time is too large (ms), the constraints will be "
                   "output to the destination that -dump-cnts-dst set."));

int SMTSolver::GlobalTimeout;

static llvm::cl::opt<int> EnableLocalSimplify(
    "enable-local-simplify", llvm::cl::init(0),
    llvm::cl::desc(
        "Enable local simplifications while adding a vector of constraints"));

// static llvm::cl::opt<int> EnableModelCache("enable-model-cache",
//		llvm::cl::init(0), llvm::cl::desc("Enable satisfying model
// cache"));

// only for debugging (single-thread)
bool SMTSolvingTimeOut = false;

SMTSolver::SMTSolver(SMTFactory *F, z3::solver &Z3Solver, z3::model &Z3Model)
    : SMTObject(F), Solver(Z3Solver), checkCount(0) {
  if (SMTSolver::GlobalTimeout > 0) {
    CurrentTimeout = static_cast<unsigned>(SMTSolver::GlobalTimeout);
    z3::params Z3Params(Z3Solver.ctx());
    Z3Params.set("timeout", CurrentTimeout);
    Z3Solver.set(Z3Params);
  }
}

SMTSolver::SMTSolver(const SMTSolver &Other)
    : SMTObject(Other), Solver(Other.Solver), checkCount(Other.checkCount),
      PendingAssertions(Other.PendingAssertions),
      ScopeAssertionMarks(Other.ScopeAssertionMarks),
      Z3ScopeMarks(Other.Z3ScopeMarks), Z3ScopeSatVars(Other.Z3ScopeSatVars),
      SyncedAsserts(Other.SyncedAsserts), ActiveAsts(Other.ActiveAsts),
      FalseCount(Other.FalseCount), CurrentTimeout(Other.CurrentTimeout),
      HasCachedResult(Other.HasCachedResult), CachedResult(Other.CachedResult),
      Z3ModelValid(Other.Z3ModelValid) {}

SMTSolver &SMTSolver::operator=(const SMTSolver &Other) {
  SMTObject::operator=(Other);
  if (this != &Other) {
    this->Solver = Other.Solver;
    this->checkCount = Other.checkCount;
    this->PendingAssertions = Other.PendingAssertions;
    this->ScopeAssertionMarks = Other.ScopeAssertionMarks;
    this->Z3ScopeMarks = Other.Z3ScopeMarks;
    this->Z3ScopeSatVars = Other.Z3ScopeSatVars;
    this->SyncedAsserts = Other.SyncedAsserts;
    this->ActiveAsts = Other.ActiveAsts;
    this->FalseCount = Other.FalseCount;
    this->CurrentTimeout = Other.CurrentTimeout;
    this->HasCachedResult = Other.HasCachedResult;
    this->CachedResult = Other.CachedResult;
    this->Z3ModelValid = Other.Z3ModelValid;
  }
  return *this;
}

SMTSolver::~SMTSolver() {}

uint64_t SMTSolver::currentSatVars() const {
  z3::stats St = Solver.statistics();
  const unsigned Sz = St.size();
  for (unsigned I = 0; I < Sz; ++I) {
    const char *Key = Z3_stats_get_key(Solver.ctx(), St, I);
    if (!Key)
      continue;
    if (std::strcmp(Key, "sat mk var") == 0 ||
        std::strcmp(Key, "sat-mk-var") == 0) {
      return St.is_uint(I) ? static_cast<uint64_t>(St.uint_value(I))
                           : static_cast<uint64_t>(St.double_value(I));
    }
  }
  return 0;
}

void SMTSolver::flushToZ3() {
  const size_t total = PendingAssertions.size();
  if (SyncedAsserts >= total)
    return;

  // Only materialize non-empty Z3 scopes so nested push() calls without new
  // assertions do not trigger redundant incremental solver internalization.
  for (size_t mark : ScopeAssertionMarks) {
    if (mark < SyncedAsserts)
      continue;
    if (mark >= total)
      break;
    while (SyncedAsserts < mark) {
      Solver.add(PendingAssertions[SyncedAsserts].Expr);
      ++SyncedAsserts;
    }
    if (Z3ScopeMarks.empty() || Z3ScopeMarks.back() < mark) {
      uint64_t vars_before_push = (SyncedAsserts > 0) ? currentSatVars() : 0;
      Solver.push();
      Z3ScopeMarks.push_back(mark);
      Z3ScopeSatVars.push_back(vars_before_push);
    }
  }

  while (SyncedAsserts < total) {
    Solver.add(PendingAssertions[SyncedAsserts].Expr);
    ++SyncedAsserts;
  }
}

SMTSolver::SMTResultType SMTSolver::check(unsigned Timeout) {
  if (FalseCount > 0) {
    return SMTResultType::SMTRT_Unsat;
  }
  if (PendingAssertions.empty()) {
    return SMTResultType::SMTRT_Sat;
  }
  if (HasCachedResult && Timeout == 0 &&
      CachedResult != SMTResultType::SMTRT_Unknown &&
      !DumpingConstraintsTimeout.getNumOccurrences() &&
      !UsingSimplify.getNumOccurrences()) {
    return CachedResult;
  }

  if (Timeout > 0) {
    this->setTimeout(Timeout);
  }

  z3::check_result Result;
  try {
    clock_t Start;
    if (DumpingConstraintsTimeout.getNumOccurrences()) {
      Start = clock();
    }

    if (UsingSimplify.getNumOccurrences()) {
      z3::solver Z3Solver4Sim(Solver.ctx());

      // 1. merge as one
      SMTExpr Whole = this->assertions().toAndExpr();

      // 2. simplify
      if (UsingSimplify.getValue() == "local") {
        Z3Solver4Sim.add(Whole.localSimplify().Expr);
      } else if (UsingSimplify.getValue() == "dillig") {
        SMTExpr SimplifiedForm = Whole.dilligSimplify();
        Z3Solver4Sim.add(SimplifiedForm.Expr);
      } else {
        Z3Solver4Sim.add(Whole.Expr);
      }

      Result = Z3Solver4Sim.check();
    } else {
      flushToZ3();
      Result = Solver.check();
      ++checkCount;
    }

    if (DumpingConstraintsTimeout.getNumOccurrences()) {
      double TimeCost = (double)(clock() - Start) * 1000 / CLOCKS_PER_SEC;
      if (TimeCost > DumpingConstraintsTimeout.getValue() &&
          DumpingConstraintsDst.getNumOccurrences()) {
        // output the constraints to a temp file in the dst
        std::string DstFileName = DumpingConstraintsDst.getValue();
        DstFileName.append("/case");
        DstFileName.append(std::to_string(clock()));
        DstFileName.append(".smt2");

        std::ofstream DstFile;
        DstFile.open(DstFileName);

        if (DstFile.is_open()) {
          DstFile << *this << "\n";
          DstFile.close();
        } else {
          std::cerr << "File cannot be opened: " << DstFileName << "\n";
        }

        SMTSolvingTimeOut = true;
      } else {
        SMTSolvingTimeOut = false;
      }
    } else {
    }
  } catch (z3::exception &Ex) {
    std::cerr << __FILE__ << " : " << __LINE__ << " : " << Ex << "\n";
    return SMTResultType::SMTRT_Unknown;
  }

  // Use a return value to suppress gcc warning
  SMTResultType RetVal = SMTResultType::SMTRT_Unknown;

  switch (Result) {
  case z3::check_result::sat:
    RetVal = SMTResultType::SMTRT_Sat;
    break;
  case z3::check_result::unsat:
    RetVal = SMTResultType::SMTRT_Unsat;
    break;
  case z3::check_result::unknown:
    RetVal = SMTResultType::SMTRT_Unknown;
    break;
  }

  // return to the default timeout setting
  if (Timeout > 0) {
    if (SMTSolver::GlobalTimeout > 0) {
      this->setTimeout((unsigned)SMTSolver::GlobalTimeout);
    }
  }
  if (RetVal != SMTResultType::SMTRT_Unknown &&
      !UsingSimplify.getNumOccurrences()) {
    HasCachedResult = true;
    CachedResult = RetVal;
    Z3ModelValid = (RetVal == SMTResultType::SMTRT_Sat);
  } else if (!UsingSimplify.getNumOccurrences()) {
    Z3ModelValid = false;
  }
  return RetVal;
}

// This can filter very trivial cases, such as "x + y == 2"(sat), " x > 2 && x
// <=2"(unsat);
// TODO: currently we need to get assertions from solver, and then apply a
// tactic; better to provide a API for z3 solver.
SMTSolver::SMTResultType SMTSolver::fastCheck(unsigned Timeout) {
  SMTExpr Whole = this->assertions().toAndExpr();
  // bv-fast-check() is too week. I will use a restricted form of bit-blasting
  // solver
  z3::goal G(Solver.ctx());
  G.add(Whole.Expr);
  z3::tactic fastCheckTactic =
      z3::try_for(z3::tactic(Solver.ctx(), "bv-fast-check"), Timeout);
  Z3_lbool Res = fastCheckTactic(G)[0].as_expr().bool_value();

  SMTResultType RetVal = SMTResultType::SMTRT_Unknown;

  switch (Res) {
  case Z3_L_TRUE:
    RetVal = SMTResultType::SMTRT_Sat;
    break;
  case Z3_L_FALSE:
    RetVal = SMTResultType::SMTRT_Unsat;
    break;
  default:
    RetVal = SMTResultType::SMTRT_Unknown;
    break;
  }
  return RetVal;
}

bool SMTSolver::checkN2NQueryWithUnderAppro(
    SMTExprVec &FVec, std::vector<SMTSolver::SMTResultType> &ResVec) {
  SMTExpr CommonVC = this->assertions().toAndExpr();
  SMTExpr UnderApproExpr = CommonVC && FVec.toAndExpr();
  SMTSolver TmpSol = CommonVC.Factory->createSMTSolver();
  TmpSol.add(UnderApproExpr);
  if (TmpSol.check() == SMTSolver::SMTRT_Sat) {
    for (unsigned I = 0; I < ResVec.size(); I++) {
      ResVec[I] = SMTSolver::SMTRT_Sat;
    }
    return true;
  } else {
    // TODO: should we return false, or solve the SMTExpr in FVec one-by-one, or
    // apply "unsat-core based refinement?"
    return false;
  }
}

bool SMTSolver::overApproCheckMisc(
    SMTExpr &G, SMTExprVec &FVec,
    std::vector<SMTSolver::SMTResultType> &Labels) {
  unsigned K = FVec.size();
  SMTExpr Total = G.getSMTFactory().createBoolVal(false);
  for (unsigned I = 0; I < K; I++) {
    // Merge formulas that are not determined yet
    if (Labels[I] == SMTSolver::SMTRT_Unknown) {
      Total = Total || FVec[I];
    }
  }
  if (Total.isFalse()) {
    // This indicates that no labels are Unknown, which means we can finish the
    // algorithm
    return true;
  }
  Total = Total && G;

  SMTSolver Sol = G.getSMTFactory().createSMTSolverWithTactic(
      "qfbv"); // or pp_qfbv, pp_qfbv_light
  Sol.add(Total);

  auto Res = Sol.check();
  if (Res == SMTSolver::SMTRT_Unsat) {
    // current over-appro successes!!
    for (unsigned I = 0; I < K; I++) {
      if (Labels[I] == SMTSolver::SMTRT_Unknown) {
        Labels[I] = SMTSolver::SMTRT_Unsat;
      }
    }
    // finish the algorithm.
    return true;
  } else if (Res == SMTSolver::SMTRT_Sat) {
    // Use model-based filtering/refinement
    SMTModel M = Sol.getSMTModel();
    for (unsigned I = 0; I < K; I++) {
      if (Labels[I] == SMTSolver::SMTRT_Unknown) {
        auto TmpRes = M.getBoolValue(FVec[I], true);
        if (TmpRes.second) {
          // true under this model
          if (TmpRes.first) {
            Labels[I] = SMTSolver::SMTRT_Sat;
          } // else {
            //    Labels[I] = LUndef;
            //}
        } else {
          // Exception.
          return false;
        }
      }
    }
    overApproCheckMisc(G, FVec, Labels);

  } else {
    // If any call to solver times out, the remaining labels are Unknown
    // This is not a good idea..
    return false;
  }

  return true;
}

bool SMTSolver::checkN2NQueryWithOverAppro(
    SMTExprVec &FVec, std::vector<SMTSolver::SMTResultType> &ResVec) {
  // TODO: should we initialize ResVec here or before calling this function?
  for (unsigned I = 0; I < FVec.size(); I++) {
    ResVec.push_back(SMTSolver::SMTRT_Unknown);
  }
  SMTExpr CommonVC = this->assertions().toAndExpr();

  return overApproCheckMisc(CommonVC, FVec, ResVec);

  // for (unsigned I = 0; I < FVec.size(); I++) {
  //    std::cout << ResVec[I] << '\n';
  //}
}

// instead of using the global timeout option, setting timeout explicitly for a
// SMTSolver
void SMTSolver::setTimeout(unsigned Timeout) {
  if (Timeout > 0) {
    CurrentTimeout = Timeout;
    z3::params Z3Params(Solver.ctx());
    Z3Params.set("timeout", Timeout);
    Solver.set(Z3Params);
  }
}

void SMTSolver::push() {
  ScopeAssertionMarks.push_back(PendingAssertions.size());
}

void SMTSolver::pop(unsigned N) {
  if (N == 0)
    return;
  if (N > ScopeAssertionMarks.size())
    N = static_cast<unsigned>(ScopeAssertionMarks.size());
  if (N == 0)
    return;

  const size_t target_depth = ScopeAssertionMarks.size() - N;
  const size_t new_size = ScopeAssertionMarks[target_depth];
  ScopeAssertionMarks.resize(target_depth);

  if (new_size < PendingAssertions.size()) {
    for (size_t i = new_size; i < PendingAssertions.size(); ++i) {
      if (PendingAssertions[i].isApp() && PendingAssertions[i].isFalse())
        --FalseCount;
      ActiveAsts.erase(static_cast<Z3_ast>(PendingAssertions[i].Expr));
    }
    PendingAssertions.erase(
        PendingAssertions.begin() + static_cast<std::ptrdiff_t>(new_size),
        PendingAssertions.end());
    HasCachedResult = false;
    Z3ModelValid = false;
  }

  unsigned z3_pops = 0;
  uint64_t popped_base_vars = std::numeric_limits<uint64_t>::max();
  const size_t old_synced = SyncedAsserts;
  while (!Z3ScopeMarks.empty() && Z3ScopeMarks.back() >= new_size) {
    SyncedAsserts = Z3ScopeMarks.back();
    popped_base_vars = Z3ScopeSatVars.back();
    Z3ScopeMarks.pop_back();
    Z3ScopeSatVars.pop_back();
    ++z3_pops;
  }
  if (z3_pops > 0) {
    const size_t popped_asserts = old_synced - SyncedAsserts;
    bool should_reset = (new_size == 0);
    if (!should_reset && popped_asserts > 0) {
      uint64_t cur_vars = currentSatVars();
      uint64_t delta_vars =
          (cur_vars > popped_base_vars) ? (cur_vars - popped_base_vars) : 0;
      // Avoid quadratic sat::solver::gc_vars() / erase_clause_watch() in Z3's
      // incremental QF_BV solver when popping a massive scope; reset() clears
      // clauses in O(N) while PendingAssertions preserves the exact outer
      // assertions for lazy re-flush on the next check().
      if (delta_vars >= 65536)
        should_reset = true;
    }
    try {
      if (should_reset) {
        Solver.reset();
        if (CurrentTimeout > 0)
          setTimeout(CurrentTimeout);
        Z3ScopeMarks.clear();
        Z3ScopeSatVars.clear();
        SyncedAsserts = 0;
      } else {
        Solver.pop(z3_pops);
      }
    } catch (z3::exception &Ex) {
      std::cerr << __FILE__ << " : " << __LINE__ << " : " << Ex << "\n";
      exit(1);
    }
    Z3ModelValid = false;
  }
}

unsigned SMTSolver::getNumScopes() {
  return static_cast<unsigned>(ScopeAssertionMarks.size());
}

void SMTSolver::add(SMTExpr E) {
  if (E.isApp() && E.isTrue()) {
    return;
  }
  Z3_ast ast = static_cast<Z3_ast>(E.Expr);
  if (!ActiveAsts.insert(ast).second) {
    return;
  }
  if (E.isApp() && E.isFalse()) {
    ++FalseCount;
  }
  PendingAssertions.push_back(std::move(E));
  HasCachedResult = false;
  Z3ModelValid = false;
}

void SMTSolver::addAll(SMTExprVec EVec) {
  if (EVec.empty())
    return;
  // TODO: more tactics
  // 1. Add one by one(the default tactic)
  // 2. Call toAnd(EVec), and add the returned formula
  // 3. Call toAnd(EVec), add add a simplified version of the returned formula
  // 4. Take the size of EVec into considerations; choose a proper method for
  // simplification
  switch (EnableLocalSimplify.getValue()) {
  case 0:
    for (unsigned I = 0; I < EVec.size(); I++) {
      add(EVec[I]);
    }
    break;
  case 1:
    add(EVec.toAndExpr());
    break;
  case 2:
    add(EVec.toAndExpr().localSimplify());
    break;
  case 3:
    add(EVec.toAndExpr().localSimplify().doConstantPropagation());
    break;
  default:
    for (unsigned I = 0; I < EVec.size(); I++) {
      add(EVec[I]);
    }
    break;
  }
}

void SMTSolver::addAll(const std::vector<SMTExpr> &EVec) {
  // TODO: add EnableLocalSimplify option for this.
  for (unsigned I = 0; I < EVec.size(); I++) {
    add(EVec[I]);
  }
}

SMTExprVec SMTSolver::assertions() {
  if (PendingAssertions.empty())
    return getSMTFactory().createEmptySMTExprVec();
  auto Vec = std::make_shared<z3::expr_vector>(Solver.ctx());
  for (const auto &E : PendingAssertions)
    Vec->push_back(E.Expr);
  return SMTExprVec(&getSMTFactory(), Vec);
}

void SMTSolver::reset() {
  if (!Z3ScopeMarks.empty() || SyncedAsserts > 0) {
    Solver.reset();
    if (CurrentTimeout > 0)
      setTimeout(CurrentTimeout);
  }
  PendingAssertions.clear();
  ScopeAssertionMarks.clear();
  Z3ScopeMarks.clear();
  Z3ScopeSatVars.clear();
  SyncedAsserts = 0;
  ActiveAsts.clear();
  FalseCount = 0;
  HasCachedResult = false;
  CachedResult = SMTRT_Uncheck;
  Z3ModelValid = false;
  checkCount = 0;
}

bool SMTSolver::operator<(const SMTSolver &Solver) const {
  return ((Z3_solver)this->Solver) < ((Z3_solver)Solver.Solver);
}

SMTModel SMTSolver::getSMTModel() {
  try {
    if (!Z3ModelValid || SyncedAsserts < PendingAssertions.size()) {
      flushToZ3();
      if (Solver.check() == z3::sat)
        Z3ModelValid = true;
      ++checkCount;
    }
    return SMTModel(&getSMTFactory(), Solver.get_model());
  } catch (z3::exception &e) {
    std::cerr << __FILE__ << " : " << __LINE__ << " : " << e << "\n";
    exit(1);
  }
}
