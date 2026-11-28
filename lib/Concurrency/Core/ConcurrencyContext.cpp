#include "Concurrency/Core/ConcurrencyContext.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/CUDA/CUDAModel.h"
#include "Concurrency/Thread/ThreadMultiplicity.h"

#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/StringSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <unordered_set>

using namespace llvm;

namespace concurrency {

namespace {

bool isCallSignatureCompatible(const CallBase *call, const Function *callee) {
  if (!call || !callee) {
    return false;
  }
  const FunctionType *fnTy = callee->getFunctionType();
  if (!fnTy) {
    return false;
  }
  const unsigned numArgs = call->arg_size();
  const unsigned numParams = fnTy->getNumParams();
  if (fnTy->isVarArg()) {
    if (numArgs < numParams) {
      return false;
    }
  } else if (numArgs != numParams) {
    return false;
  }
  // Return void vs non-void mismatch when return value is actually used.
  if (fnTy->getReturnType()->isVoidTy() && !call->getType()->isVoidTy() &&
      !call->use_empty()) {
    return false;
  }
  return true;
}

bool collectStaticFunctionTargets(
    const Value *v, std::vector<const Function *> &targets,
    std::unordered_set<const Value *> &visited) {
  if (!v) {
    return false;
  }
  v = v->stripPointerCasts();
  if (!visited.insert(v).second) {
    return true;
  }
  if (const auto *fn = dyn_cast<Function>(v)) {
    targets.push_back(fn);
    return true;
  }
  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    bool t = collectStaticFunctionTargets(sel->getTrueValue(), targets, visited);
    bool f =
        collectStaticFunctionTargets(sel->getFalseValue(), targets, visited);
    return t && f;
  }
  if (const auto *phi = dyn_cast<PHINode>(v)) {
    if (phi->getNumIncomingValues() == 0) {
      return false;
    }
    bool allResolved = true;
    for (const Value *inc : phi->incoming_values()) {
      allResolved &= collectStaticFunctionTargets(inc, targets, visited);
    }
    return allResolved;
  }
  if (const auto *ce = dyn_cast<ConstantExpr>(v)) {
    if (ce->isCast() && ce->getNumOperands() > 0) {
      return collectStaticFunctionTargets(ce->getOperand(0), targets, visited);
    }
  }
  return false;
}

bool mayOriginateFromExternalOrUnknown(
    const Value *v, std::unordered_set<const Value *> &visited) {
  if (!v) {
    return true;
  }
  v = v->stripPointerCasts();
  if (!visited.insert(v).second) {
    return false;
  }
  if (isa<Function>(v)) {
    return false;
  }
  if (const auto *gv = dyn_cast<GlobalVariable>(v)) {
    return gv->isDeclaration() || gv->hasExternalLinkage() ||
           !gv->hasInitializer();
  }
  if (const auto *load = dyn_cast<LoadInst>(v)) {
    return mayOriginateFromExternalOrUnknown(load->getPointerOperand(), visited);
  }
  if (const auto *gep = dyn_cast<GEPOperator>(v)) {
    return mayOriginateFromExternalOrUnknown(gep->getPointerOperand(), visited);
  }
  if (const auto *phi = dyn_cast<PHINode>(v)) {
    for (const Value *inc : phi->incoming_values()) {
      if (mayOriginateFromExternalOrUnknown(inc, visited)) {
        return true;
      }
    }
    return false;
  }
  if (const auto *sel = dyn_cast<SelectInst>(v)) {
    return mayOriginateFromExternalOrUnknown(sel->getTrueValue(), visited) ||
           mayOriginateFromExternalOrUnknown(sel->getFalseValue(), visited);
  }
  if (isa<Argument>(v) || isa<CallBase>(v) || isa<IntToPtrInst>(v)) {
    return true;
  }
  if (const auto *alloca = dyn_cast<AllocaInst>(v)) {
    // Only safe if every store into the alloca stores a known function/source
    // and the alloca does not escape into calls or other stores.
    for (const User *u : alloca->users()) {
      if (const auto *st = dyn_cast<StoreInst>(u)) {
        if (st->getPointerOperand()->stripPointerCasts() == alloca) {
          if (mayOriginateFromExternalOrUnknown(st->getValueOperand(),
                                                visited)) {
            return true;
          }
        } else {
          return true;
        }
      } else if (!isa<LoadInst>(u) && !isa<BitCastInst>(u)) {
        return true;
      }
    }
    return false;
  }
  return true;
}

} // namespace

