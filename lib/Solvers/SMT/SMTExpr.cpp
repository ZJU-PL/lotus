/**
 * @file SMTExpr.cpp
 * @brief Implementation of the SMTExpr and SMTExprVec classes for SMT expressions
 * 
 * This file implements SMTExpr and SMTExprVec, which represent expressions in the SMT
 * solving system. It provides:
 * - Expression creation and manipulation
 * - Overloaded operators for easy constraint building
 * - Expression simplification using various techniques (local, contextual, dillig)
 * - Quantifier elimination and variable forgetting
 * - Interval abstraction for numeric expressions
 * - Basic operations for bitvector, boolean, array expressions
 * - Expression substitution
 * 
 * The SMTExpr class serves as the core representation for constraints that can be
 * added to an SMTSolver for satisfiability checking.
 */

#include "Solvers/SMT/SMTExpr.h"
#include "Solvers/SMT/SMTFactory.h"
//#include <algorithm>
#include <functional>
#include <set>
#include <utility>
#include <vector>

SMTExpr::SMTExpr(SMTFactory *F, z3::expr Z3Expr) : SMTObject(F), Expr(Z3Expr) {}

SMTExpr::SMTExpr(SMTExpr const &E) : SMTObject(E), Expr(E.Expr) {}

SMTExpr &SMTExpr::operator=(const SMTExpr &E) {
  SMTObject::operator=(E);
  if (this != &E) {
    this->Expr = E.Expr;
  }
  return *this;
}

SMTExpr SMTExpr::substitute(SMTExprVec &From, SMTExprVec &To) {
  assert(From.size() == To.size());
  if (From.empty()) {
    return *this;
  }
  return SMTExpr(&getSMTFactory(), Expr.substitute(*From.ExprVec, *To.ExprVec));
}

SMTExpr SMTExpr::localSimplify() {
  // TODO: customize parameters for `simplify()` method
  return SMTExpr(&getSMTFactory(), Expr.simplify());
}

// Slow
SMTExpr SMTExpr::ctxSimplify() {
  // apply contextual simplification rules
  z3::goal G(Expr.ctx());
  G.add(Expr);
  z3::tactic CS = z3::tactic(Expr.ctx(), "ctx-simplify");
  return SMTExpr(&getSMTFactory(), CS.apply(G)[0].as_expr());
}

SMTExpr SMTExpr::doConstantPropagation() {
  // TODO: figure out whether CP will eliminate variables.
  z3::goal G(Expr.ctx());
  G.add(Expr);
  z3::tactic CP = z3::tactic(Expr.ctx(), "propagate-values");
  return SMTExpr(&getSMTFactory(), CP.apply(G)[0].as_expr());
}

SMTExpr SMTExpr::elimValidOrUnsatExpr() {
  z3::context &Ctx = Expr.ctx();
  z3::params Param(Ctx);
  Param.set("timeout", (unsigned)1000);
  z3::solver S1(Ctx);
  S1.add(Expr);
  S1.set(Param);
  z3::solver S2(Ctx);
  S2.add(!Expr);
  S2.set(Param);
  if (S1.check() == z3::unsat) {
    return SMTExpr(&getSMTFactory(), Ctx.bool_val(false));
  } else if (S2.check() == z3::unsat) {
    return SMTExpr(&getSMTFactory(), Ctx.bool_val(true));
  } else {
    return *this;
  }
}

std::pair<bool, SMTExpr> SMTExpr::forgetVars(unsigned Timeout) {
  // TODO: design new qe procedures
  std::pair<bool, SMTExpr> Ret(false, getSMTFactory().createEmptySMTExpr());
  if (!Expr.is_quantifier()) {
    // If Expr is not a quantified formula, then do not apply QE.
  } else {
    z3::context &Ctx = Expr.ctx();
    z3::tactic QE = z3::try_for(z3::tactic(Ctx, "qe2"), Timeout);
    z3::goal G(Ctx);
    G.add(Expr);
    try {
      z3::apply_result Rt = QE.apply(G);
      Ret.first = true;
      Ret.second = SMTExpr(&getSMTFactory(), Rt[0].as_expr());
    } catch (z3::exception &Ex) {
      // timeout
    }
  }
  return Ret;
}

std::pair<bool, SMTExpr> SMTExpr::forgetVar(SMTExpr &Var, unsigned Timeout) {
  // Expr is not quantified, and we need to `forget' Var
  std::pair<bool, SMTExpr> Ret(false, getSMTFactory().createEmptySMTExpr());
  z3::context &Ctx = Expr.ctx();
  z3::tactic QE = z3::try_for(z3::tactic(Ctx, "qe2"), Timeout);
  z3::goal G(Ctx);
  G.add(z3::forall(Var.Expr, Expr));
  try {
    z3::apply_result Rt = QE.apply(G);
    Ret.first = true;
    Ret.second = SMTExpr(&getSMTFactory(), Rt[0].as_expr());
  } catch (z3::exception &Ex) {
    // timeout
  }
  return Ret;
}

std::pair<bool, SMTExpr> SMTExpr::forgetVars(SMTExprVec &Vars,
                                             unsigned Timeout) {
  // Expr is not quantified, and we need to `forget' Vars
  std::pair<bool, SMTExpr> Ret(false, getSMTFactory().createEmptySMTExpr());
  z3::context &Ctx = Expr.ctx();
  z3::tactic QE = z3::try_for(z3::tactic(Ctx, "qe2"), Timeout);
  z3::goal G(Ctx);
  //G.add(z3::exists(*Vars.ExprVec, Expr));
  try {
    z3::apply_result Rt = QE.apply(G);
    Ret.first = true;
    Ret.second = SMTExpr(&getSMTFactory(), Rt[0].as_expr());
  } catch (z3::exception &Ex) {
    // timeout
  }
  return Ret;
}

