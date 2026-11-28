#pragma once

#include "llvm/ADT/SmallVector.h"

#include "Solvers/EGraph/Id.h"
#include "Solvers/EGraph/Util.h"

namespace lotus::egraph {

class Var {
public:
  Var() = default;
  explicit Var(Symbol name) : name_(name) {}

  static Var fromU32(uint32_t num) {
    Var var;
    var.numeric_ = num;
    var.name_ = Symbol("?#" + std::to_string(num));
    return var;
  }

  static Var parse(std::string_view text) {
    if (text.size() < 2 || text[0] != '?') {
      throw std::runtime_error("Pattern variable must start with '?'");
    }
    if (text.size() >= 3 && text[1] == '#') {
      for (size_t i = 2; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
          throw std::runtime_error("Malformed numeric pattern variable");
        }
      }
      if (text.size() == 2) {
        throw std::runtime_error("Malformed numeric pattern variable");
      }
      return fromU32(
          static_cast<uint32_t>(std::stoul(std::string(text.substr(2)))));
    }
    return Var(Symbol(text));
  }

  const Symbol &name() const { return name_; }

  std::optional<uint32_t> asU32() const { return numeric_; }

  friend bool operator==(const Var &lhs, const Var &rhs) {
    return lhs.name_ == rhs.name_;
  }

  friend bool operator!=(const Var &lhs, const Var &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const Var &lhs, const Var &rhs) {
    return lhs.name_ < rhs.name_;
  }

private:
  Symbol name_;
  std::optional<uint32_t> numeric_;
};

inline std::ostream &operator<<(std::ostream &os, const Var &var) {
  os << var.name();
  return os;
}

class Subst {
public:
  static Subst withCapacity(size_t capacity) {
    Subst subst;
    subst.bindings_.reserve(capacity);
    return subst;
  }

  void emplaceUnique(const Var &var, Id id) {
    bindings_.emplace_back(var, id);
  }

  std::optional<Id> insert(const Var &var, Id id) {
    for (auto &[bound_var, bound_id] : bindings_) {
      if (bound_var == var) {
        Id old = bound_id;
        bound_id = id;
        return old;
      }
    }
    bindings_.emplace_back(var, id);
    return std::nullopt;
  }

  const Id *get(const Var &var) const noexcept {
    const uint32_t target = var.name().id();
    for (const auto &[bound_var, id] : bindings_) {
      if (bound_var.name().id() == target) {
        return &id;
      }
    }
    return nullptr;
  }

  Id at(const Var &var) const {
    if (const Id *id = get(var)) {
      return *id;
    }
    throw std::runtime_error("Substitution variable not found");
  }

  const Id &operator[](const Var &var) const {
    if (const Id *id = get(var)) {
      return *id;
    }
    throw std::runtime_error("Substitution variable not found");
  }

  const llvm::SmallVector<std::pair<Var, Id>, 4> &bindings() const {
    return bindings_;
  }

  friend bool operator==(const Subst &lhs, const Subst &rhs) {
    return lhs.bindings_ == rhs.bindings_;
  }

  friend bool operator!=(const Subst &lhs, const Subst &rhs) {
    return !(lhs == rhs);
  }

private:
  llvm::SmallVector<std::pair<Var, Id>, 4> bindings_;
};

} // namespace lotus::egraph

template <> struct std::hash<lotus::egraph::Var> {
  size_t operator()(const lotus::egraph::Var &var) const noexcept {
    return std::hash<lotus::egraph::Symbol>{}(var.name());
  }
};
