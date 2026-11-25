/** @file Query.h @brief Bug query hierarchy, owning handle, and guarded query set. */
#pragma once

#include "Solvers/SMT/LIBSMT/SMTExpr.h"
#include "SymbolicExecution/Checks/BugTypes.h"
#include "SymbolicExecution/Domain/PropertyAllocator.h"
#include "SymbolicExecution/Domain/PropertyValue.h"
#include "SymbolicExecution/Engine/GuardedValue.h"
#include "SymbolicExecution/Solver/PathCondSolver.h"
#include "SymbolicExecution/Support/AnalysisLimit.h"
#include "SymbolicExecution/Support/HashUtil.h"

#include <cassert>
#include <memory>

namespace SymbolicExecution {

class NumericalQuery;
class DirectNumericalQuery;
class IndirectNumericalQuery;

/// Owning handle to a bug query with structural equality and hashing.
class NumericalQueryPtr {
public:
  NumericalQueryPtr() = default;
  NumericalQueryPtr(const std::shared_ptr<NumericalQuery> &V) : Data(V) {}
  NumericalQueryPtr(const std::shared_ptr<DirectNumericalQuery> &V);
  NumericalQueryPtr(const std::shared_ptr<IndirectNumericalQuery> &V);
  NumericalQueryPtr(const NumericalQueryPtr &) = default;
  NumericalQueryPtr &operator=(const NumericalQueryPtr &) = default;

  bool operator==(const NumericalQueryPtr &R) const;
  size_t hash() const;

  const NumericalQuery *operator->() const { return get(); }
  operator bool() const { return Data != nullptr; }
  NumericalQuery *get() const { return Data.get(); }

  void translate(PathCondSolver *Solver) const;

private:
  std::shared_ptr<NumericalQuery> Data;
};

/// Abstract bug query discharged against symbolic state.
class NumericalQuery {
  friend class NumericalQueryPtr;

public:
  enum QueryKind { QK_DIRECT, QK_INDIRECT };

  NumericalQuery(QueryKind QK, SymexBugType BugTy,
                 const GuardedProgramValSet &Deps)
      : QK(QK), BugTy(BugTy), Deps(Deps) {}

  virtual ~NumericalQuery() = default;

  QueryKind getKind() const { return QK; }
  static bool classof(const NumericalQuery *) { return true; }

  virtual GuardedProgramValSet getUsedVals() const = 0;
  NumericalQueryPtr clone() const;
  virtual void dump() const {}
  virtual size_t hash() const = 0;

  const GuardedProgramValSet &getDeps() const { return Deps; }
  SymexBugType getBugTy() const { return BugTy; }

protected:
  QueryKind QK;
  SymexBugType BugTy = BUG_TY_UNDEF;
  mutable GuardedProgramValSet Deps;

  void dumpDeps() const;

private:
  void translate(PathCondSolver *Solver) const { Deps.translate(Solver); }
};

/// Direct numeric predicate query (either `Q == 0`, `Q > 0`, must-sat, or taint-only).
class DirectNumericalQuery : public NumericalQuery {
  DirectNumericalQuery(SymexBugType BugTy, const PropertyValuePtr &Qr, bool Eq,
                       const GuardedProgramValSet &Deps)
      : NumericalQuery(QK_DIRECT, BugTy, Deps), Q(Qr), Eq(Eq) {
    if (Q) {
      assert(IsaProperty<PropertySymExpr>(Q));
    }
  }

  explicit DirectNumericalQuery(SymexBugType BugTy)
      : NumericalQuery(QK_DIRECT, BugTy, {}) {}

  DirectNumericalQuery(SymexBugType BugTy, const GuardedProgramValSet &Deps)
      : NumericalQuery(QK_DIRECT, BugTy, Deps) {}

public:
  ~DirectNumericalQuery() override = default;

  static NumericalQueryPtr createMustErr(SymexBugType BugTy);
  static NumericalQueryPtr createTaintOnly(SymexBugType BugTy,
                                           const GuardedProgramValSet &Deps);
  static NumericalQueryPtr createSymbolic(SymexBugType BugTy,
                                          const PropertyValuePtr &Qr, bool Eq,
                                          const GuardedProgramValSet &Deps);