std::pair<int, int> SMTExpr::abstractInterval(SMTExpr &E, int Timeout) {
  // TODO: should we return an Expr or a domain value(like [a, b])
  z3::context &Ctx = Expr.ctx();
  std::pair<int, int> Ret(INT_MIN, INT_MAX);
  z3::params Param(Ctx);
  // Param.set("priority", Ctx.str_symbol("pareto"));
  Param.set("priority", Ctx.str_symbol("box"));
  z3::set_param("smt.timeout",
                Timeout); // p.set("timeout", Timeout); TODO: it seems we cannot
                          // set timeout directly to opt
  z3::optimize UpperFinder(Ctx);
  z3::optimize LowerFinder(Ctx);
  UpperFinder.set(Param);
  LowerFinder.set(Param);

  UpperFinder.add(Expr);
  z3::optimize::handle UpperGoal = UpperFinder.maximize(E.Expr);
  LowerFinder.add(Expr);
  z3::optimize::handle LowerGoal = LowerFinder.minimize(E.Expr);

  try {
    if (UpperFinder.check() == z3::sat) {
      // TODO: can we always get the numeric value?
      Ret.second = UpperFinder.lower(UpperGoal).get_numeral_int();
    }
  } catch (z3::exception &Ex) {
    // timeout
  }

  try {
    if (LowerFinder.check() == z3::sat) {
      Ret.first = LowerFinder.upper(LowerGoal).get_numeral_int();
    }
  } catch (z3::exception &Ex) {
    // timeout
  }

  return Ret;
}

void SMTExpr::abstractIntervalAsExprvec(SMTExpr &E, SMTExprVec &Evec,
                                        int Timeout) {
  z3::context &Ctx = Expr.ctx();
  // SMTExpr FalseVal =
  // Evec.push_back(SMTExpr(&getSMTFactory(), Ctx.bool_val(false)));
  // Evec.push_back(SMTExpr(&getSMTFactory(), Ctx.bool_val(false)));
  z3::params Param(Ctx);
  // Param.set("priority", Ctx.str_symbol("pareto"));
  Param.set("priority", Ctx.str_symbol("box"));
  z3::set_param("smt.timeout",
                Timeout); // p.set("timeout", Timeout); TODO: it seems we cannot
                          // set timeout directly to opt
  z3::optimize UpperFinder(Ctx);
  z3::optimize LowerFinder(Ctx);
  UpperFinder.set(Param);
  LowerFinder.set(Param);

  UpperFinder.add(Expr);
  z3::optimize::handle UpperGoal = UpperFinder.maximize(E.Expr);
  LowerFinder.add(Expr);
  z3::optimize::handle LowerGoal = LowerFinder.minimize(E.Expr);

  try {
    if (LowerFinder.check() == z3::sat) {
      std::cout << "Find lower success\n";
      Evec.push_back(SMTExpr(&getSMTFactory(), LowerFinder.upper(LowerGoal)));
    }
  } catch (z3::exception &Ex) {
    Evec.push_back(SMTExpr(&getSMTFactory(), Ctx.bool_val(false)));
  }

  try {
    if (UpperFinder.check() == z3::sat) {
      // TODO: can we always get the numeric value?
      std::cout << "Find upper success\n";
      Evec.push_back(SMTExpr(&getSMTFactory(), UpperFinder.lower(UpperGoal)));
    }
  } catch (z3::exception &Ex) {
    // timeout
    Evec.push_back(SMTExpr(&getSMTFactory(), Ctx.bool_val(false)));
  }
}

SMTExpr SMTExpr::dilligSimplify(SMTExpr N, z3::solver &Solver4Sim,
                                z3::context &Ctx) {
  if (!N.isLogicAnd() && !N.isLogicOr()) {
    // A leaf
    Solver4Sim.push();
    Solver4Sim.add(N.Expr);
    if (Solver4Sim.check() == z3::check_result::unsat) {
      Solver4Sim.pop();
      return SMTExpr(&getSMTFactory(), Ctx.bool_val(false));
    }
    Solver4Sim.pop();
    Solver4Sim.push();
    Solver4Sim.add(!N.Expr);
    if (Solver4Sim.check() == z3::check_result::unsat) {
      Solver4Sim.pop();
      return SMTExpr(&getSMTFactory(), Ctx.bool_val(true));
    }
    Solver4Sim.pop();
    return N;
  } else {
    // A connective (AND or OR)
    assert(N.isLogicAnd() || N.isLogicOr());

    std::vector<SMTExpr> C;
    std::set<SMTExpr, SMTExprComparator> CSet;
    for (unsigned I = 0, E = N.numArgs(); I < E; I++) {
      if (N.getArg(I).isTrue()) {
        if (N.isLogicAnd()) {
          continue;
        } else if (N.isLogicOr()) {
          return SMTExpr(&getSMTFactory(), Ctx.bool_val(true));
        }
      } else if (N.getArg(I).isFalse()) {
        if (N.isLogicAnd()) {
          return SMTExpr(&getSMTFactory(), Ctx.bool_val(false));
        } else if (N.isLogicOr()) {
          continue;
        }
      }

      if (!CSet.count(N.getArg(I))) {
        C.push_back(N.getArg(I));
        CSet.insert(N.getArg(I));
      }
    }

    bool FixedPoint = false;
    while (!FixedPoint) {
      // std::cout << "............." << (void*) (Z3_ast) N.z3_expr << "\n\n";
      FixedPoint = true;

      for (size_t I = 0, E = C.size(); I < E; ++I) {
        Z3_ast *Args = new Z3_ast[C.size() - 1];
        for (size_t J = 0; J < C.size() - 1; J++) {
          SMTExpr *Candidate = nullptr;
          if (J < I) {
            Candidate = &C[J];
          } else {
            assert(J + 1 < C.size());
            Candidate = &C[J + 1];
          }

          if (N.isLogicOr()) {
            Args[J] = !(*Candidate).Expr;
          } else {
            Args[J] = (*Candidate).Expr;
          }
        }
        SMTExpr Alpha(&getSMTFactory(),
                      to_expr(Ctx, Z3_mk_and(Ctx, C.size() - 1, Args)));
        delete[] Args;

        SMTExpr &Ci = C[I];

        Solver4Sim.push();
        Solver4Sim.add(Alpha.Expr);
        SMTExpr NewCi = dilligSimplify(Ci, Solver4Sim, Ctx);
        Solver4Sim.pop();

        if (!z3::eq(Ci.Expr, NewCi.Expr)) {
          if (FixedPoint)
            FixedPoint = false;
          C[I] = NewCi;
        }

        if (NewCi.isTrue() && N.isLogicOr()) {
          return SMTExpr(&getSMTFactory(), Ctx.bool_val(true));
        } else if (NewCi.isFalse() && N.isLogicAnd()) {
          return SMTExpr(&getSMTFactory(), Ctx.bool_val(false));
        }
      }

      // FIXME
      FixedPoint = true;
    }

    if (N.isLogicAnd()) {
      Z3_ast *Args = new Z3_ast[C.size()];
      size_t J = 0;
      for (size_t I = 0; I < C.size(); I++) {
        if (C[I].isTrue()) {
          continue;
        }
        Args[J++] = C[I].Expr;
      }

      if (J == 1) {
        SMTExpr Ret(&getSMTFactory(), to_expr(Ctx, Args[0]));
        delete[] Args;
        return Ret;
      }

      if (J == 0) {
        delete[] Args;
        return SMTExpr(&getSMTFactory(), Ctx.bool_val(true));
      }

      SMTExpr Ret(&getSMTFactory(), to_expr(Ctx, Z3_mk_and(Ctx, J, Args)));
      delete[] Args;

      return Ret;
    } else {
      // is logic OR
      Z3_ast *Args = new Z3_ast[C.size()];
      size_t J = 0;
      for (size_t I = 0; I < C.size(); I++) {
        if (C[I].isFalse()) {
          continue;
        }
        Args[J++] = C[I].Expr;
      }

      if (J == 1) {
        SMTExpr Ret(&getSMTFactory(), to_expr(Ctx, Args[0]));
        delete[] Args;
        return Ret;
      }

      if (J == 0) {
        delete[] Args;
        return SMTExpr(&getSMTFactory(), Ctx.bool_val(false));
      }

      SMTExpr Ret(&getSMTFactory(), to_expr(Ctx, Z3_mk_or(Ctx, J, Args)));
      delete[] Args;

      return Ret;
    }
  }
}

