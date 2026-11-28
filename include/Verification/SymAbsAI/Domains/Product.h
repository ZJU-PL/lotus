/**
 * @file Product.h
 * @brief Cartesian product abstract domain — composes multiple domains.
 *
 * Allows combining independent abstract domains into a product domain where
 * each sub-domain tracks a different facet of program state. Supports
 * randomized projection for scalability.
 */
#pragma once

#include "Verification/SymAbsAI/Core/Foundation/AbstractValue.h"
#include "Verification/SymAbsAI/Core/Integration/FunctionContext.h"
#include "Verification/SymAbsAI/Core/Integration/ResultStore.h"
#include "Verification/SymAbsAI/Utils/Config.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

#include <algorithm>
#include <random>
#include <typeinfo>

#include <llvm/IR/CFG.h>

namespace symabs_ai {
namespace domains {
/** @brief Cartesian product of multiple abstract domains. */
class Product : public AbstractValue {
protected:
  using values_t = std::vector<unique_ptr<AbstractValue>>;
  values_t Values_;
  const FunctionContext &FunctionContext_;
  bool Finalized_;
  bool EmptyIsBottom_ = true;
  std::mt19937 RNG_;
  unsigned int KeepPercent_;

protected:
  virtual void prettyPrint(PrettyPrinter &out) const override {
    if (out.compactProducts()) {
      if (isTop()) {
        out << pp::top;
        return;
      }

      if (isBottom()) {
        out << pp::bottom;
        return;
      }
    }

    for (auto &x : Values_) {
      if (out.compactProducts() && x->isTop())
        continue;

      PrettyPrinter::Entry block(&out, "AbstractValue");
      x->prettyPrint(out);
    }
  }

public:
  Product(const FunctionContext &fctx)
      : FunctionContext_(fctx), Finalized_(false) {
    auto cfg = FunctionContext_.getConfig();
    KeepPercent_ =
        cfg.get<int>("Product", "AbstractConsequenceKeepPercent", 100);

    int seed = (cfg.get<int>("Product", "RandomSeed", 0));
    if (seed)
      RNG_ = std::mt19937(seed);
  }

  const values_t &getValues() const {
    assert(Finalized_);
    return Values_;
  }

  values_t::value_type &add(values_t::value_type &&val) {
    assert(!Finalized_);
    Values_.emplace_back(std::move(val));
    return Values_.back();
  }

  template <typename T> T *add(T *val) {
    assert(!Finalized_);
    auto *base_ptr = add(values_t::value_type(val)).get();
    return static_cast<T *>(base_ptr);
  }

  void finalize() {
    assert(!Finalized_);
    Finalized_ = true;
  }

  virtual bool joinWith(const AbstractValue &av_other) override {
    assert(Finalized_);
    const auto *other = dynamic_cast<const Product *>(&av_other);
    if (!other)
      return false;
    if (Values_.empty() && other->Values_.empty()) {
      if (EmptyIsBottom_ && !other->EmptyIsBottom_) {
        EmptyIsBottom_ = false;
        return true;
      }
      return false;
    }
    if (Values_.empty()) {
      if (EmptyIsBottom_ && !other->isBottom()) {
        EmptyIsBottom_ = false;
        return true;
      }
      return false;
    }
    if (other->Values_.empty()) {
      if (!other->isBottom()) {
        bool wasTop = isTop();
        havoc();
        return !wasTop;
      }
      return false;
    }
    // Allow different sizes (e.g. parameterized domains with location-dependent
    // param sets).
    const size_t n = std::min(Values_.size(), other->Values_.size());
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
      if (Values_[i]->joinWith(*other->Values_[i]))
        changed = true;
    }
    return changed;
  }

  virtual bool meetWith(const AbstractValue &av_other) override {
    assert(Finalized_);
    const auto *other = dynamic_cast<const Product *>(&av_other);
    if (!other)
      return false;
    if (Values_.empty()) {
      if (!EmptyIsBottom_ && other->isBottom()) {
        EmptyIsBottom_ = true;
        return true;
      }
      return false;
    }
    if (other->Values_.empty()) {
      if (other->isBottom() && !isBottom()) {
        resetToBottom();
        return true;
      }
      return false;
    }
    const size_t n = std::min(Values_.size(), other->Values_.size());
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
      if (Values_[i]->meetWith(*other->Values_[i]))
        changed = true;
    }
    return changed;
  }

  virtual void havoc() override {
    assert(Finalized_);
    EmptyIsBottom_ = false;
    for (unsigned i = 0; i < Values_.size(); i++) {
      Values_[i]->havoc();
    }
  }

  virtual void widen() override {
    assert(Finalized_);
    for (auto &x : Values_)
      x->widen();
  }

  virtual AbstractValue *clone() const override {
    assert(Finalized_);
    Product *result = new Product(FunctionContext_);
    result->EmptyIsBottom_ = EmptyIsBottom_;
    for (auto &value : Values_) {
      result->add(unique_ptr<AbstractValue>(value->clone()));
    }
    result->finalize();
    return result;
  }

  virtual bool updateWith(const ConcreteState &cstate) override {
    assert(Finalized_);
    if (Values_.empty()) {
      if (EmptyIsBottom_) {
        EmptyIsBottom_ = false;
        return true;
      }
      return false;
    }
    bool changed = false;

    for (unsigned i = 0; i < Values_.size(); i++) {
      if (Values_[i]->updateWith(cstate))
        changed = true;
    }

    return changed;
  }