  bool operator==(const DirectNumericalQuery &R) const {
    return Q == R.Q && Eq == R.Eq;
  }

  size_t hash() const override {
    return hashCombine({static_cast<size_t>(QK), Q.hash(),
                        static_cast<size_t>(Eq)});
  }

  static bool classof(const NumericalQuery *V) {
    return V->getKind() == QK_DIRECT;
  }

  void dump() const override;
  SMTExprVec toSMT(PathCondSolver &Solver) const;

  GuardedProgramValSet getUsedVals() const override {
    GuardedProgramValSet Res = Deps;
    if (Q) {
      for (const auto &V : CastProperty<PropertySymExpr>(Q)->getUsedVars()) {
        Res.addValue(V.getValue());
      }
    }
    return Res;
  }

  PropertyValuePtr getSymExpr() const { return Q; }
  bool isEquation() const { return Eq; }
  bool mustSat() const { return !Q && getDeps().empty(); }

private:
  PropertyValuePtr Q;
  bool Eq = false;
};

/// Indirect memory-access query keeping base pointer, offset, and access size separate.
class IndirectNumericalQuery : public NumericalQuery {
  IndirectNumericalQuery(SymexBugType BugTy, const ProgramValuePtr &BasePtr,
                         const PropertyValuePtr &Offset,
                         const PropertyValuePtr &AccSize,
                         const GuardedProgramValSet &Deps)
      : NumericalQuery(QK_INDIRECT, BugTy, Deps), BasePtr(BasePtr),
        Offset(Offset), AccSize(AccSize) {}

public:
  ~IndirectNumericalQuery() override = default;

  static NumericalQueryPtr createBof(const ProgramValuePtr &BasePtr,
                                     const PropertyValuePtr &Offset,
                                     const PropertyValuePtr &AccSize,
                                     const GuardedProgramValSet &Deps);

  bool operator==(const IndirectNumericalQuery &R) const {
    return BasePtr == R.BasePtr && Offset == R.Offset && AccSize == R.AccSize;
  }

  size_t hash() const override {
    return hashCombine({static_cast<size_t>(QK), BasePtr.hash(), Offset.hash(),
                        AccSize.hash()});
  }

  static bool classof(const NumericalQuery *O) {
    return O->getKind() == QK_INDIRECT;
  }

  void dump() const override;

  ProgramValuePtr getBasePtr() const { return BasePtr; }
  const PropertyValuePtr &getOffset() const { return Offset; }
  const PropertyValuePtr &getAccSize() const { return AccSize; }

  GuardedProgramValSet getUsedVals() const override {
    GuardedProgramValSet Res = Deps;
    if (IsaProperty<PropertySymExpr>(Offset)) {
      for (const auto &P : CastProperty<PropertySymExpr>(Offset)->getUsedVars()) {
        Res.addValue(P.getValue());
      }
    }
    if (AccSize && IsaProperty<PropertySymExpr>(AccSize)) {
      for (const auto &P :
           CastProperty<PropertySymExpr>(AccSize)->getUsedVars()) {
        Res.addValue(P.getValue());
      }
    }
    return Res;
  }

private:
  ProgramValuePtr BasePtr;
  PropertyValuePtr Offset;
  PropertyValuePtr AccSize;
};

} // namespace SymbolicExecution

namespace std {
template <> struct hash<SymbolicExecution::NumericalQueryPtr> {
  size_t operator()(const SymbolicExecution::NumericalQueryPtr &V) const {
    return V.hash();
  }
};
} // namespace std

namespace SymbolicExecution {

class QuerySet : public GuardedSet<QuerySet, NumericalQueryPtr,
                                   &AnalysisLimit::INST_QUERY_LIMIT_V> {
public:
  QuerySet() = default;
  QuerySet(const NumericalQueryPtr &Q) { addValue(Q); }
};

} // namespace SymbolicExecution