SMTExpr SMTExpr::dilligSimplify() {
  z3::context &Ctx = Expr.ctx();
  z3::solver Solver4Sim(Ctx);
  Solver4Sim.add(Ctx.bool_val(true));

  //	std::cout << "\nstart...." << this->size() << "\n";
  SMTExpr AftSim = dilligSimplify(*this, Solver4Sim, Ctx);
  //	std::cout << "end..." << AftSim.size() << "\n";

  return AftSim;
}

// TODO: add heavy-weight simplifications, such as:
// Quantifier elimination, some contextual simplifications such as
// dillig-Simpify, function minimization, etc.
SMTExpr SMTExpr::lightweightSimpifyAllInOne() {
  return this->localSimplify().doConstantPropagation().elimValidOrUnsatExpr();
}

bool SMTExpr::getExprVars(SMTExprVec &Vars) {
  // TODO: test the efficiency of this function
  // Should we return a vector or a set?
  try {
    auto &ctx = Expr.ctx();
    auto compare_func = [](const z3::expr &a, const z3::expr &b) {
      Z3_symbol sym_a = a.decl().name();
      Z3_symbol sym_b = b.decl().name();
      return sym_a < sym_b;
    };
    std::set<z3::expr, decltype(compare_func)> syms(compare_func);

    std::function<void(const z3::expr &)> recur = [&recur, &syms,
                                                   &ctx](const z3::expr &e) {
      assert(Z3_is_app(ctx, e));
      auto app = Z3_to_app(ctx, e);
      unsigned n_args = Z3_get_app_num_args(ctx, app);

      auto fdecl = Z3_get_app_decl(ctx, app);
      if (n_args == 0 && Z3_get_decl_kind(ctx, fdecl) == Z3_OP_UNINTERPRETED)
        syms.insert(e);

      for (unsigned i = 0; i < n_args; ++i) {
        z3::expr arg(ctx, Z3_get_app_arg(ctx, app, i));
        recur(arg);
      }
    };
    recur(Expr);

    for (auto &i : syms) {
      Vars.push_back(SMTExpr(&getSMTFactory(), i));
    }
  } catch (z3::exception &ex) {
    std::cout << ex.msg() << "\n";
    return false;
  }

  return true;
}

unsigned
SMTExpr::size(std::map<SMTExpr, unsigned, SMTExprComparator> &SizeCache) {
  if (!this->isLogicAnd() && !this->isLogicOr()) {
    if (isLogicNot()) {
      return this->getArg(0).size(SizeCache);
    } else {
      return 1;
    }
  } else {
    if (SizeCache.count(*this)) {
      return 0;
    }

    unsigned Sz = 0;
    for (unsigned I = 0, E = this->numArgs(); I < E; I++) {
      Sz += this->getArg(I).size(SizeCache);
    }
    SizeCache.insert(std::make_pair(*this, Sz));
    return Sz;
  }
}

SMTExpr SMTExpr::getQuantifierBody() const {
  return SMTExpr(&getSMTFactory(), Expr.body());
}

SMTExpr SMTExpr::getArg(unsigned I) const {
  return SMTExpr(&getSMTFactory(), Expr.arg(I));
}

SMTExpr SMTExpr::bv12bool() {
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_bv());
    unsigned bvSz = Expr.get_sort().array_range().bv_size();
    assert(bvSz == 1);
    auto func = (Expr.ctx().bv_val("0", 1) == Expr.ctx().bv_val("0", 1)).decl();

    z3::expr const_bv1 =
        const_array(Expr.get_sort().array_domain(), Expr.ctx().bv_val("1", 1));
    Z3_ast mapargs[2] = {Expr, const_bv1};

    return SMTExpr(
        &getSMTFactory(),
        z3::expr(Expr.ctx(), Z3_mk_map(Expr.ctx(), func, 2, mapargs)));
  } else {
    assert(Expr.is_bv() && Expr.get_sort().bv_size() == 1);
    return SMTExpr(&getSMTFactory(), Expr == Expr.ctx().bv_val(1, 1));
  }
}

