/**
 * @file ThreadLocalAnalysis.cpp
 * @brief Implementation of Thread-Local Storage Detection
 */

#include "Concurrency/Thread/Sharing/ThreadLocalAnalysis.h"

#include "Concurrency/API/ThreadAPI.h"

#include <deque>
#include <functional>
#include <unordered_set>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace ThreadLocal;

namespace {

bool isPointerFlowInstruction(const Instruction *inst) {
  return isa<CastInst>(inst) || isa<GetElementPtrInst>(inst) ||
         isa<PHINode>(inst) || isa<SelectInst>(inst) || isa<FreezeInst>(inst) ||
         isa<InsertValueInst>(inst) || isa<ExtractValueInst>(inst) ||
         isa<BinaryOperator>(inst);
}

} // namespace

ThreadLocalAnalysis::ThreadLocalAnalysis(Module &module) : m_module(module) {}

void ThreadLocalAnalysis::analyze() {
  invalidate();
  identifyThreadLocalGlobals();
  identifyThreadLocalAllocas();
  identifyPthreadSpecificData();
  m_analyzed = true;

  errs() << "Thread-Local Analysis: Found " << m_tls_globals.size()
         << " TLS globals, " << m_tls_allocas.size() << " TLS allocas\n";
}

void ThreadLocalAnalysis::invalidate() {
  m_tls_globals.clear();
  m_tls_allocas.clear();
  m_tls_values.clear();
  m_pthread_keys.clear();
  m_analyzed = false;
}

void ThreadLocalAnalysis::identifyThreadLocalGlobals() {
  for (GlobalVariable &gv : m_module.globals()) {
    // TLS linkage gives each thread a distinct instance, but another thread
    // may still access a particular instance if its address is published.
    if (hasThreadLocalStorageLinkage(&gv) && !escapesThread(&gv)) {
      m_tls_globals.insert(&gv);
      m_tls_values.insert(&gv);
    }
  }
}

void ThreadLocalAnalysis::identifyThreadLocalAllocas() {
  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (AllocaInst *alloca = dyn_cast<AllocaInst>(&inst)) {
          // Check if this alloca is thread-local (doesn't escape)
          if (isAllocaThreadLocal(alloca)) {
            m_tls_allocas.insert(alloca);
            m_tls_values.insert(alloca);
          }
        }
      }
    }
  }
}

void ThreadLocalAnalysis::identifyPthreadSpecificData() {
  // pthread_key_create creates thread-specific data keys
  // pthread_getspecific/pthread_setspecific access thread-specific data

  for (Function &func : m_module) {
    if (func.isDeclaration()) {
      continue;
    }

    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (CallBase *call = dyn_cast<CallBase>(&inst)) {
          Function *callee = call->getCalledFunction();
          if (!callee) {
            callee = dyn_cast<Function>(
                call->getCalledOperand()->stripPointerCasts());
          }
          if (!callee) {
            continue;
          }

          StringRef name = callee->getName();

          // pthread_key_create(pthread_key_t *key, ...)
          if (name.equals("pthread_key_create")) {
            if (call->arg_size() > 0) {
              m_pthread_keys.insert(call->getArgOperand(0));
            }
          }

          // pthread_getspecific returns the pointer stored in a per-thread
          // binding. The pointee itself may be shared. pthread_setspecific
          // returns an integer status, not thread-local storage.
        }
      }
    }
  }
}

bool ThreadLocalAnalysis::hasThreadLocalStorageLinkage(
    const GlobalVariable *gv) {
  // Check if the global has TLS storage
  return gv->isThreadLocal();
}

bool ThreadLocalAnalysis::isAllocaThreadLocal(const AllocaInst *alloca) const {
  // An alloca is thread-local if:
  // 1. It's a stack allocation (by definition in one thread's stack)
  // 2. Its address doesn't escape to other threads
  //
  // For now, we use a simple heuristic: if the alloca's address is never
  // stored to memory or passed to functions that could share it, it's
  // thread-local

  return !escapesThread(alloca);
}

