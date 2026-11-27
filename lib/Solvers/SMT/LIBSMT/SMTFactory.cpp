/**
 * @file SMTFactory.cpp
 * @brief Implementation of the SMTFactory class that creates and manages SMT
 * expressions
 *
 * This file implements the SMTFactory class, which serves as the primary
 * factory for creating and managing SMT expressions, solvers, and related
 * objects. It provides:
 * - Creation of various types of SMT expressions (boolean, bitvector, real,
 * array, etc.)
 * - Translation of expressions between different Z3 contexts
 * - Renaming of variables with suffixes for context separation
 * - Expression substitution and manipulation
 *
 * The factory pattern ensures all SMT objects share a common context and
 * can interact safely. All SMT objects created by this factory become
 * invalid when the factory is destroyed.
 */

#include "Solvers/SMT/LIBSMT/SMTFactory.h"

#include "Solvers/SMT/LIBSMT/SMTConfigure.h"

#define DEBUG_TYPE "smt-fctry"

SMTFactory::SMTFactory() : TempSMTVaraibleIndex(0) {}

SMTExprVec SMTFactory::translate(const SMTExprVec &Exprs) {
  if (Exprs.empty()) {
    return this->createEmptySMTExprVec();
  }

  std::lock_guard<std::mutex> L(Exprs.getSMTFactory().getFactoryLock());

  std::shared_ptr<z3::expr_vector> Vec(new z3::expr_vector(
      z3::expr_vector(Ctx, Z3_ast_vector_translate(Exprs.ExprVec->ctx(),
                                                   *Exprs.ExprVec, Ctx))));
  SMTExprVec Ret(this, Vec);
  return Ret;
}

SMTExpr SMTFactory::translate(const SMTExpr &Expr) {
  std::lock_guard<std::mutex> L(Expr.getSMTFactory().getFactoryLock());

  // Z3_translate requires distinct source and target contexts. Summary
  // composition can pass an expression already owned by this factory.
  if (&Expr.getSMTFactory() == this)
    return Expr;

  if (Expr.isTrue()) {
    return this->createBoolVal(true);
  } else if (Expr.isFalse()) {
    return this->createBoolVal(false);
  }

  z3::expr RetExpr =
      z3::expr(Ctx, Z3_translate(Expr.Expr.ctx(), Expr.Expr, Ctx));
  SMTExpr Ret(this, RetExpr);
  return Ret;
}