SMTExpr SMTExpr::bool2bv1() {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_bool());
    auto func =
        ite(ctx.bool_val(false), ctx.bv_val(1, 1), ctx.bv_val(0, 1)).decl();

    z3::expr const_bv0 =
        const_array(Expr.get_sort().array_domain(), ctx.bv_val(0, 1));
    z3::expr const_bv1 =
        const_array(Expr.get_sort().array_domain(), ctx.bv_val(1, 1));

    Z3_ast mapargs[3] = {Expr, const_bv0, const_bv1};

    z3::expr bvret(ctx, Z3_mk_map(ctx, func, 3, mapargs));
    return SMTExpr(&getSMTFactory(), bvret);
  } else {
    assert(Expr.is_bool());
    return SMTExpr(&getSMTFactory(),
                   ite(Expr, ctx.bv_val(1, 1), ctx.bv_val(0, 1)));
  }
}

SMTExpr SMTExpr::real2int() {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_real());
    auto func = z3::expr(ctx, Z3_mk_real2int(ctx, ctx.real_val("0.0"))).decl();

    Z3_ast mapargs[1] = {Expr};
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 1, mapargs)));
  } else {
    assert(Expr.is_real());
    return SMTExpr(&getSMTFactory(), z3::expr(ctx, Z3_mk_real2int(ctx, Expr)));
  }
}

SMTExpr SMTExpr::int2real() {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_int());

    auto func = to_real(Expr).decl();
    Z3_ast mapargs[1] = {Expr};
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 1, mapargs)));
  } else {
    assert(Expr.is_int());
    return SMTExpr(&getSMTFactory(), to_real(Expr));
  }
}

SMTExpr SMTExpr::int2bv(unsigned sz) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_int());
    auto func = z3::expr(ctx, Z3_mk_int2bv(ctx, sz, ctx.int_val("0"))).decl();

    Z3_ast mapargs[1] = {Expr};
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 1, mapargs)));
  } else {
    assert(Expr.is_int());
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_int2bv(ctx, sz, Expr)));
  }
}

SMTExpr SMTExpr::bv2int(bool isSigned) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_bv());
    unsigned bvSz = Expr.get_sort().array_range().bv_size();
    auto func =
        z3::expr(ctx, Z3_mk_bv2int(ctx, ctx.bv_val("0", bvSz), isSigned))
            .decl();

    Z3_ast mapargs[1] = {Expr};
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 1, mapargs)));
  } else {
    assert(Expr.is_bv());
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_bv2int(ctx, Expr, isSigned)));
  }
}

#define BINARY_OPERATION(X)                                                    \
  SMTExpr SMTExpr::basic_##X(SMTExpr &b) {                                     \
    z3::context &ctx = Expr.ctx();                                             \
    if (Expr.is_array() && b.Expr.is_array()) {                                \
      Z3_ast const mapargs[2] = {Expr, b.Expr};                                \
      z3::func_decl func(ctx);                                                 \
      if (Expr.get_sort().array_range().is_bv()) {                             \
        unsigned bvSz = Expr.get_sort().array_range().bv_size();               \
        func = z3::expr(ctx, Z3_mk_bv##X(ctx, ctx.bv_val("0", bvSz),           \
                                         ctx.bv_val("0", bvSz)))               \
                   .decl();                                                    \
      } else {                                                                 \
        assert(false);                                                         \
      }                                                                        \
      return SMTExpr(&getSMTFactory(),                                         \
                     z3::expr(ctx, Z3_mk_map(ctx, func, 2, mapargs)));         \
    } else {                                                                   \
      assert(isBitVector() && b.isBitVector());                                \
      return SMTExpr(&getSMTFactory(),                                         \
                     z3::expr(ctx, Z3_mk_bv##X(ctx, Expr, b.Expr)));           \
    }                                                                          \
  }

BINARY_OPERATION(add)
BINARY_OPERATION(sub)
BINARY_OPERATION(mul)
BINARY_OPERATION(udiv)
BINARY_OPERATION(sdiv)
BINARY_OPERATION(urem)
BINARY_OPERATION(srem)
BINARY_OPERATION(shl)
BINARY_OPERATION(ashr)
BINARY_OPERATION(lshr)
BINARY_OPERATION(and)
BINARY_OPERATION(or)
BINARY_OPERATION(xor)

BINARY_OPERATION(ugt)
BINARY_OPERATION(uge)
BINARY_OPERATION(sgt)
BINARY_OPERATION(sge)
BINARY_OPERATION(ult)
BINARY_OPERATION(ule)
BINARY_OPERATION(sle)
BINARY_OPERATION(slt)

SMTExpr SMTExpr::basic_concat(SMTExpr &b) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array() && b.Expr.is_array()) {
    Z3_ast const mapargs[2] = {Expr, b.Expr};
    z3::func_decl func(ctx);
    if (Expr.get_sort().array_range().is_bv()) {
      unsigned bvSz = Expr.get_sort().array_range().bv_size();
      func = z3::expr(ctx, Z3_mk_concat(ctx, ctx.bv_val("0", bvSz),
                                        ctx.bv_val("0", bvSz)))
                 .decl();
    } else {
      assert(false);
    }
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 2, mapargs)));
  } else {
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_concat(ctx, Expr, b.Expr)));
  }
}

SMTExpr SMTExpr::basic_eq(SMTExpr &b) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array() && b.Expr.is_array()) {
    Z3_ast const mapargs[2] = {Expr, b.Expr};
    z3::func_decl func(ctx);
    if (Expr.get_sort().array_range().is_bv()) {
      unsigned bvSz = Expr.get_sort().array_range().bv_size();
      func = (ctx.bv_val("0", bvSz) == ctx.bv_val("0", bvSz)).decl();
    } else {
      func = (ctx.real_val("0.0") == ctx.real_val("0.0")).decl();
    }
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 2, mapargs)));
  } else {
    return *this == b;
  }
}

SMTExpr SMTExpr::basic_ne(SMTExpr &b) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array() && b.Expr.is_array()) {
    Z3_ast const mapargs[2] = {Expr, b.Expr};
    z3::func_decl func(ctx);
    if (Expr.get_sort().array_range().is_bv()) {
      unsigned bvSz = Expr.get_sort().array_range().bv_size();
      func = (ctx.bv_val("0", bvSz) != ctx.bv_val("0", bvSz)).decl();
    } else {
      func = (ctx.real_val("0.0") != ctx.real_val("0.0")).decl();
    }
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 2, mapargs)));
  } else {
    return *this != b;
  }
}

