#include "Concurrency/Thread/Sharing/EscapeAnalysis.h"
#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/Core/ConcurrencyContext.h"

#include <deque>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus {

namespace {

const Function *resolveInternalCallee(const CallBase *call) {
  if (!call) {
    return nullptr;
  }
  if (Function *direct = call->getCalledFunction()) {
    return direct;
  }
  return dyn_cast<Function>(call->getCalledOperand()->stripPointerCasts());
}

} // namespace

EscapeAnalysis::EscapeAnalysis(Module &module) : m_module(module) {}

void EscapeAnalysis::analyze() {
  m_escaped_values.clear();
  runEscapeAnalysis();
}

bool EscapeAnalysis::isEscaped(const Value *val) const {
  if (!val)
    return false;
  
  if (isa<GlobalValue>(val))
    return true;

  const Value *stripped = val->stripPointerCasts();
  if (m_escaped_values.count(stripped)) {
    return true;
  }

  const Value *obj = getUnderlyingObject(stripped);
  if (obj && obj != stripped && m_escaped_values.count(obj->stripPointerCasts())) {
    return true;
  }

  return false;
}

bool EscapeAnalysis::isThreadLocal(const Value *val) const {
  return !isEscaped(val);
}

void EscapeAnalysis::runEscapeAnalysis() {
  std::deque<const Value *> worklist;
  auto enqueueEscaped = [&](const Value *value) {
    if (!value) return;
    const Value *stripped = value->stripPointerCasts();
    if (m_escaped_values.insert(stripped).second) {
      worklist.push_back(stripped);
    }
    const Value *obj = getUnderlyingObject(stripped);
    if (obj && obj != stripped) {
      const Value *objStripped = obj->stripPointerCasts();
      if (m_escaped_values.insert(objStripped).second) {
        worklist.push_back(objStripped);
      }
    }
  };
  auto seedPotentialSinkArguments = [&](const CallBase *call) {
    if (!call) {
      return;
    }

    auto *threadAPI = ThreadAPI::getThreadAPI();
    if (threadAPI && threadAPI->isTDFork(call)) {
      for (const Value *arg : threadAPI->getForkPayloadArgs(call)) {
        if (arg && arg->getType()->isPointerTy()) {
          enqueueEscaped(arg);
        }
      }
      return;
    }

    const Function *callee = resolveInternalCallee(call);
    if (callee && callee->isIntrinsic()) {
      return;
    }
    if (callee && !callee->isDeclaration()) {
      return;
    }
    if (callee &&
        concurrency::ConcurrencyContext::isKnownNonCapturingExternalFunction(
            callee)) {
      return;
    }

    for (unsigned i = 0; i < call->arg_size(); ++i) {
      if (call->doesNotCapture(i)) {
        continue;
      }
      const Value *arg = call->getArgOperand(i);
      if (arg && arg->getType()->isPointerTy()) {
        enqueueEscaped(arg);
      }
    }
  };

  // 1. Initial seeds and direct call-site index (avoids O(Args * TotalInsts)
  // full-module scans during reverse argument propagation).
  std::unordered_map<const Function *, std::vector<const CallBase *>>
      directCallSites;
  for (const GlobalValue &gv : m_module.globals()) {
    enqueueEscaped(&gv);
  }

  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (const auto *call = dyn_cast<CallBase>(&*I)) {
        if (const Function *callee = resolveInternalCallee(call)) {
          if (!callee->isDeclaration()) {
            directCallSites[callee].push_back(call);
          }
        }
        seedPotentialSinkArguments(call);
      }
    }
  }

  // 2. Fixed-point propagation (inclusion-based)
  while (!worklist.empty()) {
    const Value *curr = worklist.front();
    worklist.pop_front();
    curr = curr->stripPointerCasts();

    if (const auto *cast = dyn_cast<CastInst>(curr)) {
      enqueueEscaped(cast->getOperand(0));
    } else if (const auto *gep = dyn_cast<GetElementPtrInst>(curr)) {
      enqueueEscaped(gep->getPointerOperand());
    } else if (const auto *phi = dyn_cast<PHINode>(curr)) {
      for (const Value *incoming : phi->incoming_values()) {
        enqueueEscaped(incoming);
      }
    } else if (const auto *select = dyn_cast<SelectInst>(curr)) {
      enqueueEscaped(select->getTrueValue());
      enqueueEscaped(select->getFalseValue());
    } else if (const auto *load = dyn_cast<LoadInst>(curr)) {
      enqueueEscaped(load->getPointerOperand());
      const Value *pointer = load->getPointerOperand();
      for (const User *user : pointer->users()) {
        if (const auto *store = dyn_cast<StoreInst>(user)) {
          if (store->getPointerOperand() == pointer) {
            enqueueEscaped(store->getValueOperand());
          }
        }
      }
    } else if (const auto *call = dyn_cast<CallBase>(curr)) {
      const Function *callee = resolveInternalCallee(call);
      if (callee && !callee->isDeclaration()) {
        for (const BasicBlock &BB : *callee) {
          if (const auto *ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
            enqueueEscaped(ret->getReturnValue());
          }
        }
      }
    }

    for (const User *U : curr->users()) {
      if (const auto *inst = dyn_cast<Instruction>(U)) {
        if (const auto *store = dyn_cast<StoreInst>(inst)) {
          if (store->getValueOperand() == curr) {
            // value escapes to pointer
            enqueueEscaped(store->getPointerOperand());
          } else if (store->getPointerOperand() == curr) {
            // pointer is escaped, so any value stored in it escapes
            enqueueEscaped(store->getValueOperand());
          }
        } else if (const auto *load = dyn_cast<LoadInst>(inst)) {
          // loading from escaped pointer makes result escaped
          enqueueEscaped(load);
        } else if (isa<GetElementPtrInst>(inst) || isa<BitCastInst>(inst) ||
                   isa<PHINode>(inst) || isa<SelectInst>(inst) || isa<AddrSpaceCastInst>(inst)) {
          enqueueEscaped(inst);
        } else if (const auto *cb = dyn_cast<CallBase>(inst)) {
          // Argument passing
          const Function *callee = resolveInternalCallee(cb);
          if (callee && !callee->isDeclaration()) {
            for (unsigned i = 0; i < cb->arg_size(); ++i) {
              if (cb->getArgOperand(i) == curr && i < callee->arg_size()) {
                enqueueEscaped(callee->getArg(i));
              }
            }
          } else {
            for (unsigned i = 0; i < cb->arg_size(); ++i) {
              if (cb->getArgOperand(i) == curr &&
                  cb->getArgOperand(i)->getType()->isPointerTy()) {
                enqueueEscaped(cb->getArgOperand(i));
              }
            }
          }
        } else if (const auto *ret = dyn_cast<ReturnInst>(inst)) {
          // Return value escapes to all call sites
          const Function *F = ret->getFunction();
          auto csIt = directCallSites.find(F);
          if (csIt != directCallSites.end()) {
            for (const CallBase *cb : csIt->second) {
              enqueueEscaped(cb);
            }
          }
        }
      }
    }

    // Handle Argument -> CallSite (reverse of above)
    if (const auto *arg = dyn_cast<Argument>(curr)) {
      const Function *F = arg->getParent();
      auto csIt = directCallSites.find(F);
      if (csIt != directCallSites.end()) {
        for (const CallBase *cb : csIt->second) {
          if (arg->getArgNo() < cb->arg_size()) {
            enqueueEscaped(cb->getArgOperand(arg->getArgNo()));
          }
        }
      }
    }
  }
}

} // namespace lotus