std::pair<SMTExprVec, bool>
SMTFactory::rename(const SMTExprVec &Exprs, const std::string &RenamingSuffix,
                   std::unordered_map<std::string, SMTExpr> &Mapping,
                   SMTRenamingAdvisor *Advisor) {

  SMTExprVec RetExprVec = this->createEmptySMTExprVec();
  bool RetBool = false; // the constraint is pruned?

  for (unsigned ExprIdx = 0; ExprIdx < Exprs.size(); ExprIdx++) {
    SMTExpr Ret = Exprs[ExprIdx];

    if (Ret.isApp() && Ret.isTrue()) {
      RetExprVec.push_back(Ret);
      continue;
    }

    const std::unordered_map<std::string, SMTExpr> *SymbolMapPtr = nullptr;
    std::unordered_map<std::string, SMTExpr> LocalMapping;

    RenamingCacheKey CacheKey{static_cast<Z3_ast>(Ret.getZ3Expr()), Advisor};
    auto It = ExprRenamingCache.find(CacheKey);
    if (It != ExprRenamingCache.end()) {
      auto &Cache = It->second;
      if (Cache.WillBePruned) {
        RetBool = true;
        Ret = Cache.AfterBeingPruned;

        if (Ret.isApp() && Ret.isTrue()) {
          continue;
        }
      }
      SymbolMapPtr = &Cache.SymbolMapping;
    } else {
      SMTExprVec ToPrune = this->createEmptySMTExprVec();
      std::unordered_map<Z3_ast, bool> Visited;

      bool AllPruned = visit(Ret, LocalMapping, ToPrune, Visited, Advisor);
      if (AllPruned) {
        RenamingUtility RU{Ret, true, createBoolVal(true),
                           createEmptySMTExprVec(), std::move(LocalMapping)};
        ExprRenamingCache.emplace(CacheKey, std::move(RU));

        RetBool = true;
        continue;
      } else if (ToPrune.size()) {
        SMTExprVec TrueVec = this->createBoolSMTExprVec(true, ToPrune.size());
        assert(ToPrune.size() == TrueVec.size());
        SMTExpr AfterSubstitution = Ret.substitute(ToPrune, TrueVec);
        RenamingUtility RU{Ret, true, AfterSubstitution, ToPrune,
                           std::move(LocalMapping)};
        auto Inserted = ExprRenamingCache.emplace(CacheKey, std::move(RU));
        SymbolMapPtr = &Inserted.first->second.SymbolMapping;

        RetBool = true;
        Ret = AfterSubstitution;
      } else {
        RenamingUtility RU{Ret, false, createBoolVal(true),
                           createEmptySMTExprVec(), std::move(LocalMapping)};
        auto Inserted = ExprRenamingCache.emplace(CacheKey, std::move(RU));
        SymbolMapPtr = &Inserted.first->second.SymbolMapping;
      }
    }

    // renaming
    assert(RenamingSuffix.find(' ') == std::string::npos);
    if (!RenamingSuffix.empty() && SymbolMapPtr && !SymbolMapPtr->empty()) {
      // Utility for replacement
      SMTExprVec From = createEmptySMTExprVec(), To = createEmptySMTExprVec();

      // Mapping.string + suffix --> new string + Mapping.expr.sort -> new expr
      for (const auto &Entry : *SymbolMapPtr) {
        const std::string &OldSymbol = Entry.first;
        const SMTExpr &OldExpr = Entry.second;

        if (!Advisor || Advisor->rename(OldExpr)) {
          std::string NewSymbol = OldSymbol + RenamingSuffix;
          SMTExpr NewExpr(this,
                          z3::expr(Ctx, Z3_mk_const(Ctx,
                                                    Z3_mk_string_symbol(
                                                        Ctx, NewSymbol.c_str()),
                                                    OldExpr.Expr.get_sort())));
          Mapping.emplace(OldSymbol, NewExpr);

          From.push_back(OldExpr);
          To.push_back(NewExpr);
        } else {
          Mapping.emplace(OldSymbol, OldExpr);
        }
      }

      if (From.size()) {
        assert(From.size() == To.size());
        Ret = Ret.substitute(From, To);
      }
    }

    RetExprVec.push_back(Ret);
  }

  return std::make_pair(RetExprVec, RetBool);
}

bool SMTFactory::visit(SMTExpr &Expr2Visit,
                       std::unordered_map<std::string, SMTExpr> &Mapping,
                       SMTExprVec &ToPrune,
                       std::unordered_map<Z3_ast, bool> &Visited,
                       SMTRenamingAdvisor *Advisor) {
  assert(Expr2Visit.isApp() && "Must be an app-only constraints.");

  Z3_ast Ast = static_cast<Z3_ast>(Expr2Visit.getZ3Expr());
  auto VisIt = Visited.find(Ast);
  if (VisIt != Visited.end()) {
    return VisIt->second;
  }

  if (!ExprRenamingCache.empty()) {
    auto It = ExprRenamingCache.find(RenamingCacheKey{Ast, Advisor});
    if (It != ExprRenamingCache.end()) {
      auto &Cache = It->second;
      Mapping.insert(Cache.SymbolMapping.begin(), Cache.SymbolMapping.end());

      if (Cache.WillBePruned) {
        if (Cache.AfterBeingPruned.isApp() && Cache.AfterBeingPruned.isTrue()) {
          Visited.emplace(Ast, true);
          return true;
        } else {
          ToPrune.mergeWithAnd(Cache.ToPrune);
          Visited.emplace(Ast, false);
          return false;
        }
      } else {
        Visited.emplace(Ast, false);
        return false;
      }
    }
  }

  unsigned NumArgs = Expr2Visit.numArgs();
  if (NumArgs == 0) {
    if (Expr2Visit.isNumeral() || Expr2Visit.isTrue() || Expr2Visit.isFalse()) {
      Visited.emplace(Ast, false);
      return false;
    }
    if (Expr2Visit.isConst()) {
      if (Advisor && Advisor->prune(Expr2Visit)) {
        Visited.emplace(Ast, true);
        return true;
      }
      std::string Symbol = Expr2Visit.getSymbol();
      assert(Symbol != "true" && Symbol != "false");
      Mapping.emplace(std::move(Symbol), Expr2Visit);
    }
    Visited.emplace(Ast, false);
    return false;
  }

  const bool IsAnd = Expr2Visit.isLogicAnd();
  std::vector<bool> Arg2Prune;
  if (IsAnd)
    Arg2Prune.reserve(NumArgs);
  bool All2Prune = true;
  bool One2Prune = false;
  for (unsigned I = 0; I < NumArgs; I++) {
    SMTExpr Arg = Expr2Visit.getArg(I);
    bool WillPrune = visit(Arg, Mapping, ToPrune, Visited, Advisor);
    if (IsAnd)
      Arg2Prune.push_back(WillPrune);
    if (!WillPrune && All2Prune) {
      All2Prune = false;
    } else if (WillPrune && !One2Prune) {
      One2Prune = true;
    }
  }

  if (IsAnd) {
    if (All2Prune) {
      Visited.emplace(Ast, true);
      return true;
    }
    for (unsigned I = 0; I < NumArgs; I++) {
      if (Arg2Prune[I]) {
        ToPrune.push_back(Expr2Visit.getArg(I));
      }
    }
  } else if (One2Prune) {
    Visited.emplace(Ast, true);
    return true;
  }

  Visited.emplace(Ast, false);
  return false;
}

