#pragma once

#include "Checker/TPA/Engine/EvalSuccessor.h"
#include "Checker/TPA/Support/TaintStore.h"

#include <vector>

namespace tpa::taint {

class EvalResult {
private:
  TaintStore store;

  using SuccessorList = std::vector<EvalSuccessor>;
  SuccessorList succs;

public:
  using const_iterator = typename SuccessorList::const_iterator;

  EvalResult() = default;

  EvalResult(const EvalResult &) = delete;
  EvalResult(EvalResult &&) noexcept = default;
  EvalResult &operator=(const EvalResult &) = delete;
  EvalResult &operator=(EvalResult &&) = delete;

  TaintStore &getStore() { return store; }
  const TaintStore &getStore() const { return store; }
  void setStore(const TaintStore &s) { store = s; }
  void setStore(TaintStore &&s) { store = std::move(s); }

  void addTopLevelSuccessor(const ProgramPoint &pp) {
    succs.push_back(EvalSuccessor(pp, nullptr));
  }

  void addMemLevelSuccessor(const ProgramPoint &pp,
                            const tpa::MemoryObject *obj) {
    succs.push_back(EvalSuccessor(pp, obj));
  }

  const_iterator begin() const { return succs.begin(); }
  const_iterator end() const { return succs.end(); }
  bool empty() const { return succs.empty(); }
  size_t size() const { return succs.size(); }
};

} // namespace tpa::taint