SMTExpr SMTExpr::basic_extract(unsigned high, unsigned low) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_bv());
    unsigned bvSz = Expr.get_sort().array_range().bv_size();
    auto func =
        z3::expr(ctx, Z3_mk_extract(ctx, high, low, ctx.bv_val("10", bvSz)))
            .decl();
    Z3_ast mapargs[1] = {Expr};
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 1, mapargs)));
  } else {
    assert(Expr.is_bv());
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_extract(ctx, high, low, Expr)));
  }
}

SMTExpr SMTExpr::basic_sext(unsigned sz) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_bv());
    unsigned bvSz = Expr.get_sort().array_range().bv_size();
    auto func =
        z3::expr(ctx, Z3_mk_sign_ext(ctx, sz, ctx.bv_val("10", bvSz))).decl();
    Z3_ast mapargs[1] = {Expr};
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 1, mapargs)));
  } else {
    assert(Expr.is_bv());
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_sign_ext(ctx, sz, Expr)));
  }
}

SMTExpr SMTExpr::basic_zext(unsigned sz) {
  z3::context &ctx = Expr.ctx();
  if (Expr.is_array()) {
    assert(Expr.get_sort().array_range().is_bv());
    unsigned bvSz = Expr.get_sort().array_range().bv_size();
    auto func =
        z3::expr(ctx, Z3_mk_zero_ext(ctx, sz, ctx.bv_val("10", bvSz))).decl();
    Z3_ast mapargs[1] = {Expr};
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 1, mapargs)));
  } else {
    assert(Expr.is_bv());
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_zero_ext(ctx, sz, Expr)));
  }
}

SMTExpr SMTExpr::basic_ite(SMTExpr &TBValue, SMTExpr &FBValue) {
  z3::context &ctx = Expr.ctx();
  z3::expr &condition = Expr;
  if (condition.is_array()) {
    assert(TBValue.Expr.is_array() && FBValue.Expr.is_array());

    Z3_ast mapargs[3] = {condition, TBValue.Expr, FBValue.Expr};
    z3::func_decl func(ctx);
    if (TBValue.Expr.get_sort().array_range().is_bv()) {
      assert(FBValue.Expr.get_sort().array_range().is_bv());
      unsigned bvSz = TBValue.Expr.get_sort().array_range().bv_size();
      func = ite(ctx.bool_val(true), ctx.bv_val(1, bvSz), ctx.bv_val(0, bvSz))
                 .decl();
    } else {
      assert(TBValue.Expr.get_sort().array_range().is_real());
      assert(FBValue.Expr.get_sort().array_range().is_real());
      func = ite(ctx.bool_val(true), ctx.real_val("0.0"), ctx.real_val("0.0"))
                 .decl();
    }

    z3::expr mapped(ctx, Z3_mk_map(ctx, func, 3, mapargs));
    return SMTExpr(&getSMTFactory(),
                   z3::expr(ctx, Z3_mk_map(ctx, func, 3, mapargs)));
  } else {
    return SMTExpr(&getSMTFactory(),
                   ite(condition, TBValue.Expr, FBValue.Expr));
  }
}

SMTExpr SMTExpr::array_elmt(unsigned ElmtNum, unsigned Index) {
  assert(ElmtNum > Index);
  unsigned TotalSz = getBitVecSize();
  unsigned EachSz = TotalSz / ElmtNum;
  unsigned High = TotalSz - Index * EachSz - 1;
  unsigned Low = TotalSz - (Index + 1) * EachSz;
  return basic_extract(High, Low);
}

#define ARRAY_CMP_OPERATION(X)                                                 \
  SMTExpr SMTExpr::array_##X(SMTExpr &BvAsArray, unsigned ElmtNum) {           \
    assert(ElmtNum > 0);                                                       \
    if (ElmtNum == 1) {                                                        \
      return basic_##X(BvAsArray).bool2bv1();                                  \
    } else {                                                                   \
      SMTExpr Op2 = BvAsArray.array_elmt(ElmtNum, 0);                          \
      SMTExpr Ret = array_elmt(ElmtNum, 0).basic_##X(Op2).bool2bv1();          \
      for (unsigned I = 1; I < ElmtNum; I++) {                                 \
        Op2 = BvAsArray.array_elmt(ElmtNum, I);                                \
        SMTExpr NextRet = array_elmt(ElmtNum, I).basic_##X(Op2).bool2bv1();    \
        Ret = Ret.basic_concat(NextRet);                                       \
      }                                                                        \
      return Ret;                                                              \
    }                                                                          \
  }

ARRAY_CMP_OPERATION(sgt)
ARRAY_CMP_OPERATION(sge)
ARRAY_CMP_OPERATION(ugt)
ARRAY_CMP_OPERATION(uge)
ARRAY_CMP_OPERATION(slt)
ARRAY_CMP_OPERATION(sle)
ARRAY_CMP_OPERATION(ule)
ARRAY_CMP_OPERATION(ult)
ARRAY_CMP_OPERATION(eq)
ARRAY_CMP_OPERATION(ne)

#define ARRAY_BIN_OPERATION(X)                                                 \
  SMTExpr SMTExpr::array_##X(SMTExpr &BvAsArray, unsigned ElmtNum) {           \
    assert(ElmtNum > 0);                                                       \
    if (ElmtNum == 1) {                                                        \
      return basic_##X(BvAsArray);                                             \
    } else {                                                                   \
      SMTExpr Op2 = BvAsArray.array_elmt(ElmtNum, 0);                          \
      SMTExpr Ret = array_elmt(ElmtNum, 0).basic_##X(Op2);                     \
      for (unsigned I = 1; I < ElmtNum; I++) {                                 \
        Op2 = BvAsArray.array_elmt(ElmtNum, I);                                \
        SMTExpr Next = array_elmt(ElmtNum, I).basic_##X(Op2);                  \
        Ret = Ret.basic_concat(Next);                                          \
      }                                                                        \
      return Ret;                                                              \
    }                                                                          \
  }