bool ThreadLocalAnalysis::escapesThread(const Value *val) const {
  // Check if a value escapes its thread
  // A value escapes if:
  // - Its address is stored to a global variable
  // - It's passed to a function that could share it (pthread_create, etc.)
  // - It's stored to heap memory that could be accessed by other threads

  struct WorkItem {
    const Value *value;
    bool is_carrier;
  };

  std::deque<WorkItem> worklist;
  std::unordered_set<const Value *> visited_values;
  std::unordered_set<const Value *> visited_carriers;

  auto enqueue = [&](const Value *value, bool is_carrier) {
    if (!value) {
      return;
    }
    auto &visited = is_carrier ? visited_carriers : visited_values;
    if (visited.insert(value).second) {
      worklist.push_back({value, is_carrier});
    }
  };

  enqueue(val, false);
  ThreadAPI *thread_api = ThreadAPI::getThreadAPI();

  while (!worklist.empty()) {
    const WorkItem item = worklist.front();
    worklist.pop_front();
    const Value *current = item.value;

    for (const Use &use : current->uses()) {
      const User *user = use.getUser();

      if (isa<GlobalVariable>(user)) {
        return true;
      }

      // Check for escape scenarios
      if (const StoreInst *store = dyn_cast<StoreInst>(user)) {
        if (store->getValueOperand() != current) {
          continue;
        }

        const Value *ptr = store->getPointerOperand();
        const Value *base = getUnderlyingObject(ptr);
        base = base ? base->stripPointerCasts() : nullptr;

        if (isa_and_nonnull<GlobalVariable>(base) ||
            (base && !isa<AllocaInst>(base)) ||
            (!base && !isa<AllocaInst>(ptr))) {
          return true;
        }

        // The tracked pointer is temporarily stored in a stack carrier. Track
        // both publication of the carrier itself and loads of its contents.
        const Value *slot_base = base ? base : ptr->stripPointerCasts();
        if (isa<AllocaInst>(slot_base)) {
          enqueue(slot_base, true);
        }
      } else if (const LoadInst *load = dyn_cast<LoadInst>(user)) {
        if (item.is_carrier && load->getPointerOperand() == current) {
          enqueue(load, false);
        }
      } else if (const CallBase *call = dyn_cast<CallBase>(user)) {
        const unsigned op_no = use.getOperandNo();
        const bool is_call_arg = op_no < call->arg_size();

        if (!is_call_arg) {
          return true;
        }

        // Passing an address as thread payload is a direct cross-thread escape.
        if (thread_api && thread_api->isTDFork(call)) {
          for (const Value *payload : thread_api->getForkPayloadArgs(call)) {
            if (payload == current ||
                (payload && payload->stripPointerCasts() ==
                                current->stripPointerCasts())) {
              return true;
            }
          }
          continue;
        }

        const Function *callee =
            thread_api ? thread_api->getCallee(call) : nullptr;
        if (!callee) {
          return true;
        }

        if (!callee->isDeclaration()) {
          if (is_call_arg && op_no < callee->arg_size()) {
            const Argument *formal = callee->getArg(op_no);
            enqueue(formal, item.is_carrier);
          }
          continue;
        }

        StringRef name = callee->getName();

        // Known thread/task creation functions = escape
        if (name.contains("pthread_create") || name.contains("std::thread") ||
            name.contains("std::async")) {
          return true;
        }

        if (callee->isIntrinsic()) {
          // Retain known pointer-aliasing results. Other intrinsics with a
          // pointer argument may copy or publish pointer-containing memory;
          // only lifetime/debug markers are harmless for confinement.
          if (call->getType()->isPointerTy()) {
            enqueue(call, item.is_carrier);
            continue;
          }
          const auto *intrinsic = dyn_cast<IntrinsicInst>(call);
          if (intrinsic &&
              (intrinsic->getIntrinsicID() == Intrinsic::lifetime_start ||
               intrinsic->getIntrinsicID() == Intrinsic::lifetime_end ||
               isa<DbgInfoIntrinsic>(intrinsic))) {
            continue;
          }
          return true;
        }

        if (name.equals("free")) {
          continue;
        }

        // nocapture is a lifetime/capture property. It does not prove that an
        // external callee cannot expose the pointer to concurrent execution
        // while the call is active, nor does it account for returned aliases.
        return true;
      } else if (const ReturnInst *ret = dyn_cast<ReturnInst>(user)) {
        const Function *parent = ret->getFunction();
        if (!parent) {
          return true;
        }

        // Traverse parent's uses (including bitcast/GEP ConstantExpr wrappers)
        // instead of scanning every instruction in the module.
        bool propagated_to_caller = false;
        std::deque<const Value *> fn_worklist;
        std::unordered_set<const Value *> fn_visited;
        fn_worklist.push_back(parent);
        fn_visited.insert(parent);
        while (!fn_worklist.empty()) {
          const Value *fn_val = fn_worklist.front();
          fn_worklist.pop_front();
          for (const Use &fn_use : fn_val->uses()) {
            const User *fn_user = fn_use.getUser();
            if (const auto *cb = dyn_cast<CallBase>(fn_user)) {
              if (cb->isCallee(&fn_use) && thread_api &&
                  thread_api->getCallee(cb) == parent) {
                propagated_to_caller = true;
                enqueue(cb, item.is_carrier);
              }
            } else if (const auto *ce = dyn_cast<ConstantExpr>(fn_user)) {
              if (ce->isCast() ||
                  ce->getOpcode() == Instruction::GetElementPtr) {
                if (fn_visited.insert(ce).second) {
                  fn_worklist.push_back(ce);
                }
              }
            } else if (isa<BitCastInst>(fn_user) ||
                       isa<AddrSpaceCastInst>(fn_user)) {
              if (fn_visited.insert(fn_user).second) {
                fn_worklist.push_back(fn_user);
              }
            }
          }
        }

        if (!propagated_to_caller || parent->hasAddressTaken()) {
          return true;
        }
      } else if (const ConstantExpr *expr = dyn_cast<ConstantExpr>(user)) {
        if (expr->isCast() || expr->getOpcode() == Instruction::GetElementPtr) {
          enqueue(expr, item.is_carrier);
        } else {
          return true;
        }
      } else if (const Constant *constant = dyn_cast<Constant>(user)) {
        // Follow constant aggregates and aliases until they either reach a
        // global initializer (an escape) or prove dead.
        enqueue(constant, item.is_carrier);
      } else if (const Instruction *inst = dyn_cast<Instruction>(user)) {
        if (isPointerFlowInstruction(inst)) {
          enqueue(inst, item.is_carrier);
        } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(inst)) {
          if (rmw->getPointerOperand() != current) {
            return true;
          }
        } else if (const auto *cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst)) {
          if (cmpxchg->getPointerOperand() != current) {
            return true;
          }
        } else if (current->getType()->isPointerTy() && !isa<ICmpInst>(inst)) {
          // An unmodelled pointer-consuming instruction is an incomplete
          // pointer-flow edge, so it cannot support a definite-local result.
          return true;
        }
      } else {
        return true;
      }
    }
  }

  return false; // Doesn't escape
}