ConcurrencyContext::ConcurrencyContext(
    Module &module, lotus::AliasAnalysisWrapper *borrowed_aa)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()),
      m_alias_analysis(borrowed_aa) {}

ConcurrencyContext::~ConcurrencyContext() = default;

void ConcurrencyContext::setAliasAnalysis(lotus::AliasAnalysisWrapper *aa) {
  m_owned_alias_analysis.reset();
  m_alias_analysis = aa;
  m_resolved_callees.clear();
}

bool ConcurrencyContext::isKnownSyncFreeExternalFunction(
    const Function *func) {
  if (!func || !func->isDeclaration()) {
    return false;
  }
  if (func->isIntrinsic()) {
    // Thread/sync intrinsics are handled by ThreadAPI/Atomics, not here.
    StringRef name = func->getName();
    if (name.contains("barrier") || name.contains("fence") ||
        name.contains("atomic") || name.contains("nvvm") ||
        name.contains("dmb") || name.contains("dsb")) {
      return false;
    }
    return true;
  }
  if (func->doesNotAccessMemory() || func->onlyReadsMemory()) {
    return true;
  }

  static const StringSet<> kSyncFreeExternals = {
      // Memory & string operations (never invoke callbacks or touch user locks)
      "memcpy", "memmove", "memset", "memcmp", "bcmp", "bzero", "bcopy",
      "memchr", "strlen", "strnlen", "strcmp", "strncmp", "strcasecmp",
      "strncasecmp", "strcpy", "strncpy", "strcat", "strncat", "strchr",
      "strrchr", "strstr", "strspn", "strcspn", "strpbrk", "strdup", "strndup",
      "strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof", "atoi",
      "atol", "atoll", "atof", "__memcpy_chk", "__memmove_chk", "__memset_chk",
      "__strcpy_chk", "__strncpy_chk", "__strcat_chk", "__strncat_chk",
      // Standard I/O formatting (non-callback)
      "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf",
      "vsprintf", "vsnprintf", "puts", "fputs", "putchar", "fputc", "putc",
      "fwrite", "fread", "fflush", "perror", "strerror", "strerror_r",
      "__printf_chk", "__fprintf_chk", "__sprintf_chk", "__snprintf_chk",
      // Memory allocation (internal allocator locks are disjoint from user locks)
      "malloc", "calloc", "realloc", "free", "aligned_alloc", "posix_memalign",
      "_Znwm", "_Znam", "_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm",
      "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t",
      // Math, time, errno, process/exception runtime
      "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh",
      "tanh", "exp", "exp2", "log", "log2", "log10", "pow", "sqrt", "cbrt",
      "ceil", "floor", "round", "trunc", "fabs", "fmod", "hypot", "abs",
      "labs", "llabs", "rand", "srand", "time", "clock", "clock_gettime",
      "gettimeofday", "__errno_location", "__error", "abort", "exit", "_exit",
      "__assert_fail", "__cxa_allocate_exception", "__cxa_free_exception",
      "__cxa_throw", "__cxa_begin_catch", "__cxa_end_catch", "__cxa_rethrow"};

  StringRef name = func->getName();
  if (name.startswith("\01")) {
    name = name.drop_front(1);
  }
  return kSyncFreeExternals.count(name) != 0;
}