SMTSolver SMTFactory::createSMTSolver() {
  z3::solver Ret(Ctx);
  z3::model Z3Model(Ctx);
  return SMTSolver(this, Ret, Z3Model);
}

SMTSolver SMTFactory::createSimpleSMTSolver() {
  z3::solver Ret(Ctx, Z3_mk_simple_solver(Ctx));
  z3::model Z3Model(Ctx);
  return SMTSolver(this, Ret, Z3Model);
}

SMTSolver SMTFactory::createSMTSolverForLogic(const std::string &Logic) {
  z3::solver Ret(
      Ctx, Z3_mk_solver_for_logic(Ctx, Z3_mk_string_symbol(Ctx, Logic.c_str())));
  z3::model Z3Model(Ctx);
  return SMTSolver(this, Ret, Z3Model);
}

SMTSolver SMTFactory::createSMTSolverWithTactic(const std::string &Tactic) {
  if (Tactic.empty()) {
    z3::solver Ret(Ctx);
    z3::model Z3Model(Ctx);
    return SMTSolver(this, Ret, Z3Model);
  } else {
    z3::solver Ret = z3::tactic(Ctx, Tactic.c_str()).mk_solver();
    z3::model Z3Model(Ctx);
    return SMTSolver(this, Ret, Z3Model);
  }
}

SMTExprVec SMTFactory::createBoolSMTExprVec(bool Content, size_t Size) {
  SMTExprVec Ret = createEmptySMTExprVec();
  for (unsigned J = 0; J < Size; J++) {
    Ret.push_back(createBoolVal(Content), true);
  }
  return Ret;
}

SMTExprVec SMTFactory::createSMTExprVec(const std::vector<SMTExpr> &ExprVec) {
  SMTExprVec Ret = createEmptySMTExprVec();
  for (size_t J = 0; J < ExprVec.size(); J++) {
    const SMTExpr &Ex = ExprVec[J];
    assert(&Ret.getSMTFactory() == &Ex.getSMTFactory() &&
           "Contexts are not compatible!");
    Ret.push_back(Ex);
  }
  return Ret;
}

SMTExpr SMTFactory::createSMTExprFromStream(std::string &ExprStr) {
  // TODO: the created SMTExpr looses type info of variables.
  //
  return SMTExpr(this, z3::mk_and(Ctx.parse_string(ExprStr.c_str())));
}

SMTExpr SMTFactory::parseSMTLib2File(const std::string &FileName) {
  // Z3_ast_vector AstV = Z3_parse_smtlib2_file(Ctx, FileName.c_str(), 0, 0, 0,
  // 0, 0, 0); z3::expr Whole(Ctx, Ast);
  z3::expr Whole = z3::mk_and(Ctx.parse_file(FileName.c_str()));
  return SMTExpr(this, Whole);
}