ARRAY_BIN_OPERATION(add)
ARRAY_BIN_OPERATION(sub)
ARRAY_BIN_OPERATION(mul)
ARRAY_BIN_OPERATION(udiv)
ARRAY_BIN_OPERATION(sdiv)
ARRAY_BIN_OPERATION(urem)
ARRAY_BIN_OPERATION(srem)
ARRAY_BIN_OPERATION(shl)
ARRAY_BIN_OPERATION(ashr)
ARRAY_BIN_OPERATION(lshr)
ARRAY_BIN_OPERATION(and)
ARRAY_BIN_OPERATION(or)
ARRAY_BIN_OPERATION(xor)

SMTExpr SMTExpr::array_ite(SMTExpr &TBValue, SMTExpr &FBValue,
                           unsigned ElmtNum) {
  assert(ElmtNum > 0);
  if (ElmtNum == 1) {
    return this->bv12bool().basic_ite(TBValue, FBValue);
  } else {
    SMTExpr T = TBValue.array_elmt(ElmtNum, 0);
    SMTExpr F = FBValue.array_elmt(ElmtNum, 0);
    SMTExpr Ret = array_elmt(ElmtNum, 0).bv12bool().basic_ite(T, F);

    for (unsigned I = 1; I < ElmtNum; I++) {
      T = TBValue.array_elmt(ElmtNum, I);
      F = FBValue.array_elmt(ElmtNum, I);
      SMTExpr Next = array_elmt(ElmtNum, I).bv12bool().basic_ite(T, F);
      Ret = Ret.basic_concat(Next);
    }
    return Ret;
  }
}

#define ARRAY_EXT_OPERATION(X)                                                 \
  SMTExpr SMTExpr::array_##X(unsigned Sz, unsigned ElmtNum) {                  \
    assert(ElmtNum > 0);                                                       \
    if (ElmtNum == 1) {                                                        \
      return basic_##X(Sz);                                                    \
    } else {                                                                   \
      unsigned ElmtExtSz = Sz / ElmtNum;                                       \
      SMTExpr Ret = array_elmt(ElmtNum, 0).basic_##X(ElmtExtSz);               \
      for (unsigned I = 1; I < ElmtNum; I++) {                                 \
        SMTExpr Next = array_elmt(ElmtNum, I).basic_##X(ElmtExtSz);            \
        Ret = Ret.basic_concat(Next);                                          \
      }                                                                        \
      return Ret;                                                              \
    }                                                                          \
  }

ARRAY_EXT_OPERATION(zext)
ARRAY_EXT_OPERATION(sext)

SMTExpr SMTExpr::array_trunc(unsigned Sz, unsigned ElmtNum) {
  assert(ElmtNum > 0);
  if (ElmtNum == 1) {
    return basic_extract(getBitVecSize() - Sz - 1, 0);
  } else {
    unsigned ElmtSz = getBitVecSize() / ElmtNum;
    unsigned ElmtTruncSz = Sz / ElmtNum;

    assert(ElmtSz > ElmtTruncSz);

    unsigned High = ElmtSz - ElmtTruncSz - 1;
    SMTExpr Ret = array_elmt(ElmtNum, 0).basic_extract(High, 0);
    for (unsigned I = 1; I < ElmtNum; I++) {
      SMTExpr Next = array_elmt(ElmtNum, I).basic_extract(High, 0);
      Ret = Ret.basic_concat(Next);
    }
    return Ret;
  }
}

SMTExpr operator!(SMTExpr const &A) {
  if (A.isLogicNot()) {
    assert(A.numArgs() == 1);
    return A.getArg(0);
  } else if (A.isTrue()) {
    return A.getSMTFactory().createBoolVal(false);
  } else if (A.isFalse()) {
    return A.getSMTFactory().createBoolVal(true);
  } else {
    return SMTExpr(&A.getSMTFactory(), !A.Expr);
  }
}

SMTExpr operator||(SMTExpr const &A, SMTExpr const &B) {
  if (A.isFalse() || B.isTrue()) {
    return B;
  } else if (A.isTrue() || B.isFalse()) {
    return A;
  }

  return SMTExpr(&A.getSMTFactory(), A.Expr || B.Expr);
}

SMTExpr operator||(SMTExpr const &A, bool B) {
  return A || A.getSMTFactory().createBoolVal(B);
}

SMTExpr operator||(bool A, SMTExpr const &B) { return B || A; }

SMTExpr operator&&(SMTExpr const &A, SMTExpr const &B) {
  if (A.isTrue() || B.isFalse()) {
    return B;
  } else if (B.isTrue() || A.isFalse()) {
    return A;
  }

  return SMTExpr(&A.getSMTFactory(), A.Expr && B.Expr);
}

SMTExpr operator&&(SMTExpr const &A, bool B) {
  return A && A.getSMTFactory().createBoolVal(B);
}

SMTExpr operator&&(bool A, SMTExpr const &B) { return B && A; }

#define UNARY_OPERATION_EXPR(X)                                                \
  SMTExpr operator X(SMTExpr const &A) {                                       \
    return SMTExpr(&A.getSMTFactory(), X(A.Expr));                             \
  }

UNARY_OPERATION_EXPR(-)
UNARY_OPERATION_EXPR(~)

#define BINARY_OPERATION_EXPR_EXPR(X)                                          \
  SMTExpr operator X(SMTExpr const &A, SMTExpr const &B) {                     \
    assert(A.isSameSort(B));                                                   \
    return SMTExpr(&A.getSMTFactory(), A.Expr X B.Expr);                       \
  }

BINARY_OPERATION_EXPR_EXPR(|)
BINARY_OPERATION_EXPR_EXPR(^)
BINARY_OPERATION_EXPR_EXPR(&)
BINARY_OPERATION_EXPR_EXPR(>)
BINARY_OPERATION_EXPR_EXPR(<)
BINARY_OPERATION_EXPR_EXPR(>=)
BINARY_OPERATION_EXPR_EXPR(<=)
BINARY_OPERATION_EXPR_EXPR(!=)
BINARY_OPERATION_EXPR_EXPR(==)
BINARY_OPERATION_EXPR_EXPR(+)
BINARY_OPERATION_EXPR_EXPR(-)
BINARY_OPERATION_EXPR_EXPR(*)
BINARY_OPERATION_EXPR_EXPR(/)

