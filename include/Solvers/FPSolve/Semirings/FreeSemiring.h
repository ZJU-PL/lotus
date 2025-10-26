/**
 * Free Semiring - Symbolic expressions with no evaluation
 */

#ifndef FPSOLVE_FREE_SEMIRING_H
#define FPSOLVE_FREE_SEMIRING_H

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include "Solvers/FPSolve/DataStructs/FreeStructure.h"
#include "Solvers/FPSolve/DataStructs/Var.h"
#include <string>
#include <memory>

namespace fpsolve {

class FreeSemiring : public StarableSemiring<FreeSemiring, 
                                               Commutativity::NonCommutative, 
                                               Idempotence::NonIdempotent> {
private:
  NodePtr node_;
  static NodeFactory factory_;

  FreeSemiring(NodePtr n) : node_(n) {}

public:
  FreeSemiring() {
    node_ = factory_.GetEmpty();
  }

  FreeSemiring(const VarId var) {
    node_ = factory_.NewElement(var);
  }

  static FreeSemiring null() {
    return FreeSemiring{factory_.GetEmpty()};
  }

  static FreeSemiring one() {
    return FreeSemiring{factory_.GetEpsilon()};
  }

  FreeSemiring star() const {
    return FreeSemiring{factory_.NewStar(node_)};
  }

  FreeSemiring operator+(const FreeSemiring &x) {
    return FreeSemiring{factory_.NewAddition(node_, x.node_)};
  }

  FreeSemiring& operator+=(const FreeSemiring &x) {
    node_ = factory_.NewAddition(node_, x.node_);
    return *this;
  }

  FreeSemiring operator*(const FreeSemiring &x) {
    return FreeSemiring{factory_.NewMultiplication(node_, x.node_)};
  }

  FreeSemiring& operator*=(const FreeSemiring &x) {
    node_ = factory_.NewMultiplication(node_, x.node_);
    return *this;
  }

  bool operator==(const FreeSemiring &x) const {
    return node_ == x.node_;
  }

  std::string string() const {
    return NodeToString(*node_);
  }

  std::string RawString() const {
    return NodeToRawString(*node_);
  }

  template <typename SR>
  SR Eval(const ValuationMap<SR> &valuation) const;

  void PrintDot(std::ostream &out) {
    factory_.PrintDot(out);
  }

  void PrintStats(std::ostream &out = std::cout) {
    factory_.PrintStats(out);
  }

  void GC() {
    factory_.GC();
  }

  NodePtr GetNode() const { return node_; }

  friend struct std::hash<FreeSemiring>;
};

// Evaluator for converting FreeSemiring to concrete semiring
template <typename SR>
class FreeSemiringEvaluator : public NodeVisitor {
private:
  const ValuationMap<SR> &valuation_;
  SR result_;

public:
  FreeSemiringEvaluator(const ValuationMap<SR> &val) 
    : valuation_(val), result_(SR::null()) {}

  void Visit(const Addition &a) {
    FreeSemiringEvaluator left(valuation_);
    a.GetLhs()->Accept(left);
    FreeSemiringEvaluator right(valuation_);
    a.GetRhs()->Accept(right);
    result_ = left.result_ + right.result_;
  }

  void Visit(const Multiplication &m) {
    FreeSemiringEvaluator left(valuation_);
    m.GetLhs()->Accept(left);
    FreeSemiringEvaluator right(valuation_);
    m.GetRhs()->Accept(right);
    result_ = left.result_ * right.result_;
  }

  void Visit(const Star &s) {
    FreeSemiringEvaluator inner(valuation_);
    s.GetNode()->Accept(inner);
    result_ = inner.result_.star();
  }

  void Visit(const Element &e) {
    auto iter = valuation_.find(e.GetVar());
    if (iter != valuation_.end()) {
      result_ = iter->second;
    } else {
      result_ = SR::null();
    }
  }

  void Visit(const Epsilon &e) {
    result_ = SR::one();
  }

  void Visit(const Empty &e) {
    result_ = SR::null();
  }

  SR GetResult() const { return result_; }
};

template <typename SR>
SR FreeSemiring::Eval(const ValuationMap<SR> &valuation) const {
  FreeSemiringEvaluator<SR> evaluator(valuation);
  node_->Accept(evaluator);
  return evaluator.GetResult();
}

} // namespace fpsolve

namespace std {
template <>
struct hash<fpsolve::FreeSemiring> {
  std::size_t operator()(const fpsolve::FreeSemiring &fs) const {
    return std::hash<const void*>()(fs.GetNode());
  }
};
} // namespace std

#endif // FPSOLVE_FREE_SEMIRING_H