SMTExpr SMTFactory::parseSMTLib2String(const std::string &Raw) {
  // Z3_ast Ast = Z3_parse_smtlib2_string(Ctx, Raw.c_str(), 0, 0, 0, 0, 0, 0);
  // z3::expr Whole(Ctx, Ast);
  z3::expr Whole = z3::mk_and(Ctx.parse_string(Raw.c_str()));
  return SMTExpr(this, Whole);
}

SMTExpr SMTFactory::createEmptySMTExpr() {
  return SMTExpr(this, z3::expr(Ctx));
}

SMTExprVec SMTFactory::createEmptySMTExprVec() {
  std::shared_ptr<z3::expr_vector> Vec(nullptr);
  return SMTExprVec(this, Vec);
}

SMTExpr SMTFactory::createRealConst(const std::string &Name) {
  return SMTExpr(this, Ctx.real_const(Name.c_str()));
}

SMTExpr SMTFactory::createRealVal(const std::string &ValStr) {
  return SMTExpr(this, Ctx.real_val(ValStr.c_str()));
}

SMTExpr SMTFactory::createBitVecConst(const std::string &Name, uint64_t Sz) {
  return SMTExpr(this, Ctx.bv_const(Name.c_str(), Sz));
}

SMTExpr SMTFactory::createTemporaryBitVecConst(uint64_t Sz) {
  std::string Symbol("temp_");
  Symbol.append(std::to_string(TempSMTVaraibleIndex++));
  return SMTExpr(this, Ctx.bv_const(Symbol.c_str(), Sz));
}

SMTExpr SMTFactory::createBitVecVal(const std::string &ValStr, uint64_t Sz) {
  return SMTExpr(this, Ctx.bv_val(ValStr.c_str(), Sz));
}

SMTExpr SMTFactory::createBoolConst(const std::string &Name) {
  return SMTExpr(this, Ctx.bool_const(Name.c_str()));
}

SMTExpr SMTFactory::createBitVecVal(uint64_t Val, uint64_t Sz) {
  return SMTExpr(this, Ctx.bv_val((__uint64_t)Val, Sz));
}

SMTExpr SMTFactory::createIntVal(int Val) {
  return SMTExpr(this, Ctx.int_val(Val));
}

SMTExpr SMTFactory::createSelect(SMTExpr &Vec, SMTExpr Index) {
  return SMTExpr(this, z3::expr(Ctx, Z3_mk_select(Ctx, Vec.Expr, Index.Expr)));
}

SMTExpr SMTFactory::createStore(SMTExpr &Vec, SMTExpr Index,
                                SMTExpr &Val2Store) {
  return SMTExpr(this, z3::expr(Ctx, Z3_mk_store(Ctx, Vec.Expr, Index.Expr,
                                                 Val2Store.Expr)));
}

SMTExpr
SMTFactory::createIntRealArrayConstFromStringSymbol(const std::string &Name) {
  Z3_sort ArraySort = Ctx.array_sort(Ctx.int_sort(), Ctx.real_sort());
  return SMTExpr(
      this,
      z3::expr(Ctx, Z3_mk_const(Ctx, Z3_mk_string_symbol(Ctx, Name.c_str()),
                                ArraySort)));
}

SMTExpr
SMTFactory::createIntBvArrayConstFromStringSymbol(const std::string &Name,
                                                  uint64_t Sz) {
  Z3_sort ArraySort = Ctx.array_sort(Ctx.int_sort(), Ctx.bv_sort(Sz));
  return SMTExpr(
      this,
      z3::expr(Ctx, Z3_mk_const(Ctx, Z3_mk_string_symbol(Ctx, Name.c_str()),
                                ArraySort)));
}

SMTExpr SMTFactory::createIntDomainConstantArray(SMTExpr &ElmtExpr) {
  return SMTExpr(this, z3::expr(Ctx, Z3_mk_const_array(Ctx, Ctx.int_sort(),
                                                       ElmtExpr.Expr)));
}

SMTExpr SMTFactory::createBoolVal(bool B) {
  return SMTExpr(this, Ctx.bool_val(B));
}