#define BINARY_OPERATION_EXPR_INT(X)                                           \
  SMTExpr operator X(SMTExpr const &A, int B) {                                \
    return SMTExpr(&A.getSMTFactory(), A.Expr X B);                            \
  }

BINARY_OPERATION_EXPR_INT(|)
BINARY_OPERATION_EXPR_INT(^)
BINARY_OPERATION_EXPR_INT(&)
BINARY_OPERATION_EXPR_INT(>)
BINARY_OPERATION_EXPR_INT(<)
BINARY_OPERATION_EXPR_INT(>=)
BINARY_OPERATION_EXPR_INT(<=)
BINARY_OPERATION_EXPR_INT(!=)
BINARY_OPERATION_EXPR_INT(==)
BINARY_OPERATION_EXPR_INT(+)
BINARY_OPERATION_EXPR_INT(-)
BINARY_OPERATION_EXPR_INT(*)
BINARY_OPERATION_EXPR_INT(/)

#define BINARY_OPERATION_INT_EXPR(X)                                           \
  SMTExpr operator X(int A, SMTExpr const &B) {                                \
    return SMTExpr(&B.getSMTFactory(), A X B.Expr);                            \
  }

BINARY_OPERATION_INT_EXPR(|)
BINARY_OPERATION_INT_EXPR(^)
BINARY_OPERATION_INT_EXPR(&)
BINARY_OPERATION_INT_EXPR(>)
BINARY_OPERATION_INT_EXPR(<)
BINARY_OPERATION_INT_EXPR(>=)
BINARY_OPERATION_INT_EXPR(<=)
BINARY_OPERATION_INT_EXPR(!=)
BINARY_OPERATION_INT_EXPR(==)
BINARY_OPERATION_INT_EXPR(+)
BINARY_OPERATION_INT_EXPR(-)
BINARY_OPERATION_INT_EXPR(*)
BINARY_OPERATION_INT_EXPR(/)

std::ostream &operator<<(std::ostream &Out, SMTExpr const &N) {
  Out << N.Expr;
  return Out;
}

bool SMTExpr::SMTExprToStream(std::string &ExprStr) {
  try {
    z3::solver Sol(Expr.ctx());
    Sol.add(Expr);
    ExprStr = Sol.to_smt2();
    // The following function does not work. We cannot recover expr
    // from the generated string because it looses type info.
    // ExprStr = Z3_ast_to_string(Expr.ctx(), Expr);
    return true;
  } catch (z3::exception &Ex) {
    return false;
  }
}

bool SMTExpr::SMTExprToStream(std::ostream &Out) {
  try {
    z3::solver Sol(Expr.ctx());
    Sol.add(Expr);
    // TODO: in fact we don't need call to_smt2() here.
    Out << Sol.to_smt2();
    return true;
  } catch (z3::exception &Ex) {
    return false;
  }
}

bool SMTExpr::isLogicalEquivTo(SMTExpr const &E) {
  // TODO: set timeout for each query

  z3::context &Ctx = Expr.ctx();
  // First, very simple cases, e.g., e1 = 2x + 3y, e2 = 2x + 3y
  // but if e1 = 2x + 3y, e2 = 3y + 2x, their hash will not be the same.
  if (Expr.hash() == E.Expr.hash())
    return true;

  // Second, e1, e2 not predicate.
  z3::expr bvZero = Ctx.bv_val(0, 32); // TODO: identify the size first..
  if (!Expr.is_bool() && !E.Expr.is_bool()) {
    // simplify() can "calculate", and to_string is fast for simple expr
    // TODO: implement to_string() for z3::expr
    // if ((Expr - E.Expr).simplify().to_string() == bvZero.to_string()) return
    // true;
    z3::solver S(Ctx);
    S.add(Expr - E.Expr != 0);
    if (S.check() == z3::unsat) {
      return true;
    } else {
      return false;
    }
  } else if (Expr.is_bool() && E.Expr.is_bool()) {
    // Finally, e1, e2 predicates
    z3::solver S1(Ctx);
    S1.add(!z3::implies(Expr, E.Expr));
    z3::solver S2(Ctx);
    S2.add(!z3::implies(E.Expr, Expr));
    if (S1.check() == z3::unsat && S2.check() == z3::unsat) {
      return true;
    } else {
      return false;
    }
  } else {
    // Else, e1 and e2 are not comparable
    return false;
  }
}

/*==-- SMTExprVec --==*/

SMTExprVec::SMTExprVec(SMTFactory *F, std::shared_ptr<z3::expr_vector> Vec)
    : SMTObject(F), ExprVec(Vec) {}

SMTExprVec::SMTExprVec(const SMTExprVec &Vec)
    : SMTObject(Vec), ExprVec(Vec.ExprVec) {}

SMTExprVec &SMTExprVec::operator=(const SMTExprVec &Vec) {
  SMTObject::operator=(Vec);
  if (this != &Vec) {
    this->ExprVec = Vec.ExprVec;
  }
  return *this;
}

unsigned SMTExprVec::size() const {
  if (ExprVec.get() == nullptr) {
    return 0;
  }
  return ExprVec->size();
}

void SMTExprVec::push_back(SMTExpr E, bool Enforce) {
  if (E.isTrue() && !Enforce) {
    return;
  }
  if (ExprVec.get() == nullptr) {
    ExprVec = std::make_shared<z3::expr_vector>(E.Expr.ctx());
  }
  ExprVec->push_back(E.Expr);
}

SMTExpr SMTExprVec::operator[](unsigned I) const {
  assert(I < size());
  return SMTExpr(&getSMTFactory(), (*ExprVec)[I]);
}

bool SMTExprVec::empty() const { return size() == 0; }