bool ConcurrencyContext::isKnownNonCapturingExternalFunction(
    const Function *func) {
  if (!func || !func->isDeclaration()) {
    return false;
  }
  if (func->isIntrinsic()) {
    return true;
  }
  static const StringSet<> kNonCapturingExternals = {
      // Read-only inspection / formatting / zeroing / freeing: never publish
      // pointer arguments into thread-shared global state.
      "memcmp", "bcmp", "memchr", "memset", "bzero", "__memset_chk",
      "strlen", "strnlen", "strcmp", "strncmp", "strcasecmp", "strncasecmp",
      "strchr", "strrchr", "strstr", "strspn", "strcspn", "strpbrk",
      "strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof", "atoi",
      "atol", "atoll", "atof",
      "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf",
      "vsprintf", "vsnprintf", "puts", "fputs", "fwrite", "fread", "fflush",
      "perror", "__printf_chk", "__fprintf_chk", "__sprintf_chk",
      "__snprintf_chk",
      "free", "_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm",
      "clock_gettime", "gettimeofday", "time"};

  StringRef name = func->getName();
  if (name.startswith("\01")) {
    name = name.drop_front(1);
  }
  return kNonCapturingExternals.count(name) != 0;
}

const ConcurrencyContext::ResolvedCallees &
ConcurrencyContext::resolveCallees(const CallBase *call) {
  static const ResolvedCallees kEmpty;
  if (!call) {
    return kEmpty;
  }
  auto it = m_resolved_callees.find(call);
  if (it != m_resolved_callees.end()) {
    return it->second;
  }

  ResolvedCallees resolved;
  auto addTarget = [&](const Function *fn) {
    if (!fn) {
      return;
    }
    if (fn->isDeclaration()) {
      if (!isKnownSyncFreeExternalFunction(fn)) {
        resolved.has_unresolved = true;
      }
      return;
    }
    if (std::find(resolved.targets.begin(), resolved.targets.end(), fn) ==
        resolved.targets.end()) {
      resolved.targets.push_back(fn);
    }
  };

  if (const Function *direct = call->getCalledFunction()) {
    addTarget(direct);
    return m_resolved_callees.emplace(call, std::move(resolved)).first->second;
  }

  const Value *called = call->getCalledOperand();
  if (!called) {
    resolved.has_unresolved = true;
    return m_resolved_callees.emplace(call, std::move(resolved)).first->second;
  }

  if (const Function *directStripped =
          dyn_cast<Function>(called->stripPointerCastsAndAliases())) {
    addTarget(directStripped);
    return m_resolved_callees.emplace(call, std::move(resolved)).first->second;
  }

  // 1. Static def-use targets (phi/select/cast of Function*).
  std::vector<const Function *> staticTargets;
  std::unordered_set<const Value *> visitedStatic;
  const bool staticallyComplete =
      collectStaticFunctionTargets(called, staticTargets, visitedStatic);
  for (const Function *fn : staticTargets) {
    if (isCallSignatureCompatible(call, fn)) {
      addTarget(fn);
    }
  }
  if (staticallyComplete && !staticTargets.empty()) {
    return m_resolved_callees.emplace(call, std::move(resolved)).first->second;
  }

  // 2. AA points-to targets (filtered by compatible signature).
  bool aaResolvedAll = false;
  if (m_alias_analysis) {
    std::vector<const Value *> pts;
    if (m_alias_analysis->getPointsToSet(called, pts) && !pts.empty()) {
      bool allAreFunctions = true;
      for (const Value *target : pts) {
        if (!target) {
          allAreFunctions = false;
          continue;
        }
        const Value *stripped = target->stripPointerCastsAndAliases();
        if (const auto *fn = dyn_cast<Function>(stripped)) {
          if (isCallSignatureCompatible(call, fn)) {
            addTarget(fn);
          }
        } else {
          allAreFunctions = false;
        }
      }
      std::unordered_set<const Value *> originVisited;
      if (allAreFunctions &&
          !mayOriginateFromExternalOrUnknown(called, originVisited)) {
        aaResolvedAll = !resolved.targets.empty();
      }
    }
  }

  if (!aaResolvedAll) {
    resolved.has_unresolved = true;
  }

  return m_resolved_callees.emplace(call, std::move(resolved)).first->second;
}