  virtual z3::expr toFormula(const ValueMapping &vmap,
                             z3::context &zctx) const override {
    assert(Finalized_);
    if (Values_.empty())
      return zctx.bool_val(!EmptyIsBottom_);
    z3::expr result = zctx.bool_val(true);

    for (auto &x : Values_)
      result = result && x->toFormula(vmap, zctx);

    return result;
  }

  virtual bool isTop() const override {
    if (Values_.empty())
      return !EmptyIsBottom_;
    for (auto &x : Values_) {
      if (!x->isTop())
        return false;
    }
    return true;
  }

  virtual bool isBottom() const override {
    if (Values_.empty())
      return EmptyIsBottom_;
    for (auto &x : Values_) {
      if (!x->isBottom())
        return false;
    }
    return true;
  }

  virtual void gatherFlattenedSubcomponents(
      std::vector<const AbstractValue *> *result) const override {
    for (auto &val : Values_) {
      val->gatherFlattenedSubcomponents(result);
    }
  }

  virtual void abstractConsequence(const AbstractValue &av_other) override {
    using std::pair;
    using std::vector;

    assert(Finalized_);
    assert(isJoinableWith(av_other));

    // recursively flatten contained abstract values
    vector<const AbstractValue *> all_this, all_other;
    gatherFlattenedSubcomponents(&all_this);
    av_other.gatherFlattenedSubcomponents(&all_other);
    const size_t npairs = std::min(all_this.size(), all_other.size());

    // prepare and shuffle a list of corresponding pairs of values
    vector<pair<AbstractValue *, const AbstractValue *>> av_pairs;
    for (size_t i = 0; i < npairs; ++i) {
      // all_this belong to this abstract value so they're safe to modify
      AbstractValue *ati = const_cast<AbstractValue *>(all_this[i]);
      av_pairs.push_back({ati, all_other[i]});
    }
    if (av_pairs.empty())
      return;
    std::shuffle(av_pairs.begin(), av_pairs.end(), RNG_);

    int to_keep = std::max<int>(1, (KeepPercent_ * Values_.size()) / 100);

    // Recursively keep abstract consequences for at most to_keep components and
    // havoc the rest. This implements the generic conjunctive-domain strategy:
    // most leaf domains can rely on their no-op AbstractValue fallback while
    // Product still presents a smaller formula to the bilateral solver.
    for (auto &current : av_pairs) {
      if ((*(current.second) <= *(current.first)) || !(to_keep > 0)) {
        // we're not keeping this one
        current.first->havoc();
      } else {
        current.first->abstractConsequence(*current.second);
        to_keep--;
      }
    }
  }

  virtual void resetToBottom() override {
    EmptyIsBottom_ = true;
    for (auto &val : Values_) {
      val->resetToBottom();
    }
  }

  virtual bool isJoinableWith(const AbstractValue &other) const override {
    if (const auto *oth_val = dynamic_cast<const Product *>(&other)) {
      if (Values_.empty() && oth_val->Values_.empty())
        return true;
      const size_t n = std::min(Values_.size(), oth_val->Values_.size());
      for (size_t i = 0; i < n; ++i) {
        if (!(oth_val->Values_[i])->isJoinableWith(*Values_[i]))
          return false;
      }
      return n > 0;
    }
    return false;
  }

  /**
   * An efficient representation of a reduced product of two domains.
   *
   * For two abstract values (assumed to be bottom), creates a bottom
   * abstract value of a domain that is at least as expressive as a
   * product of domains of `a` and `b`. If the arguments are products
   * themselves it flattens them and creates a `Product` object while
   * avoiding unnecessary repetition of values from the same domain.
   *
   * The result is not guaranteed to be a `Product`.
   */
  static unique_ptr<AbstractValue> Combine(const FunctionContext &fctx,
                                           const AbstractValue &a,
                                           const AbstractValue &b) {
    assert(a.isBottom() && b.isBottom());
    std::vector<const AbstractValue *> vals;
    a.gatherFlattenedSubcomponents(&vals);
    b.gatherFlattenedSubcomponents(&vals);

    if (vals.size() == 1)
      return unique_ptr<AbstractValue>(vals[0]->clone());

    // remove (set to nullptr) redundant abstract values
    for (unsigned i = 0; i < vals.size(); i++) {
      for (unsigned j = i + 1; j < vals.size(); j++) {
        if (vals[i] && vals[j] && vals[i]->isJoinableWith(*vals[j])) {
          vals[j] = nullptr;
        }
      }
    }

    auto *prod = new Product(fctx);
    for (auto *val : vals) {
      if (val != nullptr)
        prod->add(unique_ptr<AbstractValue>(val->clone()));
    }
    prod->finalize();

    return unique_ptr<AbstractValue>(prod);
  }

#ifdef ENABLE_DYNAMIC
  template <class Archive> void save(Archive &archive) const {
    assert(Values_.size() <= (size_t)UINT32_MAX);
    uint32_t size = Values_.size();
    archive(size);

    for (auto &component : Values_)
      archive(component);
  }

  template <class Archive> void load(Archive &archive) {}

  template <class Archive>
  static void load_and_construct(Archive &archive,
                                 cereal::construct<Product> &construct) {
    uint32_t size;
    archive(size);

    auto &fctx = cereal::get_user_data<FunctionContext>(archive);
    construct(fctx);

    for (uint32_t i = 0; i < size; i++) {
      unique_ptr<AbstractValue> component;
      archive(component);
      construct->add(std::move(component));
    }

    construct->finalize();
  }
#endif
};
} // namespace domains
} // namespace symabs_ai

#ifdef ENABLE_DYNAMIC
CEREAL_REGISTER_TYPE(symabs_ai::domains::Product);
#endif