bool ThreadLocalAnalysis::isThreadLocal(const Value *val) const {
  if (!m_analyzed || !val) {
    return false;
  }

  std::unordered_set<const Value *> visiting;
  std::function<bool(const Value *)> prove_local = [&](const Value *value) {
    if (!value) {
      return false;
    }

    if (!visiting.insert(value).second) {
      return false;
    }

    auto finish = [&](bool result) {
      visiting.erase(value);
      return result;
    };

    // Direct check
    if (m_tls_values.count(value)) {
      return finish(true);
    }

    // Check if it's a global with TLS
    if (const GlobalVariable *gv = dyn_cast<GlobalVariable>(value)) {
      return finish(m_tls_globals.count(gv) > 0);
    }

    // Check if it's a thread-local alloca
    if (const AllocaInst *alloca = dyn_cast<AllocaInst>(value)) {
      return finish(m_tls_allocas.count(alloca) > 0);
    }

    // Trace through GEP, bitcast, etc.
    if (const GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(value)) {
      return finish(prove_local(gep->getPointerOperand()));
    }

    if (const CastInst *cast = dyn_cast<CastInst>(value)) {
      return finish(prove_local(cast->getOperand(0)));
    }

    if (const ConstantExpr *expr = dyn_cast<ConstantExpr>(value)) {
      if (expr->isCast() || expr->getOpcode() == Instruction::GetElementPtr) {
        return finish(prove_local(expr->getOperand(0)));
      }
      return finish(false);
    }

    if (const PHINode *phi = dyn_cast<PHINode>(value)) {
      for (const Value *incoming : phi->incoming_values()) {
        if (!prove_local(incoming)) {
          return finish(false);
        }
      }
      return finish(true);
    }

    if (const SelectInst *select = dyn_cast<SelectInst>(value)) {
      return finish(prove_local(select->getTrueValue()) &&
                    prove_local(select->getFalseValue()));
    }

    // Soundness fix:
    // A load from thread-local storage does NOT imply the loaded pointee/value
    // is itself thread-local. TLS slots frequently store pointers to
    // shared/global state.  Treating the loaded SSA value as thread-local can
    // suppress real races on the referenced object.
    if (const LoadInst *load = dyn_cast<LoadInst>(value)) {
      if (m_tls_values.count(load)) {
        return finish(true);
      }
      return finish(false);
    }

    return finish(false);
  };

  return prove_local(val);
}

bool ThreadLocalAnalysis::accessesThreadLocalStorage(
    const Instruction *inst) const {
  if (!m_analyzed) {
    return false;
  }

  auto accessesKnownThreadLocalBase = [this](const Value *ptr) {
    if (!ptr) {
      return false;
    }
    return isThreadLocal(ptr);
  };

  // Only accesses to known thread-local storage bases are pruned here.
  // Values returned by pthread_getspecific may themselves point to shared
  // memory, so deriving an address from that result is not enough to prove
  // the dereference thread-local.
  if (const LoadInst *load = dyn_cast<LoadInst>(inst)) {
    return accessesKnownThreadLocalBase(load->getPointerOperand());
  }

  if (const StoreInst *store = dyn_cast<StoreInst>(inst)) {
    return accessesKnownThreadLocalBase(store->getPointerOperand());
  }

  if (const AtomicRMWInst *rmw = dyn_cast<AtomicRMWInst>(inst)) {
    return accessesKnownThreadLocalBase(rmw->getPointerOperand());
  }

  if (const AtomicCmpXchgInst *cmpxchg = dyn_cast<AtomicCmpXchgInst>(inst)) {
    return accessesKnownThreadLocalBase(cmpxchg->getPointerOperand());
  }

  return false;
}

bool ThreadLocal::isObviouslyThreadLocal(const Value *val) {
  (void)val;
  // Without module-wide escape information, neither an alloca nor a TLS
  // global is definitely confined to the current thread.
  return false;
}