SMTExprVec SMTExprVec::copy() {
  if (size() == 0) {
    // std::shared_ptr<z3::expr_vector> Ret(nullptr);
    assert(!ExprVec.get());
    return *this;
  }

  std::shared_ptr<z3::expr_vector> Ret =
      std::make_shared<z3::expr_vector>(ExprVec->ctx());
  Ret->resize(ExprVec->size());
  for (unsigned Idx = 0; Idx < ExprVec->size(); Idx++) {
    Z3_ast_vector_set(ExprVec->ctx(), *Ret, Idx, (*ExprVec)[Idx]);
  }
  return SMTExprVec(&getSMTFactory(), Ret);
}

/// *this = *this && v2
void SMTExprVec::mergeWithAnd(const SMTExprVec &Vec) {
  if (Vec.size() == 0) {
    return;
  }

  if (ExprVec.get() == nullptr) {
    ExprVec = std::make_shared<z3::expr_vector>(Vec.ExprVec->ctx());
  }

  for (size_t I = 0; I < Vec.size(); I++) {
    ExprVec->push_back((*Vec.ExprVec)[I]);
  }
}

SMTExprVec SMTExprVec::merge(SMTExprVec Vec1, SMTExprVec Vec2) {
  if (Vec1.size() < Vec2.size()) {
    Vec2.mergeWithAnd(Vec1);
    return Vec2;
  } else {
    Vec1.mergeWithAnd(Vec2);
    return Vec1;
  }
}

/// *this = *this || v2
void SMTExprVec::mergeWithOr(const SMTExprVec &Vec) {
  SMTExprVec Ret = getSMTFactory().createEmptySMTExprVec();
  if (Vec.empty() || empty()) {
    Ret.push_back(getSMTFactory().createBoolVal(true));
    *this = Ret;
    return;
  }

  SMTExpr E1 = this->toAndExpr();
  SMTExpr E2 = Vec.toAndExpr();
  Ret.push_back(E1 || E2);
  *this = Ret;
  return;
}

SMTExpr SMTExprVec::toAndExpr() const {
  if (empty()) {
    return getSMTFactory().createBoolVal(true);
  }

  z3::expr t = ExprVec->ctx().bool_val(true),
           f = ExprVec->ctx().bool_val(false);

  Z3_ast *Args = new Z3_ast[ExprVec->size()];
  unsigned ActualSz = 0, Index = 0;
  for (unsigned I = 0, E = ExprVec->size(); I < E; I++) {
    z3::expr e = (*ExprVec)[I];
    if (z3::eq(e, t)) {
      continue;
    } else if (z3::eq(e, f)) {
      delete[] Args;
      return getSMTFactory().createBoolVal(false);
    }
    Args[ActualSz++] = (*ExprVec)[I];
    Index = I;
  }

  if (ActualSz == 1) {
    delete[] Args;
    return SMTExpr(&getSMTFactory(), (*ExprVec)[Index]);
  }

  SMTExpr Ret(
      &getSMTFactory(),
      to_expr(ExprVec->ctx(), Z3_mk_and(ExprVec->ctx(), ActualSz, Args)));
  delete[] Args;

  return Ret;
}

SMTExpr SMTExprVec::toOrExpr() const {
  if (empty()) {
    return getSMTFactory().createBoolVal(true);
  }

  z3::expr t = ExprVec->ctx().bool_val(true),
           f = ExprVec->ctx().bool_val(false);

  Z3_ast *Args = new Z3_ast[ExprVec->size()];
  unsigned ActualSz = 0, Index = 0;
  for (unsigned I = 0, E = ExprVec->size(); I < E; I++) {
    z3::expr e = (*ExprVec)[I];
    if (z3::eq(e, f)) {
      continue;
    } else if (z3::eq(e, t)) {
      delete[] Args;
      return SMTExpr(&getSMTFactory(), t);
    }
    Args[ActualSz++] = (*ExprVec)[I];
    Index = I;
  }

  if (ActualSz == 1) {
    delete[] Args;
    return SMTExpr(&getSMTFactory(), (*ExprVec)[Index]);
  }

  SMTExpr Ret(
      &getSMTFactory(),
      to_expr(ExprVec->ctx(), Z3_mk_or(ExprVec->ctx(), ActualSz, Args)));
  delete[] Args;
  return Ret;
}

unsigned SMTExprVec::constraintSize() const {
  unsigned Ret = 0;
  std::map<SMTExpr, unsigned, SMTExprComparator> Cache;
  for (unsigned I = 0; I < this->size(); I++) {
    Ret += (*this)[I].size(Cache);
  }
  return Ret;
}

SMTExprVec SMTExprVec::setDifference(const SMTExprVec &Vars) {
  SMTExprVec Ret = Vars.getSMTFactory().createEmptySMTExprVec();
  try {
    for (unsigned I = 0; I < (*this).size(); I++) {
      bool isInDiff = true;
      Z3_symbol SymI = (*this)[I].getZ3Symbol();
      for (unsigned J = 0; J < Vars.size(); J++) {
        if (SymI == Vars[J].getZ3Symbol()) {
          isInDiff = false;
        }
      }
      if (isInDiff) {
        Ret.push_back((*this)[I]);
      }
    }
  } catch (z3::exception &Ex) {
    std::cout << Ex.msg() << "\n";
    return Ret;
  }
  return Ret;
}

std::ostream &operator<<(std::ostream &Out, SMTExprVec& Vec) {
  if (Vec.empty()) {
    Out << "(empty vector)";
    return Out;
  }
  Vec.SMTExprVecToStream(Out);
  return Out;
}

bool SMTExprVec::SMTExprVecToStream(std::string &ExprStr) {
  try {
    SMTExpr SingleExpr = this->toAndExpr();
    z3::solver Sol(SingleExpr.Expr.ctx());
    Sol.add(SingleExpr.Expr);
    ExprStr = Sol.to_smt2();
    return true;
  } catch (z3::exception &Ex) {
    return false;
  }
}

bool SMTExprVec::SMTExprVecToStream(std::ostream &Out) {
  try {
    SMTExpr SingleExpr = this->toAndExpr();
    z3::solver Sol(SingleExpr.Expr.ctx());
    Sol.add(SingleExpr.Expr);
    // TODO: in fact we don't need call to_smt2() here.
    Out << Sol.to_smt2();
    return true;
  } catch (z3::exception &Ex) {
    return false;
  }
}