ConcurrencyContext::ResolvedCallees
ConcurrencyContext::resolveFunctionTargets(const Value *fnVal) const {
  ResolvedCallees resolved;
  if (!fnVal) {
    resolved.has_unresolved = true;
    return resolved;
  }

  auto addTarget = [&](const Function *fn) {
    if (!fn) {
      return;
    }
    if (fn->isDeclaration()) {
      resolved.has_unresolved = true;
      return;
    }
    if (std::find(resolved.targets.begin(), resolved.targets.end(), fn) ==
        resolved.targets.end()) {
      resolved.targets.push_back(fn);
    }
  };

  if (const auto *direct =
          dyn_cast<Function>(fnVal->stripPointerCastsAndAliases())) {
    addTarget(direct);
    return resolved;
  }

  std::vector<const Function *> staticTargets;
  std::unordered_set<const Value *> visitedStatic;
  const bool staticallyComplete =
      collectStaticFunctionTargets(fnVal, staticTargets, visitedStatic);
  for (const Function *fn : staticTargets) {
    addTarget(fn);
  }
  if (staticallyComplete && !staticTargets.empty()) {
    return resolved;
  }

  bool aaResolvedAll = false;
  if (m_alias_analysis) {
    std::vector<const Value *> pts;
    if (m_alias_analysis->getPointsToSet(fnVal, pts) && !pts.empty()) {
      bool allAreFunctions = true;
      for (const Value *target : pts) {
        if (!target) {
          allAreFunctions = false;
          continue;
        }
        if (const auto *fn =
                dyn_cast<Function>(target->stripPointerCastsAndAliases())) {
          addTarget(fn);
        } else {
          allAreFunctions = false;
        }
      }
      std::unordered_set<const Value *> originVisited;
      if (allAreFunctions &&
          !mayOriginateFromExternalOrUnknown(fnVal, originVisited)) {
        aaResolvedAll = !resolved.targets.empty();
      }
    }
  }

  if (!aaResolvedAll) {
    resolved.has_unresolved = true;
  }
  return resolved;
}

bool ConcurrencyContext::moduleHasPotentialThreadCreation(const Module &module,
                                                          ThreadAPI *api) {
  if (!api) {
    return true;
  }
  for (const Function &func : module) {
    if (func.hasFnAttribute("nvvm.kernel") ||
        func.getCallingConv() == CallingConv::PTX_Kernel) {
      return true;
    }
    if (func.isDeclaration()) {
      const ThreadAPI::TD_TYPE ty = api->getType(&func);
      if (ty != ThreadAPI::TD_DUMMY) {
        const RuntimeKind rk = api->getRuntimeLibrary(&func);
        if (rk == RuntimeKind::OpenMP || rk == RuntimeKind::CUDA ||
            rk == RuntimeKind::LinuxKernel) {
          return true;
        }
        if (func.hasAddressTaken() &&
            (ty == ThreadAPI::TD_FORK || ty == ThreadAPI::TD_OMP_TASK ||
             ty == ThreadAPI::TD_OMP_TASKLOOP ||
             ty == ThreadAPI::TD_CUDA_KERNEL_LAUNCH)) {
          return true;
        }
      }
      continue;
    }
    for (const Instruction &inst : instructions(func)) {
      const auto *cb = dyn_cast<CallBase>(&inst);
      if (!cb) {
        continue;
      }
      if (api->isTDFork(cb)) {
        return true;
      }
      const ThreadAPI::TD_TYPE ty = api->getType(cb);
      if (ty == ThreadAPI::TD_OMP_TASK || ty == ThreadAPI::TD_OMP_TASKLOOP ||
          ty == ThreadAPI::TD_CUDA_KERNEL_LAUNCH) {
        return true;
      }
    }
  }
  return false;
}

