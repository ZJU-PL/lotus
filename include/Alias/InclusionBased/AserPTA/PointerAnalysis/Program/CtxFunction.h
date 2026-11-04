//
// Created by peiming on 10/30/19.
//
#pragma once

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Program/CallSite.h"

#include <llvm/Support/CommandLine.h>

extern llvm::cl::opt<size_t> MaxIndirectTarget;

namespace aser {

template <typename ctx> class CallGraphNode;
template <typename ctx> class CtxModule;

// wrapper for llvm::Function, but it has context attached to it
template <typename ctx> class CtxFunction {
private:
  const ctx *context;
  bool isExt = false;
  const llvm::Function *function;
  CallGraphNode<ctx> *const callNode; // the corresponding call node

  // FIXME: remove callSite, there is a set of callsite for a function because
  // the context might be merged!!!
  const llvm::Instruction *callSite;

public:
  CtxFunction(const ctx *C, const llvm::Function *F, const llvm::Instruction *I,
              CallGraphNode<ctx> *N)
      : context(C), function(F), callNode(N), callSite(I) {}
  CtxFunction(const CtxFunction<ctx> &&cf) noexcept
      : context(cf.context), function(cf.function), callNode(cf.callNode) {}

  CtxFunction(const CtxFunction<ctx> &) = delete;
  CtxFunction<ctx> &operator=(const CtxFunction<ctx> &) = delete;

  __attribute__((warn_unused_result)) inline const ctx *getContext() const { return this->context; }

  inline void markAsExtFunction() { this->isExt = true; }

  __attribute__((warn_unused_result)) inline const llvm::Function *getFunction() const {
    return this->function;
  }

  __attribute__((deprecated(
      "The Callsite returned could be wrong"))) inline const llvm::Instruction *
  getCallSite() const {
    return this->callSite;
  }

  __attribute__((warn_unused_result)) inline CallGraphNode<ctx> *getCallNode() const {
    return this->callNode;
  }
  __attribute__((warn_unused_result)) inline bool isExtFunction() const { return this->isExt; }

  __attribute__((warn_unused_result)) inline auto begin() const -> decltype(function->begin()) {
    return function->begin();
  }

  __attribute__((warn_unused_result)) inline auto end() const -> decltype(function->end()) {
    return function->end();
  }

  __attribute__((warn_unused_result)) inline auto arg_begin() const
      -> decltype(function->arg_begin()) {
    return function->arg_begin();
  }

  __attribute__((warn_unused_result)) inline auto arg_end() const -> decltype(function->arg_end()) {
    return function->arg_end();
  }

  __attribute__((warn_unused_result)) inline llvm::StringRef getName() const {
    return function->getName();
  }

  friend CtxModule<ctx>;
};

// a indirect function call invoked in ctx
template <typename ctx> class InDirectCallSite {
private:
  const ctx *context;
  const aser::CallSite callSite;
  const llvm::Value *const funPtr;

  std::set<const llvm::Function *> targets; // possible targets
  std::set<const CallGraphNode<ctx> *>
      resolvedNode; // the correponding call node

  CallGraphNode<ctx> *const callNode; // the corresponding indirect call node

public:
  InDirectCallSite(const ctx *C, const llvm::Instruction *CS,
                   const llvm::Value *V, CallGraphNode<ctx> *N)
      : context(C), callSite(CS), funPtr(V), callNode(N) {
    assert(callSite.isCallOrInvoke());
  }

  InDirectCallSite(const ctx *C, const llvm::Instruction *CS,
                   CallGraphNode<ctx> *N)
      : context(C), callSite(CS), funPtr(nullptr), callNode(N) {
    assert(callSite.isCallOrInvoke());
  }

  InDirectCallSite(const InDirectCallSite<ctx> &&cf) noexcept
      : context(cf.context), callSite(cf.callSite), funPtr(cf.funPtr),
        callNode(cf.callNode) {}

  InDirectCallSite(const InDirectCallSite<ctx> &) = delete;
  InDirectCallSite<ctx> &operator=(const InDirectCallSite<ctx> &) = delete;

  __attribute__((warn_unused_result)) inline CallGraphNode<ctx> *getCallNode() const {
    return this->callNode;
  }
  __attribute__((warn_unused_result)) inline bool isInterceptedCallSite() const {
    return this->funPtr != nullptr; // the callsite is changed by language model
  }

  __attribute__((warn_unused_result)) inline const ctx *getContext() const { return this->context; }

  __attribute__((warn_unused_result)) inline const llvm::Value *getValue() const {
    if (funPtr != nullptr) {
      // overriden by language model.
      return funPtr;
    }
    return callSite.getCalledValue();
  }

  __attribute__((warn_unused_result)) inline const llvm::Type *getCalledType() const {
    return this->getValue()->getType();
  }

  __attribute__((warn_unused_result)) inline const llvm::Instruction *getCallSite() const {
    return this->callSite.getInstruction();
  }

  __attribute__((warn_unused_result)) inline bool resolvedTo(const llvm::Function *fun) {
    if (this->targets.size() >= MaxIndirectTarget) {
      return false;
    }
    return this->targets.insert(fun).second;
  }

  inline void resolvedToNode(const CallGraphNode<ctx> *node) {
    auto result = this->resolvedNode.insert(node).second;
    assert(result);
  }

  __attribute__((warn_unused_result))
  inline const std::set<const CallGraphNode<ctx> *> &getResolvedNode() const {
    return this->resolvedNode;
  }

  __attribute__((warn_unused_result)) inline const std::set<const llvm::Function *> &
  getResolvedTarget() const {
    return this->targets;
  }

  friend CtxModule<ctx>;
};

} // namespace aser