bool ConcurrencyContext::hasPotentialThreadCreation() {
  const auto &summary = getSummary();
  if (summary.has_forks || summary.has_openmp || summary.has_cuda ||
      summary.has_linux_kernel) {
    return true;
  }
  return moduleHasPotentialThreadCreation(m_module, m_thread_api);
}

lotus::AliasAnalysisWrapper *ConcurrencyContext::getOrCreateAliasAnalysis() {
  if (!m_alias_analysis) {
    m_owned_alias_analysis = lotus::AliasAnalysisFactory::create(
        m_module, lotus::AAConfig::SparrowAA_NoCtx());
    m_alias_analysis = m_owned_alias_analysis.get();
  }
  return m_alias_analysis;
}

CallGraph &ConcurrencyContext::getCallGraph() {
  if (!m_call_graph) {
    m_call_graph = std::make_unique<CallGraph>(m_module);
  }
  return *m_call_graph;
}

const ModuleConcurrencySummary &ConcurrencyContext::getSummary() {
  if (!m_summary_built) {
    buildSummary();
  }
  return m_summary;
}

const DominatorTree &
ConcurrencyContext::getDominatorTree(const Function &func) {
  auto it = m_dom_trees.find(&func);
  if (it != m_dom_trees.end()) {
    return *it->second;
  }
  auto dt = std::make_unique<DominatorTree>(const_cast<Function &>(func));
  const DominatorTree &ref = *dt;
  m_dom_trees.emplace(&func, std::move(dt));
  return ref;
}

const PostDominatorTree &
ConcurrencyContext::getPostDominatorTree(const Function &func) {
  auto it = m_post_dom_trees.find(&func);
  if (it != m_post_dom_trees.end()) {
    return *it->second;
  }
  auto pdt = std::make_unique<PostDominatorTree>();
  pdt->recalculate(const_cast<Function &>(func));
  const PostDominatorTree &ref = *pdt;
  m_post_dom_trees.emplace(&func, std::move(pdt));
  return ref;
}

const LoopInfo &ConcurrencyContext::getLoopInfo(const Function &func) {
  auto it = m_loop_infos.find(&func);
  if (it != m_loop_infos.end()) {
    return *it->second;
  }
  const DominatorTree &dt = getDominatorTree(func);
  auto li = std::make_unique<LoopInfo>(dt);
  const LoopInfo &ref = *li;
  m_loop_infos.emplace(&func, std::move(li));
  return ref;
}

ThreadMultiplicityAnalysis &ConcurrencyContext::getThreadMultiplicity() {
  if (!m_thread_multiplicity) {
    m_thread_multiplicity =
        std::make_unique<ThreadMultiplicityAnalysis>(m_module, &getCallGraph());
  }
  return *m_thread_multiplicity;
}

void ConcurrencyContext::invalidate() {
  m_summary_built = false;
  m_summary = ModuleConcurrencySummary{};
  m_call_graph.reset();
  m_thread_multiplicity.reset();
  m_dom_trees.clear();
  m_post_dom_trees.clear();
  m_loop_infos.clear();
  m_resolved_callees.clear();
}

void ConcurrencyContext::buildSummary() {
  m_summary = ModuleConcurrencySummary{};
  m_summary_built = true;

  if (m_module.getNamedMetadata("nvvm.annotations") != nullptr ||
      StringRef(m_module.getTargetTriple()).contains("nvptx")) {
    m_summary.has_cuda = true;
  }

  for (const GlobalVariable &gv : m_module.globals()) {
    const unsigned as = gv.getAddressSpace();
    if (as == 1 || as == 3 || as == 4 || as == 101) {
      m_summary.has_cuda = true;
      break;
    }
    const StringRef name = gv.getName();
    if (name.contains("cuda") || name.contains("CUDA") ||
        name.contains("nvvm")) {
      m_summary.has_cuda = true;
      break;
    }
  }

  for (const Function &func : m_module) {
    const StringRef fn_name = func.getName();
    if (func.hasFnAttribute("nvvm.kernel") ||
        func.getCallingConv() == CallingConv::PTX_Kernel ||
        CUDAModel::isLegacyKernelConfiguration(fn_name) ||
        fn_name.contains("GraphLaunch") || fn_name.contains("cuda") ||
        fn_name.contains("CUDA") || fn_name.contains("nvvm") ||
        fn_name.contains("__device_stub__")) {
      m_summary.has_cuda = true;
    }
    if (m_thread_api) {
      const ThreadAPI::TD_TYPE fn_type = m_thread_api->getType(&func);
      if (fn_type != ThreadAPI::TD_DUMMY) {
        switch (m_thread_api->getRuntimeLibrary(&func)) {
        case RuntimeKind::OpenMP:
          m_summary.has_openmp = true;
          break;
        case RuntimeKind::MPI:
          m_summary.has_mpi = true;
          break;
        case RuntimeKind::CUDA:
          m_summary.has_cuda = true;
          break;
        case RuntimeKind::LinuxKernel:
          m_summary.has_linux_kernel = true;
          break;
        default:
          break;
        }
      }
    }

    if (func.isDeclaration()) {
      continue;
    }

    DenseSet<const Function *> seen_callees;
    for (const Instruction &inst : instructions(func)) {
      if (isa<FenceInst>(inst) || isa<AtomicRMWInst>(inst) ||
          isa<AtomicCmpXchgInst>(inst)) {
        m_summary.has_atomics = true;
        continue;
      }
      if (const auto *load = dyn_cast<LoadInst>(&inst)) {
        if (load->isAtomic()) {
          m_summary.has_atomics = true;
        }
        continue;
      }
      if (const auto *store = dyn_cast<StoreInst>(&inst)) {
        if (store->isAtomic()) {
          m_summary.has_atomics = true;
        }
        continue;
      }

      const auto *cb = dyn_cast<CallBase>(&inst);
      if (!cb) {
        continue;
      }

      if (m_thread_api) {
        const Function *callee = m_thread_api->getCallee(cb);
        if (callee && !callee->isDeclaration() &&
            seen_callees.insert(callee).second) {
          m_summary.direct_callees[&func].push_back(callee);
          m_summary.direct_callers[callee].push_back(&func);
        }

        const ThreadAPI::TD_TYPE type = m_thread_api->getType(cb);
        if (type != ThreadAPI::TD_DUMMY) {
          if (m_thread_api->isTDFork(cb)) {
            m_summary.has_forks = true;
            m_summary.fork_sites.push_back(cb);
          } else if (m_thread_api->isTDJoin(cb)) {
            m_summary.has_joins = true;
            m_summary.join_sites.push_back(cb);
          }
          if (m_thread_api->isTDAcquire(cb) || m_thread_api->isTDRelease(cb) ||
              m_thread_api->isReadLockAcquire(cb) ||
              m_thread_api->isWriteLockAcquire(cb) ||
              m_thread_api->isTryLock(cb)) {
            m_summary.has_locks = true;
          }
          if (m_thread_api->isTDCondWait(cb) ||
              m_thread_api->isTDCondSignal(cb) ||
              m_thread_api->isTDCondBroadcast(cb)) {
            m_summary.has_condvars = true;
          }
          if (m_thread_api->isTDBarWait(cb)) {
            m_summary.has_barriers = true;
          }
          switch (m_thread_api->getRuntimeLibrary(cb)) {
          case RuntimeKind::OpenMP:
            m_summary.has_openmp = true;
            break;
          case RuntimeKind::MPI:
            m_summary.has_mpi = true;
            break;
          case RuntimeKind::CUDA:
            m_summary.has_cuda = true;
            break;
          case RuntimeKind::LinuxKernel:
            m_summary.has_linux_kernel = true;
            break;
          default:
            break;
          }
        }
      }
    }
  }
}

} // namespace concurrency
