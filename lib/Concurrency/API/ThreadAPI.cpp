/**
 * @file ThreadAPI.cpp
 * @brief Thread API Recognition Implementation
 *
 * This file implements the ThreadAPI singleton class for recognizing and
 * categorizing thread-related API calls in multithreaded programs.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

/*
 *
 * Author: rainoftime
 */
#include "Concurrency/API/ThreadAPI.h"

#include "Concurrency/API/CppThreading.h"
#include "Concurrency/CUDA/CUDAModel.h"
#include "Concurrency/Core/CallTarget.h"
#include "Concurrency/LinuxKernel/LinuxKernelConfig.h"
#include "Concurrency/LockSet/RAIILockTracker.h"
#include "Concurrency/MPI/MPISymbol.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h> // for StringMap
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <stdio.h>

using namespace std;
using namespace llvm;

ThreadAPI *ThreadAPI::tdAPI = nullptr;

/**
 * @struct ei_pair
 * @brief Maps thread API function names to their types
 *
 * Internal structure mapping function name strings to their corresponding
 * TD_TYPE enumeration values for classification.
 */
struct ei_pair {
  const char *n;        ///< Function name
  ThreadAPI::TD_TYPE t; ///< Thread API type
};

static StringRef stripAPIGlobalPrefix(StringRef name) {
  while (name.startswith("\01")) {
    name = name.drop_front();
  }
  return name;
}

static bool looksLikeMPISymbol(StringRef name) {
  name = stripAPIGlobalPrefix(name);
  return name.startswith("MPI_") || name.startswith("PMPI_") ||
         name.startswith("mpi_") || name.startswith("pmpi_") ||
         name.startswith("ompi_mpi_") || name.startswith("__wrap_MPI_") ||
         name.startswith("__wrap_PMPI_");
}

static std::string normalizeGenericAPIName(StringRef name) {
  return stripAPIGlobalPrefix(name).str();
}

static std::string normalizeConfiguredAPIName(StringRef name) {
  name = stripAPIGlobalPrefix(name);
  if (looksLikeMPISymbol(name)) {
    return mpi::normalizeMPISymbolName(name);
  }
  return name.str();
}

static std::string normalizeLegacyUnderscoreAlias(StringRef name) {
  name = stripAPIGlobalPrefix(name);
  if (name.startswith("_") && !name.startswith("__wrap_")) {
    return name.drop_front().str();
  }
  return name.str();
}

static bool appendLookupName(SmallVectorImpl<std::string> &names,
                             const std::string &candidate) {
  if (candidate.empty()) {
    return false;
  }
  if (llvm::is_contained(names, candidate)) {
    return false;
  }
  names.push_back(candidate);
  return true;
}

static std::string normalizeAPIName(StringRef name);

static SmallVector<std::string, 8> getConfiguredLookupNames(StringRef name) {
  SmallVector<std::string, 8> names;
  const std::string generic = normalizeGenericAPIName(name);
  appendLookupName(names, generic);

  const std::string legacy_alias = normalizeLegacyUnderscoreAlias(name);
  appendLookupName(names, legacy_alias);

  StringRef unprefixed = stripAPIGlobalPrefix(name);
  if (unprefixed.startswith("__wrap_")) {
    appendLookupName(names, unprefixed.drop_front(7).str());
  }

  appendLookupName(names, normalizeAPIName(name));

  if (looksLikeMPISymbol(name)) {
    appendLookupName(names,
                     mpi::normalizeMPISymbolName(stripAPIGlobalPrefix(name)));
  }
  return names;
}

static std::string normalizeAPIName(StringRef name) {
  name = stripAPIGlobalPrefix(name);
  if (name.startswith("__device_stub__")) {
    name = name.drop_front(sizeof("__device_stub__") - 1);
  }
  if (name.startswith("__stub__")) {
    name = name.drop_front(sizeof("__stub__") - 1);
  }
  if (name.endswith("_v2") &&
      (name.startswith("cuda") || name.startswith("cu") ||
       name.startswith("__cuda"))) {
    name = name.drop_back(3);
  }
  return name.str();
}

static ThreadAPI::RuntimeLibrary parseRuntimeLibrary(StringRef value) {
  std::string lowered = value.lower();
  if (lowered == "pthread")
    return ThreadAPI::RuntimeLibrary::PThread;
  if (lowered == "openmp")
    return ThreadAPI::RuntimeLibrary::OpenMP;
  if (lowered == "mpi")
    return ThreadAPI::RuntimeLibrary::MPI;
  if (lowered == "cpp")
    return ThreadAPI::RuntimeLibrary::Cpp;
  if (lowered == "cuda")
    return ThreadAPI::RuntimeLibrary::CUDA;
  if (lowered == "linux-kernel")
    return ThreadAPI::RuntimeLibrary::LinuxKernel;
  if (lowered == "hare")
    return ThreadAPI::RuntimeLibrary::Hare;
  if (lowered == "custom")
    return ThreadAPI::RuntimeLibrary::Custom;
  return ThreadAPI::RuntimeLibrary::Unknown;
}

/**
 * @brief Thread API mapping table
 *
 * Maps common thread library function names to their semantic types.
 * This enables static recognition of thread operations in LLVM IR.
 */
static const ei_pair ei_pairs[] = {
    // The current llvm-gcc puts in the \01.
    {"pthread_create", ThreadAPI::TD_FORK},
    {"apr_thread_create", ThreadAPI::TD_FORK},
    {"pthread_join", ThreadAPI::TD_JOIN},
    {"\01_pthread_join", ThreadAPI::TD_JOIN},
    {"pthread_cancel", ThreadAPI::TD_CANCEL},
    {"pthread_mutex_lock", ThreadAPI::TD_ACQUIRE},
    {"sem_wait", ThreadAPI::TD_ACQUIRE},
    {"_spin_lock", ThreadAPI::TD_ACQUIRE},
    {"SRE_SplSpecLockEx", ThreadAPI::TD_ACQUIRE},
    {"pthread_rwlock_rdlock", ThreadAPI::TD_RWLOCK_RDLOCK},
    {"pthread_rwlock_tryrdlock", ThreadAPI::TD_RWLOCK_RDLOCK},
    {"pthread_rwlock_wrlock", ThreadAPI::TD_RWLOCK_WRLOCK},
    {"pthread_rwlock_trywrlock", ThreadAPI::TD_RWLOCK_WRLOCK},
    {"pthread_mutex_trylock", ThreadAPI::TD_TRY_ACQUIRE},
    {"pthread_mutex_unlock", ThreadAPI::TD_RELEASE},
    {"pthread_rwlock_unlock", ThreadAPI::TD_RELEASE},
    {"sem_post", ThreadAPI::TD_RELEASE},
    {"_spin_unlock", ThreadAPI::TD_RELEASE},
    {"SRE_SplSpecUnlockEx", ThreadAPI::TD_RELEASE},
    //    {"pthread_cancel", ThreadAPI::TD_CANCEL},
    {"pthread_exit", ThreadAPI::TD_EXIT},
    {"pthread_detach", ThreadAPI::TD_DETACH},
    {"pthread_cond_wait", ThreadAPI::TD_COND_WAIT},
    {"pthread_cond_signal", ThreadAPI::TD_COND_SIGNAL},
    {"pthread_cond_broadcast", ThreadAPI::TD_COND_BROADCAST},
    {"pthread_cond_init", ThreadAPI::TD_CONDVAR_INI},
    {"pthread_cond_destroy", ThreadAPI::TD_CONDVAR_DESTROY},
    {"pthread_mutex_init", ThreadAPI::TD_MUTEX_INI},
    {"pthread_mutex_destroy", ThreadAPI::TD_MUTEX_DESTROY},
    {"pthread_barrier_init", ThreadAPI::TD_BAR_INIT},
    {"pthread_barrier_wait", ThreadAPI::TD_BAR_WAIT},

    // Hare APIs
    {"hare_parallel_for", ThreadAPI::HARE_PAR_FOR},

    // This must be the last entry.
    {0, ThreadAPI::TD_DUMMY}

};

/**
 * @brief Initialize the thread API map
 *
 * Populates the tdAPIMap with function name to type mappings from ei_pairs.
 * Validates that entries are grouped by type for maintainability.
 * Then loads config/thread.spec and config/concurrency_api.spec. API
 * recognition precedence: exact tdAPIMap first, then match rules (first match
 * in order; prefer more specific rules when adding new ones), then
 * C++/OpenMP/MPI/kernel name checks.
 */
void ThreadAPI::init() {
  m_kernel_api_registry.load(kernel::LinuxKernelConfig{});
  set<TD_TYPE> t_seen;
  TD_TYPE prev_t = TD_DUMMY;
  t_seen.insert(TD_DUMMY);
  for (const ei_pair *p = ei_pairs; p->n; ++p) {
    if (p->t != prev_t) {
      // This will detect if you move an entry to another block
      //   but forget to change the type.
      if (t_seen.count(p->t)) {
        fputs(p->n, stderr);
        putc('\n', stderr);
        assert(!"ei_pairs not grouped by type");
      }
      t_seen.insert(p->t);
      prev_t = p->t;
    }
    if (tdAPIMap.count(p->n)) {
      fputs(p->n, stderr);
      putc('\n', stderr);
      assert(!"duplicate name in ei_pairs");
    }
    tdAPIMap[p->n] = p->t;
  }
  std::vector<std::string> config_dirs;
  if (const char *override_dir = std::getenv("LOTUS_CONFIG_DIR")) {
    if (*override_dir)
      config_dirs.emplace_back(override_dir);
  }
#ifdef LOTUS_SOURCE_CONFIG_DIR
  config_dirs.emplace_back(LOTUS_SOURCE_CONFIG_DIR);
#endif
#ifdef LOTUS_INSTALL_CONFIG_DIR
  config_dirs.emplace_back(LOTUS_INSTALL_CONFIG_DIR);
#endif
  config_dirs.emplace_back("config");
  config_dirs.emplace_back("../config");

  auto loadFirst = [&](StringRef basename, bool semantic) {
    for (const std::string &dir : config_dirs) {
      const std::string path = dir + "/" + basename.str();
      std::ifstream file(path);
      if (!file.is_open())
        continue;
      file.close();
      if (semantic)
        loadSemanticConfig(path);
      else
        loadConfig(path);
      return;
    }
    llvm::errs() << "Lotus concurrency configuration not found: " << basename
                 << "\n";
  };

  loadFirst("thread.spec", false);
  loadFirst("concurrency_api.spec", true);
  loadFirst("openmp_api.spec", true);
  loadFirst("mpi_api.spec", true);

  // OpenMP fork_call has no thread-handle operand and carries a variadic list
  // of captured values after the outlined function.
  m_fork_args["__kmpc_fork_call"] =
      ForkArgIndices{ForkArgIndices::NoArgument, 2, 3, 3, true};
}

void ThreadAPI::reinitialize() {
  tdAPIMap.clear();
  m_fork_args.clear();
  m_join_args.clear();
  m_api_descriptions.clear();
  m_match_rules.clear();
  m_exact_match_rules.clear();
  m_prefix_match_rules.clear();
  m_config = concurrency::ConcurrencyConfig{};
  clearCaches();
  init();
}

void ThreadAPI::addEntry(const std::string &name, TD_TYPE type) {
  tdAPIMap[normalizeConfiguredAPIName(name)] = type;
  clearCaches();
}

void ThreadAPI::addDescription(const std::string &name,
                               const APIDescription &description) {
  m_api_descriptions[normalizeConfiguredAPIName(name)] = description;
  clearCaches();
}

void ThreadAPI::addMatchRule(const std::string &pattern, MatchKind kind,
                             const APIDescription &description) {
  MatchRule rule;
  rule.pattern = normalizeConfiguredAPIName(pattern);
  rule.kind = kind;
  rule.description = description;
  if (kind == MatchKind::Exact) {
    m_exact_match_rules.emplace(rule.pattern, rule);
  } else {
    m_prefix_match_rules.push_back(rule);
  }
  m_match_rules.push_back(std::move(rule));
  clearCaches();
}

bool ThreadAPI::hasMappedAPIEntry(const Function *F) const {
  if (!F)
    return false;

  for (const std::string &name : getConfiguredLookupNames(F->getName())) {
    if (tdAPIMap.count(name) != 0) {
      return true;
    }
  }
  return false;
}

bool ThreadAPI::hasMappedAPIEntry(const CallBase *cb) const {
  if (!cb)
    return false;
  if (const GlobalValue *called = getCalledGlobal(cb)) {
    for (const std::string &name : getConfiguredLookupNames(called->getName()))
      if (tdAPIMap.count(name) != 0)
        return true;
  }
  return hasMappedAPIEntry(getCallee(cb));
}

bool ThreadAPI::isCppThreadLikeFork(const Function *F) const {
  if (!F)
    return false;
  StringRef name = F->getName();
  return CppThreadingModel::isFork(name) ||
         CppThreadingModel::isJthreadConstructor(name) ||
         CppThreadingModel::isAsync(name);
}

bool ThreadAPI::isDefiniteAsyncLaunch(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  const Function *callee = getCallee(inst);
  if (!cb || !callee || getType(cb) != TD_ASYNC)
    return false;

  if (cb->arg_size() == 0)
    return true;

  const Value *policy = cb->getArgOperand(0)->stripPointerCasts();
  const auto *launch_bits = dyn_cast<ConstantInt>(policy);
  if (!launch_bits)
    return false;

  // std::launch::async is specified as bit 0 in all supported libstdc++/libc++
  // implementations we target here.
  return (launch_bits->getZExtValue() & 0x1ULL) != 0;
}

bool ThreadAPI::isProvablyDeferredAsyncLaunch(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  const Function *callee = getCallee(inst);
  if (!cb || !callee || getType(cb) != TD_ASYNC || cb->arg_size() == 0) {
    return false;
  }

  const Value *policy = cb->getArgOperand(0)->stripPointerCasts();
  const auto *launch_bits = dyn_cast<ConstantInt>(policy);
  if (!launch_bits) {
    return false;
  }

  return (launch_bits->getZExtValue() & 0x1ULL) == 0;
}

unsigned
ThreadAPI::getCppForkCallableSearchStart(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  const Function *callee = getCallee(inst);
  if (!cb || !callee) {
    return 1;
  }
  if (getType(cb) != TD_ASYNC) {
    return 1;
  }
  if (cb->arg_size() == 0) {
    return 0;
  }

  const Value *first_arg = cb->getArgOperand(0);
  if (!first_arg) {
    return 0;
  }
  first_arg = first_arg->stripPointerCasts();
  if (isa<ConstantInt>(first_arg) || first_arg->getType()->isIntegerTy()) {
    return 1;
  }
  return 0;
}

const Value *ThreadAPI::getCallArg(const Instruction *inst,
                                   unsigned idx) const {
  const CallBase *cb = getLLVMCallSite(inst);
  if (!cb || idx >= cb->arg_size())
    return nullptr;
  return cb->getArgOperand(idx);
}

const Value *ThreadAPI::getCallField(const Instruction *inst,
                                     unsigned field) const {
  if (field == ForkArgIndices::NoArgument)
    return nullptr;
  if (field == ForkArgIndices::ReturnValue)
    return llvm::isa<llvm::CallBase>(inst) ? inst : nullptr;
  return getCallArg(inst, field);
}

const Value *ThreadAPI::getCppThreadCallable(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  if (!cb)
    return nullptr;

  const unsigned first_callable_idx = getCppForkCallableSearchStart(inst);

  // Skip the constructor `this` parameter and look for a direct function-like
  // operand. If none exists, callers fall back to unresolved-fork conservatism.
  for (unsigned idx = first_callable_idx; idx < cb->arg_size(); ++idx) {
    const Value *candidate = cb->getArgOperand(idx);
    if (!candidate)
      continue;
    candidate = candidate->stripPointerCasts();
    if (isa<Function>(candidate))
      return candidate;
  }
  return nullptr;
}

bool ThreadAPI::isConditionVariableAny(const Function *F) const {
  if (!F) {
    return false;
  }
  return normalizeAPIName(F->getName()).find("condition_variable_any") !=
         std::string::npos;
}

const kernel::LinuxKernelAPISemantics *
ThreadAPI::lookupKernelSemantics(StringRef name) const {
  if (name.empty()) {
    return nullptr;
  }
  auto it = m_kernel_semantics_cache.find(name);
  if (it != m_kernel_semantics_cache.end()) {
    return it->second;
  }
  const kernel::LinuxKernelAPISemantics *result =
      m_kernel_api_registry.lookup(normalizeAPIName(name));
  m_kernel_semantics_cache[name] = result;
  return result;
}

bool ThreadAPI::isKernelOperation(const CallBase *call,
                                  kernel::OperationKind kind) const {
  if (call == nullptr) {
    return false;
  }
  const kernel::LinuxKernelAPISemantics *semantics =
      lookupKernelSemantics(getCalledAPIName(call));
  return semantics != nullptr && semantics->operation == kind;
}

ThreadAPI::TD_TYPE ThreadAPI::getKernelType(StringRef name) const {
  const kernel::LinuxKernelAPISemantics *semantics =
      lookupKernelSemantics(name);
  if (semantics == nullptr) {
    return TD_DUMMY;
  }

  using kernel::LockKind;
  using kernel::LockMode;
  using kernel::OperationKind;
  switch (semantics->operation) {
  case OperationKind::LOCK_INIT:
    switch (semantics->lock_kind) {
    case LockKind::SPINLOCK:
      return TD_KERNEL_SPIN_LOCK_INIT;
    case LockKind::MUTEX:
      return TD_KERNEL_MUTEX_INIT;
    case LockKind::SEMAPHORE:
      return TD_KERNEL_SEMA_INIT;
    case LockKind::RW_SEMAPHORE:
      return TD_KERNEL_INIT_RWSEM;
    default:
      return TD_DUMMY;
    }
  case OperationKind::LOCK_ACQUIRE:
  case OperationKind::LOCK_TRY:
    switch (semantics->lock_kind) {
    case LockKind::SPINLOCK:
      return semantics->operation == OperationKind::LOCK_TRY
                 ? TD_KERNEL_SPIN_TRYLOCK
                 : TD_KERNEL_SPIN_LOCK;
    case LockKind::MUTEX:
      return semantics->operation == OperationKind::LOCK_TRY
                 ? TD_KERNEL_MUTEX_TRYLOCK
                 : TD_KERNEL_MUTEX_LOCK;
    case LockKind::SEMAPHORE:
      return TD_KERNEL_DOWN;
    case LockKind::RWLOCK:
      return semantics->lock_mode == LockMode::SHARED ? TD_KERNEL_READ_LOCK
                                                       : TD_KERNEL_WRITE_LOCK;
    case LockKind::RW_SEMAPHORE:
      return semantics->lock_mode == LockMode::SHARED ? TD_KERNEL_DOWN_READ
                                                       : TD_KERNEL_DOWN_WRITE;
    default:
      return TD_DUMMY;
    }
  case OperationKind::LOCK_RELEASE:
    switch (semantics->lock_kind) {
    case LockKind::SPINLOCK:
      return TD_KERNEL_SPIN_UNLOCK;
    case LockKind::MUTEX:
      return TD_KERNEL_MUTEX_UNLOCK;
    case LockKind::SEMAPHORE:
      return TD_KERNEL_UP;
    case LockKind::RWLOCK:
      return semantics->lock_mode == LockMode::SHARED
                 ? TD_KERNEL_READ_UNLOCK
                 : TD_KERNEL_WRITE_UNLOCK;
    case LockKind::RW_SEMAPHORE:
      return semantics->lock_mode == LockMode::SHARED ? TD_KERNEL_UP_READ
                                                       : TD_KERNEL_UP_WRITE;
    default:
      return TD_DUMMY;
    }
  case OperationKind::RCU_READ_LOCK:
    return TD_KERNEL_RCU_READ_LOCK;
  case OperationKind::RCU_READ_UNLOCK:
    return TD_KERNEL_RCU_READ_UNLOCK;
  case OperationKind::RCU_SYNC:
    return TD_KERNEL_SYNCHRONIZE_RCU;
  case OperationKind::RCU_CALL:
    return TD_KERNEL_CALL_RCU;
  case OperationKind::RCU_DEREFERENCE:
    return TD_KERNEL_RCU_DEREFERENCE;
  case OperationKind::RCU_ASSIGN:
    return TD_KERNEL_RCU_ASSIGN_POINTER;
  case OperationKind::SEQLOCK_INIT:
    return TD_KERNEL_SEQLOCK_INIT;
  case OperationKind::SEQ_READ_BEGIN:
    return TD_KERNEL_READ_SEQBEGIN;
  case OperationKind::SEQ_READ_RETRY:
    return TD_KERNEL_READ_SEQRETRY;
  case OperationKind::SEQ_WRITE_LOCK:
    return TD_KERNEL_WRITE_SEQLOCK;
  case OperationKind::SEQ_WRITE_UNLOCK:
    return TD_KERNEL_WRITE_SEQUNLOCK;
  case OperationKind::COMPLETION_INIT:
  case OperationKind::COMPLETION_REINIT:
    return TD_KERNEL_INIT_COMPLETION;
  case OperationKind::COMPLETION_WAIT:
    return TD_KERNEL_WAIT_FOR_COMPLETION;
  case OperationKind::COMPLETION_SIGNAL:
    return semantics->completion_signal == kernel::CompletionSignalKind::ALL
               ? TD_KERNEL_COMPLETE_ALL
               : TD_KERNEL_COMPLETE;
  case OperationKind::WAITQUEUE_INIT:
    return TD_KERNEL_INIT_WAITQUEUE_HEAD;
  case OperationKind::WAIT_EVENT:
    return TD_KERNEL_WAIT_EVENT;
  case OperationKind::WAKE_UP:
    return TD_KERNEL_WAKE_UP;
  case OperationKind::PREPARE_WAIT:
    return TD_KERNEL_PREPARE_TO_WAIT;
  case OperationKind::FINISH_WAIT:
    return TD_KERNEL_FINISH_WAIT;
  case OperationKind::MEMORY_BARRIER:
    return TD_KERNEL_MEMORY_BARRIER;
  case OperationKind::ATOMIC_READ:
    return TD_KERNEL_ATOMIC_READ;
  case OperationKind::ATOMIC_WRITE:
    return TD_KERNEL_ATOMIC_WRITE;
  case OperationKind::ATOMIC_RMW:
    return TD_KERNEL_ATOMIC_RMW;
  case OperationKind::KTHREAD_START:
    return TD_FORK;
  default:
    return TD_DUMMY;
  }
}

const CallBase *
ThreadAPI::getKernelThreadCreateCall(const Instruction *inst) const {
  const CallBase *wake = getLLVMCallSite(inst);
  if (!wake || wake->arg_size() < 1)
    return nullptr;
  const Value *task = wake->getArgOperand(0)->stripPointerCasts();
  const auto *create = dyn_cast<CallBase>(task);
  if (!create)
    return nullptr;
  const Function *callee = getCallee(create);
  const kernel::LinuxKernelAPISemantics *semantics =
      callee ? lookupKernelSemantics(callee->getName()) : nullptr;
  return semantics != nullptr &&
                 semantics->operation == kernel::OperationKind::KTHREAD_CREATE
             ? create
             : nullptr;
}

const Value *
ThreadAPI::getConditionVariableWaitMutex(const Instruction *inst) const {
  const CallBase *cb = getLLVMCallSite(inst);
  if (!cb || cb->arg_size() < 2) {
    return nullptr;
  }

  const Function *callee = getCallee(inst);
  const Value *lock_or_mutex = cb->getArgOperand(1);
  if (!lock_or_mutex) {
    return lock_or_mutex;
  }

  const Value *lock_object = lock_or_mutex->stripPointerCasts();
  const Function *parent = inst ? inst->getFunction() : nullptr;
  if (!lock_object || !parent || parent->isDeclaration()) {
    return lock_or_mutex;
  }

  const Value *resolved_mutex = nullptr;
  auto recordCandidate = [&](const Value *candidate) -> bool {
    candidate = candidate ? candidate->stripPointerCasts() : nullptr;
    if (!candidate) {
      return false;
    }
    if (!resolved_mutex) {
      resolved_mutex = candidate;
      return true;
    }
    return resolved_mutex == candidate;
  };

  for (const Instruction &cursor : instructions(*parent)) {
    if (&cursor == inst) {
      break;
    }

    const auto *candidate_call = dyn_cast<CallBase>(&cursor);
    if (!candidate_call) {
      continue;
    }

    const Function *candidate_callee = getCallee(candidate_call);
    if (!candidate_callee) {
      continue;
    }

    TD_TYPE type = getType(candidate_call);
    if (type != TD_LOCK_GUARD_CTOR && type != TD_UNIQUE_LOCK_CTOR &&
        type != TD_SHARED_LOCK_CTOR) {
      continue;
    }

    if (candidate_call->arg_size() < 2) {
      continue;
    }

    const Value *wrapper_object =
        candidate_call->getArgOperand(0)->stripPointerCasts();
    if (wrapper_object != lock_object) {
      continue;
    }

    if (!recordCandidate(candidate_call->getArgOperand(1))) {
      return lock_or_mutex;
    }
  }

  return resolved_mutex ? resolved_mutex : lock_or_mutex;
}

SmallVector<const Value *, 4>
ThreadAPI::getCppWrapperUnderlyingLocks(const Instruction *inst) const {
  SmallVector<const Value *, 4> result;
  const auto *cb = getLLVMCallSite(inst);
  if (!cb || cb->arg_size() == 0)
    return result;

  TD_TYPE type = getType(cb);
  if (type == TD_LOCK_GUARD_CTOR || type == TD_UNIQUE_LOCK_CTOR ||
      type == TD_SCOPED_LOCK_CTOR || type == TD_SHARED_LOCK_CTOR) {
    for (const Value *lock :
         RAIILock::RAIILockTracker::extractUnderlyingLocks(cb)) {
      if (lock)
        result.push_back(lock->stripPointerCasts());
    }
    return result;
  }

  const Value *wrapper = cb->getArgOperand(0)->stripPointerCasts();
  const Function *parent = inst->getFunction();
  if (!wrapper || !parent || parent->isDeclaration())
    return result;

  using WrapperLocks = SmallVector<const Value *, 4>;
  DenseMap<const Value *, WrapperLocks> states;

  for (const Instruction &cursor : instructions(*parent)) {
    if (&cursor == inst)
      break;
    const auto *candidate = dyn_cast<CallBase>(&cursor);
    if (!candidate)
      continue;
    TD_TYPE candidate_type = getType(candidate);
    if (candidate->arg_size() >= 2 &&
        candidate_type == TD_CPP_LOCK_MOVE_CTOR) {
      const Value *destination =
          candidate->getArgOperand(0)->stripPointerCasts();
      const Value *source = candidate->getArgOperand(1)->stripPointerCasts();
      states[destination] = states.lookup(source);
      states[source].clear();
      continue;
    }
    if (candidate->arg_size() >= 2 &&
        candidate_type == TD_CPP_LOCK_MOVE_ASSIGN) {
      const Value *destination =
          candidate->getArgOperand(0)->stripPointerCasts();
      const Value *source = candidate->getArgOperand(1)->stripPointerCasts();
      states[destination] = states.lookup(source);
      states[source].clear();
      continue;
    }
    if (candidate_type == TD_CPP_LOCK_RELEASE &&
        candidate->getArgOperand(0)->stripPointerCasts() == wrapper) {
        states[wrapper].clear();
      continue;
    }
    if (candidate->arg_size() >= 2 && candidate_type == TD_CPP_LOCK_SWAP) {
      const Value *lhs = candidate->getArgOperand(0)->stripPointerCasts();
      const Value *rhs = candidate->getArgOperand(1)->stripPointerCasts();
      WrapperLocks lhs_locks = states.lookup(lhs);
      states[lhs] = states.lookup(rhs);
      states[rhs] = std::move(lhs_locks);
      continue;
    }
    if (candidate_type != TD_LOCK_GUARD_CTOR &&
        candidate_type != TD_UNIQUE_LOCK_CTOR &&
        candidate_type != TD_SCOPED_LOCK_CTOR &&
        candidate_type != TD_SHARED_LOCK_CTOR)
      continue;
    const Value *constructed =
        RAIILock::RAIILockTracker::findLockObjectForConstructor(candidate);
    if (!constructed)
      continue;
    WrapperLocks &constructed_locks = states[constructed];
    constructed_locks.clear();
    for (const Value *lock :
         RAIILock::RAIILockTracker::extractUnderlyingLocks(candidate)) {
      if (lock)
        constructed_locks.push_back(lock->stripPointerCasts());
    }
  }
  result = states.lookup(wrapper);
  return result;
}

bool ThreadAPI::cppWrapperDestructorDefinitelyReleases(
    const Instruction *inst) const {
  const auto *dtor = getLLVMCallSite(inst);
  if (!dtor || dtor->arg_size() == 0 || !inst->getFunction())
    return false;
  const TD_TYPE dtor_type = getType(dtor);
  if (dtor_type != TD_UNIQUE_LOCK_DTOR && dtor_type != TD_SHARED_LOCK_DTOR)
    return false;
  const Value *wrapper = dtor->getArgOperand(0)->stripPointerCasts();
  DenseMap<const Value *, bool> owns;

  for (const Instruction &cursor : instructions(*inst->getFunction())) {
    if (&cursor == inst)
      break;
    const auto *call = dyn_cast<CallBase>(&cursor);
    if (!call || call->arg_size() == 0)
      continue;
    TD_TYPE type = getType(call);
    if (type == TD_DUMMY)
      continue;
    const Function *callee = getCallee(call);
    const Value *first = call->getArgOperand(0)->stripPointerCasts();
    if ((type == TD_CPP_LOCK_MOVE_CTOR ||
         type == TD_CPP_LOCK_MOVE_ASSIGN) &&
        call->arg_size() >= 2) {
      const Value *source = call->getArgOperand(1)->stripPointerCasts();
      owns[first] = owns.lookup(source);
      owns[source] = false;
      continue;
    }
    if (type == TD_CPP_LOCK_RELEASE) {
      owns[first] = false;
      continue;
    }
    if (type == TD_CPP_LOCK_SWAP && call->arg_size() >= 2) {
      const Value *second = call->getArgOperand(1)->stripPointerCasts();
      const bool first_owns = owns.lookup(first);
      owns[first] = owns.lookup(second);
      owns[second] = first_owns;
      continue;
    }
    if (type == TD_UNIQUE_LOCK_CTOR || type == TD_SHARED_LOCK_CTOR) {
      if (callee && CppThreadingModel::isDeferLockConstructor(callee->getName()))
        owns[first] = false;
      else if (callee &&
               CppThreadingModel::isTryToLockConstructor(callee->getName()))
        owns[first] = false;
      else
        owns[first] = true; // immediate and adopt both own on return
    } else if (type == TD_UNIQUE_LOCK_LOCK) {
      owns[first] = true;
    } else if (type == TD_CPP_LOCK_TRY) {
      owns[first] = false;
    } else if (type == TD_UNIQUE_LOCK_UNLOCK) {
      owns[first] = false;
    }
  }
  return owns.lookup(wrapper);
}

const llvm::Value *
ThreadAPI::getCppWrapperUnderlyingLock(const Instruction *inst) const {
  const auto *cb = getLLVMCallSite(inst);
  if (!cb || cb->arg_size() == 0) {
    return nullptr;
  }

  const TD_TYPE type = getType(cb);
  switch (type) {
  case TD_LOCK_GUARD_CTOR:
  case TD_UNIQUE_LOCK_CTOR:
  case TD_SCOPED_LOCK_CTOR:
  case TD_SHARED_LOCK_CTOR: {
    std::vector<const Value *> locks =
        RAIILock::RAIILockTracker::extractUnderlyingLocks(cb);
    if (locks.size() == 1 && locks.front()) {
      return locks.front()->stripPointerCasts();
    }
    return nullptr;
  }
  case TD_LOCK_GUARD_DTOR:
  case TD_UNIQUE_LOCK_DTOR:
  case TD_UNIQUE_LOCK_LOCK:
  case TD_UNIQUE_LOCK_UNLOCK:
  case TD_CPP_LOCK_TRY:
  case TD_SHARED_RDLOCK:
  case TD_SHARED_UNLOCK:
  case TD_SCOPED_LOCK_DTOR:
  case TD_SHARED_LOCK_DTOR:
  case TD_CPP_LOCK_MOVE_CTOR:
  case TD_CPP_LOCK_MOVE_ASSIGN:
  case TD_CPP_LOCK_SWAP:
  case TD_CPP_LOCK_RELEASE: {
    SmallVector<const Value *, 4> locks = getCppWrapperUnderlyingLocks(inst);
    return locks.size() == 1 ? locks.front() : nullptr;
  }
  default:
    return nullptr;
  }

  const Value *wrapper = cb->getArgOperand(0)->stripPointerCasts();
  const Function *parent = inst ? inst->getFunction() : nullptr;
  if (!wrapper || !parent || parent->isDeclaration()) {
    return nullptr;
  }

  const Value *resolved_lock = nullptr;
  auto recordCandidate = [&](const Value *candidate) -> bool {
    candidate = candidate ? candidate->stripPointerCasts() : nullptr;
    if (!candidate) {
      return false;
    }
    if (!resolved_lock) {
      resolved_lock = candidate;
      return true;
    }
    return resolved_lock == candidate;
  };

  for (const Instruction &cursor : instructions(*parent)) {
    if (&cursor == inst)
      break;
    const auto *candidate_call = dyn_cast<CallBase>(&cursor);
    if (!candidate_call) {
      continue;
    }

    const TD_TYPE candidate_type = getType(candidate_call);
    if (candidate_type != TD_LOCK_GUARD_CTOR &&
        candidate_type != TD_UNIQUE_LOCK_CTOR &&
        candidate_type != TD_SCOPED_LOCK_CTOR &&
        candidate_type != TD_SHARED_LOCK_CTOR) {
      continue;
    }

    const Value *candidate_wrapper =
        RAIILock::RAIILockTracker::findLockObjectForConstructor(candidate_call);
    if (candidate_wrapper != wrapper) {
      continue;
    }

    std::vector<const Value *> locks =
        RAIILock::RAIILockTracker::extractUnderlyingLocks(candidate_call);
    if (locks.size() != 1 || !recordCandidate(locks.front())) {
      return nullptr;
    }
  }

  return resolved_lock;
}

ThreadAPI::TD_TYPE ThreadAPI::stringToType(StringRef s) {
  static const auto *type_map =
      []() -> std::unordered_map<std::string, ThreadAPI::TD_TYPE> * {
    auto *map = new std::unordered_map<std::string, ThreadAPI::TD_TYPE>();
    for (int raw = static_cast<int>(ThreadAPI::TD_DUMMY);
         raw <= static_cast<int>(ThreadAPI::TD_KERNEL_MEMORY_BARRIER); ++raw) {
      ThreadAPI::TD_TYPE type = static_cast<ThreadAPI::TD_TYPE>(raw);
      const char *name = ThreadAPI::tdTypeToString(type);
      if (name && name[0] != '<') {
        (*map)[name] = type;
      }
    }
    return map;
  }();

  auto it = type_map->find(s.str());
  return it != type_map->end() ? it->second : ThreadAPI::TD_DUMMY;
}

ThreadAPI::ForkArgIndices
ThreadAPI::getForkArgIndices(const Function *F) const {
  if (!F)
    return ForkArgIndices{};
  for (const std::string &name : getConfiguredLookupNames(F->getName())) {
    auto it = m_fork_args.find(name);
    if (it != m_fork_args.end())
      return it->second;
  }
  return ForkArgIndices{};
}

ThreadAPI::ForkArgIndices
ThreadAPI::getForkArgIndices(const CallBase *cb) const {
  if (!cb)
    return ForkArgIndices{};
  if (const GlobalValue *called = getCalledGlobal(cb)) {
    for (const std::string &name : getConfiguredLookupNames(called->getName())) {
      auto it = m_fork_args.find(name);
      if (it != m_fork_args.end())
        return it->second;
    }
  }
  return getForkArgIndices(getCallee(cb));
}

ThreadAPI::JoinArgIndices
ThreadAPI::getJoinArgIndices(const Function *F) const {
  if (!F)
    return JoinArgIndices{};
  for (const std::string &name : getConfiguredLookupNames(F->getName())) {
    auto it = m_join_args.find(name);
    if (it != m_join_args.end())
      return it->second;
  }
  return JoinArgIndices{};
}

ThreadAPI::JoinArgIndices
ThreadAPI::getJoinArgIndices(const CallBase *cb) const {
  if (!cb)
    return JoinArgIndices{};
  if (const GlobalValue *called = getCalledGlobal(cb)) {
    for (const std::string &name : getConfiguredLookupNames(called->getName())) {
      auto it = m_join_args.find(name);
      if (it != m_join_args.end())
        return it->second;
    }
  }
  return getJoinArgIndices(getCallee(cb));
}

void ThreadAPI::loadConfig(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::stringstream ss(line);
    std::string name, typeStr;
    if (ss >> name >> typeStr) {
      TD_TYPE type = ThreadAPI::stringToType(typeStr);
      if (type != TD_DUMMY) {
        addEntry(name, type);
        const std::string normalized_name = normalizeConfiguredAPIName(name);
        if (type == TD_FORK) {
          auto parseField = [](const std::string &token,
                               unsigned fallback) {
            if (token == "return")
              return ForkArgIndices::ReturnValue;
            if (token == "none" || token == "-")
              return ForkArgIndices::NoArgument;
            unsigned value = fallback;
            std::stringstream token_stream(token);
            token_stream >> value;
            return token_stream.fail() ? fallback : value;
          };
          std::string t, s, a;
          if (ss >> t >> s >> a)
            m_fork_args[normalized_name] =
                ForkArgIndices{parseField(t, 0), parseField(s, 2),
                               parseField(a, 3)};
        } else if (type == TD_JOIN) {
          auto parseField = [](const std::string &token,
                               unsigned fallback) {
            if (token == "return")
              return JoinArgIndices::ReturnValue;
            if (token == "none" || token == "-")
              return JoinArgIndices::NoArgument;
            unsigned value = fallback;
            std::stringstream token_stream(token);
            token_stream >> value;
            return token_stream.fail() ? fallback : value;
          };
          std::string t, r;
          if (ss >> t >> r)
            m_join_args[normalized_name] =
                JoinArgIndices{parseField(t, 0), parseField(r, 1)};
        }
      }
    }
  }
}

void ThreadAPI::loadSemanticConfig(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::stringstream ss(line);
    std::string name;
    std::string typeStr;
    if (!(ss >> name >> typeStr)) {
      continue;
    }

    TD_TYPE type = ThreadAPI::stringToType(typeStr);
    if (type == TD_DUMMY) {
      continue;
    }

    APIDescription description;
    description.type = type;
    description.from_config = true;
    description.library = inferLibrary(type);
    MatchKind match_kind = MatchKind::Exact;

    std::string token;
    while (ss >> token) {
      size_t eq = token.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      StringRef key(token.data(), eq);
      StringRef value(token.data() + eq + 1, token.size() - eq - 1);
      if (key.equals("library")) {
        description.library = parseRuntimeLibrary(value);
      } else if (key.equals("semantic")) {
        description.semantic_tag = value.str();
      } else if (key.equals("match")) {
        std::string lowered = value.lower();
        match_kind = lowered == "prefix" ? MatchKind::Prefix : MatchKind::Exact;
      } else if (key.equals("traits")) {
        SmallVector<StringRef, 8> entries;
        value.split(entries, ',', -1, false);
        for (StringRef entry : entries) {
          if (!entry.empty()) {
            description.traits.push_back(entry.trim().str());
          }
        }
      } else if (key.equals("success")) {
        if (value.equals_insensitive("zero"))
          description.success = TryLockSuccess::Zero;
        else if (value.equals_insensitive("nonzero"))
          description.success = TryLockSuccess::NonZero;
      } else if (key.equals("conditional")) {
        description.conditional_acquire =
            value.equals_insensitive("true") || value.equals("1") ||
            value.equals_insensitive("yes");
      }
    }

    if (match_kind == MatchKind::Exact) {
      addEntry(name, type);
      addDescription(name, description);
    }
    addMatchRule(name, match_kind, description);
  }
}

ThreadAPI::RuntimeLibrary ThreadAPI::inferLibrary(TD_TYPE type) const {
  switch (type) {
  case TD_FORK:
  case TD_JOIN:
  case TD_DETACH:
  case TD_ACQUIRE:
  case TD_TRY_ACQUIRE:
  case TD_RWLOCK_RDLOCK:
  case TD_RWLOCK_WRLOCK:
  case TD_RELEASE:
  case TD_EXIT:
  case TD_CANCEL:
  case TD_COND_WAIT:
  case TD_COND_SIGNAL:
  case TD_COND_BROADCAST:
  case TD_MUTEX_INI:
  case TD_MUTEX_DESTROY:
  case TD_CONDVAR_INI:
  case TD_CONDVAR_DESTROY:
  case TD_BAR_INIT:
  case TD_BAR_WAIT:
    return RuntimeLibrary::PThread;
  case HARE_PAR_FOR:
    return RuntimeLibrary::Hare;
  case TD_SHARED_RDLOCK:
  case TD_SHARED_WRLOCK:
  case TD_SHARED_UNLOCK:
  case TD_CALL_ONCE:
  case TD_FUTURE_GET:
  case TD_FUTURE_WAIT:
  case TD_PROMISE_SET:
  case TD_ASYNC:
  case TD_LOCK_GUARD_CTOR:
  case TD_LOCK_GUARD_DTOR:
  case TD_UNIQUE_LOCK_CTOR:
  case TD_UNIQUE_LOCK_DTOR:
  case TD_UNIQUE_LOCK_LOCK:
  case TD_UNIQUE_LOCK_UNLOCK:
  case TD_SCOPED_LOCK_CTOR:
  case TD_SCOPED_LOCK_DTOR:
  case TD_SHARED_LOCK_CTOR:
  case TD_SHARED_LOCK_DTOR:
  case TD_CPP_LOCK_TRY:
  case TD_JTHREAD_FORK:
  case TD_JTHREAD_JOIN:
  case TD_JTHREAD_DTOR:
  case TD_ATOMIC_WAIT:
  case TD_ATOMIC_NOTIFY_ONE:
  case TD_ATOMIC_NOTIFY_ALL:
  case TD_LATCH_COUNT_DOWN:
  case TD_LATCH_WAIT:
  case TD_LATCH_ARRIVE_WAIT:
  case TD_BARRIER_ARRIVE_WAIT:
  case TD_BARRIER_ARRIVE:
  case TD_BARRIER_WAIT_CPP20:
  case TD_SEMAPHORE_ACQUIRE:
  case TD_SEMAPHORE_RELEASE:
  case TD_SEMAPHORE_TRY_ACQUIRE:
    return RuntimeLibrary::Cpp;
  case TD_CUDA_KERNEL_LAUNCH:
  case TD_CUDA_DEVICE_SYNC:
  case TD_CUDA_BARRIER:
  case TD_CUDA_WARP_BARRIER:
  case TD_CUDA_MEMORY_BARRIER:
  case TD_CUDA_ATOMIC:
  case TD_CUDA_MEMCPY:
  case TD_CUDA_MEMSET:
  case TD_CUDA_MALLOC:
  case TD_CUDA_FREE:
  case TD_CUDA_UNIFIED_MEMORY:
  case TD_CUDA_STREAM:
  case TD_CUDA_EVENT:
  case TD_CUDA_TEXTURE:
  case TD_CUDA_SURFACE:
  case TD_CUDA_DEVICE_MGMT:
  case TD_CUDA_ERROR:
    return RuntimeLibrary::CUDA;
  case TD_KERNEL_ATOMIC_READ:
  case TD_KERNEL_ATOMIC_WRITE:
  case TD_KERNEL_ATOMIC_RMW:
  case TD_KERNEL_SPIN_LOCK_INIT:
  case TD_KERNEL_SPIN_LOCK:
  case TD_KERNEL_SPIN_UNLOCK:
  case TD_KERNEL_SPIN_TRYLOCK:
  case TD_KERNEL_MUTEX_INIT:
  case TD_KERNEL_MUTEX_LOCK:
  case TD_KERNEL_MUTEX_UNLOCK:
  case TD_KERNEL_MUTEX_TRYLOCK:
  case TD_KERNEL_SEMA_INIT:
  case TD_KERNEL_DOWN:
  case TD_KERNEL_UP:
  case TD_KERNEL_READ_LOCK:
  case TD_KERNEL_READ_UNLOCK:
  case TD_KERNEL_WRITE_LOCK:
  case TD_KERNEL_WRITE_UNLOCK:
  case TD_KERNEL_DOWN_READ:
  case TD_KERNEL_UP_READ:
  case TD_KERNEL_DOWN_WRITE:
  case TD_KERNEL_UP_WRITE:
  case TD_KERNEL_INIT_RWSEM:
  case TD_KERNEL_RCU_READ_LOCK:
  case TD_KERNEL_RCU_READ_UNLOCK:
  case TD_KERNEL_SYNCHRONIZE_RCU:
  case TD_KERNEL_CALL_RCU:
  case TD_KERNEL_RCU_DEREFERENCE:
  case TD_KERNEL_RCU_ASSIGN_POINTER:
  case TD_KERNEL_SEQLOCK_INIT:
  case TD_KERNEL_READ_SEQBEGIN:
  case TD_KERNEL_READ_SEQRETRY:
  case TD_KERNEL_WRITE_SEQLOCK:
  case TD_KERNEL_WRITE_SEQUNLOCK:
  case TD_KERNEL_INIT_COMPLETION:
  case TD_KERNEL_WAIT_FOR_COMPLETION:
  case TD_KERNEL_COMPLETE:
  case TD_KERNEL_COMPLETE_ALL:
  case TD_KERNEL_INIT_WAITQUEUE_HEAD:
  case TD_KERNEL_WAIT_EVENT:
  case TD_KERNEL_WAKE_UP:
  case TD_KERNEL_PREPARE_TO_WAIT:
  case TD_KERNEL_FINISH_WAIT:
  case TD_KERNEL_MEMORY_BARRIER:
    return RuntimeLibrary::LinuxKernel;
  case TD_OMP_TASK:
  case TD_OMP_TASKWAIT:
  case TD_OMP_TASKWAIT_DEPS:
  case TD_OMP_TASKYIELD:
  case TD_OMP_TASKGROUP_START:
  case TD_OMP_TASKGROUP_END:
  case TD_OMP_TASK_WITH_DEPS:
  case TD_OMP_TASKLOOP:
  case TD_OMP_TASK_COMPLETE:
  case TD_OMP_SINGLE_START:
  case TD_OMP_SINGLE_END:
  case TD_OMP_MASTER_START:
  case TD_OMP_MASTER_END:
  case TD_OMP_ORDERED_START:
  case TD_OMP_ORDERED_END:
  case TD_OMP_REDUCE_START:
  case TD_OMP_REDUCE_END:
  case TD_OMP_REDUCE_NOWAIT_START:
  case TD_OMP_REDUCE_NOWAIT_END:
  case TD_OMP_FOR_STATIC_INIT:
  case TD_OMP_FOR_STATIC_FINI:
  case TD_OMP_FOR_DISPATCH_INIT:
  case TD_OMP_FOR_DISPATCH_NEXT:
  case TD_OMP_FOR_DISPATCH_FINI:
  case TD_OMP_SECTIONS_INIT:
  case TD_OMP_SECTIONS_NEXT:
  case TD_OMP_SECTIONS_END:
  case TD_OMP_ATOMIC_START:
  case TD_OMP_ATOMIC_END:
  case TD_OMP_FLUSH:
  case TD_OMP_CRITICAL_START:
  case TD_OMP_CRITICAL_END:
  case TD_OMP_PARALLEL_START:
  case TD_OMP_TARGET:
  case TD_OMP_TARGET_DATA_BEGIN:
  case TD_OMP_TARGET_DATA_END:
  case TD_OMP_TARGET_DATA_UPDATE:
  case TD_OMP_TEAMS:
  case TD_OMP_TEAMS_HOST:
  case TD_OMP_TEAMS_DISTRIBUTE:
  case TD_OMP_DISTRIBUTE:
  case TD_OMP_DISTRIBUTE_STATIC:
  case TD_OMP_DISTRIBUTE_DYNAMIC:
  case TD_OMP_DISTRIBUTE_GUIDANCE:
  case TD_OMP_LOOP_STATIC_INIT:
  case TD_OMP_LOOP_DYNAMIC_INIT:
  case TD_OMP_LOOP_GUIDANCE_INIT:
  case TD_OMP_AFFINITY:
  case TD_OMP_SCOPE_START:
  case TD_OMP_SCOPE_END:
  case TD_OMP_TASKLOOP_SIMD:
  case TD_OMP_TASKLOOP_FINI:
  case TD_OMP_INTEROP_INIT:
  case TD_OMP_INTEROP_FINI:
  case TD_OMP_DOACROSS_INIT:
  case TD_OMP_DOACROSS_WAIT:
  case TD_OMP_DOACROSS_SUBMIT:
    return RuntimeLibrary::OpenMP;
  case TD_MPI_INIT:
  case TD_MPI_FINALIZE:
  case TD_MPI_SESSION_INIT:
  case TD_MPI_SESSION_FINALIZE:
  case TD_MPI_SESSION_GET_INFO:
  case TD_MPI_SESSION_GET_NUM_ERRCODES:
  case TD_MPI_SESSION_GET_ERRHANDLER:
  case TD_MPI_SESSION_SET_ERRHANDLER:
  case TD_MPI_ERRHANDLER_CREATE:
  case TD_MPI_ERRHANDLER_FREE:
  case TD_MPI_COMM_GET_ERRHANDLER:
  case TD_MPI_COMM_SET_ERRHANDLER:
  case TD_MPI_COMM_CALL_ERRHANDLER:
  case TD_MPI_WIN_GET_ERRHANDLER:
  case TD_MPI_WIN_SET_ERRHANDLER:
  case TD_MPI_FILE_GET_ERRHANDLER:
  case TD_MPI_FILE_SET_ERRHANDLER:
  case TD_MPI_ERROR_CLASS:
  case TD_MPI_ERROR_STRING:
  case TD_MPI_INFO_CREATE:
  case TD_MPI_INFO_DUP:
  case TD_MPI_INFO_FREE:
  case TD_MPI_INFO_GET:
  case TD_MPI_INFO_GET_VALUELEN:
  case TD_MPI_INFO_GET_NKEYS:
  case TD_MPI_INFO_GET_NTHKEY:
  case TD_MPI_INFO_GET_KEYVAL:
  case TD_MPI_INFO_SET:
  case TD_MPI_INFO_DELETE:
  case TD_MPI_INFO_C2F:
  case TD_MPI_INFO_CREATE_ENV:
  case TD_MPI_INFO_FREE_ENV:
  case TD_MPI_GET_COUNT:
  case TD_MPI_GET_ELEMENTS:
  case TD_MPI_GET_ELEMENTS_X:
  case TD_MPI_STATUS_SIZE:
  case TD_MPI_STATUS_SET_ELEMENTS:
  case TD_MPI_STATUS_SET_ELEMENTS_X:
  case TD_MPI_SEND:
  case TD_MPI_RECV:
  case TD_MPI_SENDRECV:
  case TD_MPI_PROBE:
  case TD_MPI_ISEND:
  case TD_MPI_IRECV:
  case TD_MPI_IPROBE:
  case TD_MPI_MPROBE:
  case TD_MPI_IMPROBE:
  case TD_MPI_IMRECV:
  case TD_MPI_MRECV:
  case TD_MPI_PERSISTENT_SEND_INIT:
  case TD_MPI_PERSISTENT_RECV_INIT:
  case TD_MPI_REQUEST_START:
  case TD_MPI_WAIT:
  case TD_MPI_WAITALL:
  case TD_MPI_WAITANY:
  case TD_MPI_WAITSOME:
  case TD_MPI_TEST:
  case TD_MPI_TESTALL:
  case TD_MPI_TESTANY:
  case TD_MPI_TESTSOME:
  case TD_MPI_BARRIER:
  case TD_MPI_BCAST:
  case TD_MPI_SCATTER:
  case TD_MPI_GATHER:
  case TD_MPI_ALLGATHER:
  case TD_MPI_ALLTOALL:
  case TD_MPI_REDUCE:
  case TD_MPI_ALLREDUCE:
  case TD_MPI_REDUCE_SCATTER:
  case TD_MPI_SCAN:
  case TD_MPI_WIN_CREATE:
  case TD_MPI_WIN_FREE:
  case TD_MPI_PUT:
  case TD_MPI_GET:
  case TD_MPI_ACCUMULATE:
  case TD_MPI_WIN_FENCE:
  case TD_MPI_WIN_LOCK:
  case TD_MPI_WIN_UNLOCK:
  case TD_MPI_WIN_FLUSH:
  case TD_MPI_WIN_SYNC:
  case TD_MPI_WIN_POST:
  case TD_MPI_WIN_START:
  case TD_MPI_WIN_COMPLETE:
  case TD_MPI_WIN_WAIT:
  case TD_MPI_WIN_TEST:
  case TD_MPI_COMM_DUP:
  case TD_MPI_COMM_SPLIT:
  case TD_MPI_COMM_CREATE:
  case TD_MPI_COMM_FREE:
  case TD_MPI_REQUEST_FREE:
  case TD_MPI_CANCEL:
  case TD_MPI_TYPE_CONTIGUOUS:
  case TD_MPI_TYPE_VECTOR:
  case TD_MPI_TYPE_HVECTOR:
  case TD_MPI_TYPE_INDEXED:
  case TD_MPI_TYPE_HINDEXED:
  case TD_MPI_TYPE_STRUCT:
  case TD_MPI_TYPE_CREATE_DLPACK:
  case TD_MPI_TYPE_CREATE_SUBARRAY:
  case TD_MPI_TYPE_CREATE_DARRAY:
  case TD_MPI_TYPE_CREATE_RESIZED:
  case TD_MPI_TYPE_CREATE_HINDEXED:
  case TD_MPI_TYPE_CREATE_HVECTOR:
  case TD_MPI_TYPE_GET_EXTENT:
  case TD_MPI_TYPE_GET_TRUE_EXTENT:
  case TD_MPI_TYPE_SIZE:
  case TD_MPI_TYPE_COMMIT:
  case TD_MPI_CART_CREATE:
  case TD_MPI_CART_DIMS_CREATE:
  case TD_MPI_CART_GET:
  case TD_MPI_CART_SHIFT:
  case TD_MPI_CART_COORDS:
  case TD_MPI_CART_RANK:
  case TD_MPI_CART_SUB:
  case TD_MPI_DIST_GRAPH_CREATE:
  case TD_MPI_DIST_GRAPH_CREATE_ADJACENT:
  case TD_MPI_DIST_GRAPH_NEIGHBORS:
  case TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT:
  case TD_MPI_GRAPH_CREATE:
  case TD_MPI_GRAPH_GET:
  case TD_MPI_GRAPH_NEIGHBORS:
  case TD_MPI_GRAPH_NEIGHBORS_COUNT:
  case TD_MPI_GRAPH_DIMS_GET:
  case TD_MPI_GRAPH_MAP:
    return RuntimeLibrary::MPI;
  default:
    return RuntimeLibrary::Unknown;
  }
}

namespace {

using Owner = ThreadAPI::SemanticLoweringOwner;
using LoweringKind = ThreadAPI::SemanticLoweringKind;
using LoweringInfo = ThreadAPI::SemanticLoweringInfo;
using TD = ThreadAPI::TD_TYPE;

constexpr uint32_t ownerMask(Owner owner) {
  return static_cast<uint32_t>(owner);
}

constexpr uint32_t ownerMask(Owner owner_a, Owner owner_b) {
  return ownerMask(owner_a) | ownerMask(owner_b);
}

bool isExplicitFallbackType(TD type) {
  switch (type) {
  case TD::TD_DUMMY:
  case TD::TD_OMP_ATOMIC_START:
  case TD::TD_OMP_ATOMIC_END:
  case TD::TD_OMP_TARGET_DATA_UPDATE:
  case TD::TD_OMP_CANCEL:
  case TD::TD_OMP_TEAMS:
  case TD::TD_OMP_TEAMS_HOST:
  case TD::TD_OMP_TEAMS_DISTRIBUTE:
  case TD::TD_OMP_DISTRIBUTE:
  case TD::TD_OMP_DISTRIBUTE_STATIC:
  case TD::TD_OMP_DISTRIBUTE_DYNAMIC:
  case TD::TD_OMP_DISTRIBUTE_GUIDANCE:
  case TD::TD_OMP_LOOP_STATIC_INIT:
  case TD::TD_OMP_LOOP_DYNAMIC_INIT:
  case TD::TD_OMP_LOOP_GUIDANCE_INIT:
  case TD::TD_OMP_AFFINITY:
  case TD::TD_OMP_SCOPE_START:
  case TD::TD_OMP_SCOPE_END:
  case TD::TD_OMP_TASKLOOP_SIMD:
  case TD::TD_OMP_TASKLOOP_FINI:
  case TD::TD_OMP_INTEROP_INIT:
  case TD::TD_OMP_INTEROP_FINI:
  case TD::TD_MPI_SESSION_GET_INFO:
  case TD::TD_MPI_SESSION_GET_NUM_ERRCODES:
  case TD::TD_MPI_SESSION_GET_ERRHANDLER:
  case TD::TD_MPI_SESSION_SET_ERRHANDLER:
  case TD::TD_MPI_ERRHANDLER_CREATE:
  case TD::TD_MPI_ERRHANDLER_FREE:
  case TD::TD_MPI_COMM_GET_ERRHANDLER:
  case TD::TD_MPI_COMM_SET_ERRHANDLER:
  case TD::TD_MPI_COMM_CALL_ERRHANDLER:
  case TD::TD_MPI_WIN_GET_ERRHANDLER:
  case TD::TD_MPI_WIN_SET_ERRHANDLER:
  case TD::TD_MPI_FILE_GET_ERRHANDLER:
  case TD::TD_MPI_FILE_SET_ERRHANDLER:
  case TD::TD_MPI_ERROR_CLASS:
  case TD::TD_MPI_ERROR_STRING:
  case TD::TD_MPI_INFO_CREATE:
  case TD::TD_MPI_INFO_DUP:
  case TD::TD_MPI_INFO_FREE:
  case TD::TD_MPI_INFO_GET:
  case TD::TD_MPI_INFO_GET_VALUELEN:
  case TD::TD_MPI_INFO_GET_NKEYS:
  case TD::TD_MPI_INFO_GET_NTHKEY:
  case TD::TD_MPI_INFO_GET_KEYVAL:
  case TD::TD_MPI_INFO_SET:
  case TD::TD_MPI_INFO_DELETE:
  case TD::TD_MPI_INFO_C2F:
  case TD::TD_MPI_INFO_CREATE_ENV:
  case TD::TD_MPI_INFO_FREE_ENV:
  case TD::TD_MPI_GET_COUNT:
  case TD::TD_MPI_GET_ELEMENTS:
  case TD::TD_MPI_GET_ELEMENTS_X:
  case TD::TD_MPI_STATUS_SIZE:
  case TD::TD_MPI_STATUS_SET_ELEMENTS:
  case TD::TD_MPI_STATUS_SET_ELEMENTS_X:
    return true;
  default:
    return false;
  }
}

bool isLockLikeType(TD type) {
  switch (type) {
  case TD::TD_ACQUIRE:
  case TD::TD_TRY_ACQUIRE:
  case TD::TD_RWLOCK_RDLOCK:
  case TD::TD_RWLOCK_WRLOCK:
  case TD::TD_RELEASE:
  case TD::TD_SHARED_RDLOCK:
  case TD::TD_SHARED_WRLOCK:
  case TD::TD_SHARED_UNLOCK:
  case TD::TD_LOCK_GUARD_CTOR:
  case TD::TD_LOCK_GUARD_DTOR:
  case TD::TD_UNIQUE_LOCK_CTOR:
  case TD::TD_UNIQUE_LOCK_DTOR:
  case TD::TD_UNIQUE_LOCK_LOCK:
  case TD::TD_UNIQUE_LOCK_UNLOCK:
  case TD::TD_SCOPED_LOCK_CTOR:
  case TD::TD_SCOPED_LOCK_DTOR:
  case TD::TD_SHARED_LOCK_CTOR:
  case TD::TD_SHARED_LOCK_DTOR:
  case TD::TD_CPP_LOCK_TRY:
  case TD::TD_OMP_ORDERED_START:
  case TD::TD_OMP_ORDERED_END:
  case TD::TD_KERNEL_SPIN_LOCK:
  case TD::TD_KERNEL_SPIN_UNLOCK:
  case TD::TD_KERNEL_SPIN_TRYLOCK:
  case TD::TD_KERNEL_MUTEX_LOCK:
  case TD::TD_KERNEL_MUTEX_UNLOCK:
  case TD::TD_KERNEL_MUTEX_TRYLOCK:
  case TD::TD_KERNEL_DOWN:
  case TD::TD_KERNEL_UP:
  case TD::TD_KERNEL_READ_LOCK:
  case TD::TD_KERNEL_READ_UNLOCK:
  case TD::TD_KERNEL_WRITE_LOCK:
  case TD::TD_KERNEL_WRITE_UNLOCK:
  case TD::TD_KERNEL_DOWN_READ:
  case TD::TD_KERNEL_UP_READ:
  case TD::TD_KERNEL_DOWN_WRITE:
  case TD::TD_KERNEL_UP_WRITE:
    return true;
  default:
    return false;
  }
}

bool isSemaphoreTypeForLowering(TD type) {
  return type == TD::TD_SEMAPHORE_ACQUIRE || type == TD::TD_SEMAPHORE_RELEASE ||
         type == TD::TD_SEMAPHORE_TRY_ACQUIRE;
}

bool isBarrierHBType(TD type) {
  switch (type) {
  case TD::TD_BAR_WAIT:
  case TD::TD_LATCH_COUNT_DOWN:
  case TD::TD_LATCH_WAIT:
  case TD::TD_LATCH_ARRIVE_WAIT:
  case TD::TD_BARRIER_ARRIVE_WAIT:
  case TD::TD_BARRIER_ARRIVE:
  case TD::TD_BARRIER_WAIT_CPP20:
  case TD::TD_CUDA_BARRIER:
  case TD::TD_CUDA_WARP_BARRIER:
  case TD::TD_CUDA_DEVICE_SYNC:
  case TD::TD_CUDA_MEMORY_BARRIER:
  case TD::TD_CALL_ONCE:
  case TD::TD_FUTURE_GET:
  case TD::TD_FUTURE_WAIT:
  case TD::TD_PROMISE_SET:
  case TD::TD_OMP_TASK:
  case TD::TD_OMP_TASKWAIT:
  case TD::TD_OMP_TASKWAIT_DEPS:
  case TD::TD_OMP_TASKGROUP_START:
  case TD::TD_OMP_TASKGROUP_END:
  case TD::TD_OMP_TASK_WITH_DEPS:
  case TD::TD_OMP_TASKLOOP:
  case TD::TD_OMP_TASK_COMPLETE:
  case TD::TD_OMP_REDUCE_START:
    return true;
  default:
    return false;
  }
}

bool isMHPThreadType(TD type) {
  switch (type) {
  case TD::TD_FORK:
  case TD::TD_JOIN:
  case TD::TD_DETACH:
  case TD::TD_EXIT:
  case TD::TD_CANCEL:
  case TD::TD_CUDA_KERNEL_LAUNCH:
  case TD::TD_COND_WAIT:
  case TD::TD_COND_SIGNAL:
  case TD::TD_COND_BROADCAST:
  case TD::TD_BAR_WAIT:
  case TD::TD_CUDA_BARRIER:
  case TD::TD_CUDA_WARP_BARRIER:
  case TD::TD_CUDA_DEVICE_SYNC:
  case TD::TD_JTHREAD_FORK:
  case TD::TD_JTHREAD_JOIN:
  case TD::TD_ASYNC:
  case TD::HARE_PAR_FOR:
    return true;
  default:
    return false;
  }
}

bool isOpenMPTaskType(TD type) {
  switch (type) {
  case TD::TD_OMP_TASK:
  case TD::TD_OMP_TASKWAIT:
  case TD::TD_OMP_TASKWAIT_DEPS:
  case TD::TD_OMP_TASKYIELD:
  case TD::TD_OMP_TASKGROUP_START:
  case TD::TD_OMP_TASKGROUP_END:
  case TD::TD_OMP_TASK_WITH_DEPS:
  case TD::TD_OMP_TASKLOOP:
  case TD::TD_OMP_TASK_COMPLETE:
  case TD::TD_OMP_DOACROSS_WAIT:
  case TD::TD_OMP_DOACROSS_SUBMIT:
  case TD::TD_OMP_SINGLE_START:
  case TD::TD_OMP_SINGLE_END:
  case TD::TD_OMP_MASTER_START:
  case TD::TD_OMP_MASTER_END:
  case TD::TD_OMP_ORDERED_START:
  case TD::TD_OMP_ORDERED_END:
  case TD::TD_OMP_REDUCE_START:
  case TD::TD_OMP_REDUCE_END:
  case TD::TD_OMP_REDUCE_NOWAIT_START:
  case TD::TD_OMP_REDUCE_NOWAIT_END:
  case TD::TD_OMP_FOR_STATIC_INIT:
  case TD::TD_OMP_FOR_STATIC_FINI:
  case TD::TD_OMP_FOR_DISPATCH_INIT:
  case TD::TD_OMP_FOR_DISPATCH_NEXT:
  case TD::TD_OMP_FOR_DISPATCH_FINI:
  case TD::TD_OMP_SECTIONS_INIT:
  case TD::TD_OMP_SECTIONS_NEXT:
  case TD::TD_OMP_SECTIONS_END:
  case TD::TD_FORK:
    return true;
  default:
    return false;
  }
}

LoweringInfo makeLoweringInfo(TD type, ThreadAPI::RuntimeLibrary library,
                              llvm::StringRef semantic_tag) {
  if (type == TD::TD_DUMMY) {
    return {LoweringKind::RecognizedButUnmodeled, "unknown-api",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (type == TD::TD_ASYNC) {
    return {LoweringKind::Deferred, "async-launch-policy-witness",
            ownerMask(Owner::HB, Owner::MHP)};
  }
  if (type == TD::TD_CUDA_MULTI_DEVICE_LAUNCH) {
    return {LoweringKind::RecognizedButUnmodeled,
            "cuda-multi-device-launch-records-unmodeled",
            ownerMask(Owner::ExplicitFallback, Owner::MHP)};
  }
  if (type == TD::TD_CPP_LOCK_MOVE_CTOR ||
      type == TD::TD_CPP_LOCK_MOVE_ASSIGN || type == TD::TD_CPP_LOCK_SWAP ||
      type == TD::TD_CPP_LOCK_RELEASE) {
    return {LoweringKind::Modeled, "cpp-lock-wrapper-state-transition",
            ownerMask(Owner::LockSet)};
  }
  if (type == TD::TD_JTHREAD_DTOR) {
    return {LoweringKind::RecognizedButUnmodeled,
            "jthread-autojoin-lifetime-unmodeled",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (type == TD::TD_ATOMIC_WAIT) {
    return {LoweringKind::RecognizedButUnmodeled,
            "cpp-atomic-wait-runtime-unmodeled",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (type == TD::TD_ATOMIC_NOTIFY_ONE || type == TD::TD_ATOMIC_NOTIFY_ALL) {
    return {LoweringKind::RecognizedButUnmodeled,
            "cpp-atomic-notify-runtime-unmodeled",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (isSemaphoreTypeForLowering(type)) {
    return {LoweringKind::RecognizedButUnmodeled,
            "counting-semaphore-runtime-unmodeled",
            ownerMask(Owner::ExplicitFallback)};
  }
  if (type == TD::TD_KERNEL_ATOMIC_READ ||
      type == TD::TD_KERNEL_ATOMIC_WRITE ||
      type == TD::TD_KERNEL_ATOMIC_RMW) {
    return {LoweringKind::RecognizedButUnmodeled,
            "linux-call-atomic-metadata-only",
            ownerMask(Owner::ExplicitFallback, Owner::HB)};
  }
  if (type == TD::TD_OMP_ATOMIC_START || type == TD::TD_OMP_ATOMIC_END) {
    return {LoweringKind::RecognizedButUnmodeled,
            "openmp-atomic-runtime-unmodeled",
            ownerMask(Owner::OpenMP, Owner::ExplicitFallback)};
  }
  if (isExplicitFallbackType(type)) {
    const char *reason = library == ThreadAPI::RuntimeLibrary::MPI
                             ? "metadata-only-mpi-api"
                             : "recognized-openmp-runtime-unmodeled";
    uint32_t owners = ownerMask(Owner::ExplicitFallback);
    if (library == ThreadAPI::RuntimeLibrary::MPI) {
      owners |= ownerMask(Owner::MPI);
    } else if (library == ThreadAPI::RuntimeLibrary::OpenMP) {
      owners |= ownerMask(Owner::OpenMP);
    }
    return {LoweringKind::RecognizedButUnmodeled, reason, owners};
  }

  uint32_t owners = 0;
  switch (library) {
  case ThreadAPI::RuntimeLibrary::OpenMP:
    owners |= ownerMask(Owner::OpenMP);
    break;
  case ThreadAPI::RuntimeLibrary::MPI:
    owners |= ownerMask(Owner::MPI);
    break;
  case ThreadAPI::RuntimeLibrary::CUDA:
    owners |= ownerMask(Owner::MHP, Owner::HB);
    break;
  case ThreadAPI::RuntimeLibrary::PThread:
  case ThreadAPI::RuntimeLibrary::Cpp:
  case ThreadAPI::RuntimeLibrary::LinuxKernel:
  case ThreadAPI::RuntimeLibrary::Hare:
  case ThreadAPI::RuntimeLibrary::Custom:
    owners |= ownerMask(Owner::MHP);
    break;
  case ThreadAPI::RuntimeLibrary::Unknown:
    break;
  }

  if (isLockLikeType(type)) {
    owners |= ownerMask(Owner::LockSet, Owner::MHP);
  }
  if (isBarrierHBType(type)) {
    owners |= ownerMask(Owner::HB);
  }
  if (isMHPThreadType(type)) {
    owners |= ownerMask(Owner::MHP);
  }
  if (isOpenMPTaskType(type) || semantic_tag.startswith("task") ||
      semantic_tag.startswith("taskgroup") ||
      semantic_tag.startswith("doacross")) {
    owners |= ownerMask(Owner::HB, Owner::MHP);
  }
  if (library == ThreadAPI::RuntimeLibrary::MPI &&
      type == TD::TD_MPI_COMM_DUP && semantic_tag.equals("comm-idup")) {
    owners |= ownerMask(Owner::MPI);
  }
  if (owners == 0) {
    owners = ownerMask(Owner::ExplicitFallback);
  }
  return {LoweringKind::Modeled, "modeled", owners};
}

} // namespace

bool ThreadAPI::isLibraryEnabled(RuntimeLibrary library) const {
  switch (library) {
  case RuntimeLibrary::OpenMP:
    return m_config.enable_openmp();
  case RuntimeLibrary::MPI:
    return m_config.enable_mpi();
  case RuntimeLibrary::Cpp:
    return m_config.enable_cpp11();
  case RuntimeLibrary::CUDA:
    return m_config.enable_cuda();
  case RuntimeLibrary::LinuxKernel:
    return m_config.enable_linux_kernel();
  case RuntimeLibrary::PThread:
    return m_config.enable_pthread();
  case RuntimeLibrary::Hare:
  case RuntimeLibrary::Custom:
  case RuntimeLibrary::Unknown:
    return true;
  }
  return true;
}

const ThreadAPI::APIDescription *
ThreadAPI::lookupDescription(const Function *F) const {
  if (!F) {
    return nullptr;
  }
  for (const std::string &name : getConfiguredLookupNames(F->getName())) {
    auto it = m_api_descriptions.find(name);
    if (it != m_api_descriptions.end()) {
      return &it->second;
    }
  }
  return nullptr;
}

const ThreadAPI::APIDescription *
ThreadAPI::lookupDescription(StringRef name) const {
  for (const std::string &candidate : getConfiguredLookupNames(name)) {
    auto it = m_api_descriptions.find(candidate);
    if (it != m_api_descriptions.end())
      return &it->second;
  }
  return nullptr;
}

const ThreadAPI::MatchRule *
ThreadAPI::lookupMatchRule(StringRef normalized_name) const {
  auto exact_it = m_exact_match_rules.find(normalized_name.str());
  if (exact_it != m_exact_match_rules.end()) {
    return &exact_it->second;
  }
  const MatchRule *best_prefix = nullptr;
  for (const MatchRule &rule : m_prefix_match_rules) {
    if (!normalized_name.startswith(rule.pattern)) {
      continue;
    }
    if (!best_prefix || rule.pattern.size() > best_prefix->pattern.size()) {
      best_prefix = &rule;
    }
  }
  return best_prefix;
}

ThreadAPI::TD_TYPE
ThreadAPI::getConfiguredType(StringRef normalized_name) const {
  for (const std::string &lookup_name :
       getConfiguredLookupNames(normalized_name)) {
    TDAPIMap::const_iterator it = tdAPIMap.find(lookup_name);
    if (it != tdAPIMap.end()) {
      auto desc_it = m_api_descriptions.find(lookup_name);
      const RuntimeLibrary library =
          desc_it == m_api_descriptions.end() ? inferLibrary(it->second)
                                              : desc_it->second.library;
      if (isLibraryEnabled(library)) {
        return it->second;
      }
      return TD_DUMMY;
    }
    if (const MatchRule *rule = lookupMatchRule(lookup_name)) {
      return isLibraryEnabled(rule->description.library)
                 ? rule->description.type
                 : TD_DUMMY;
    }
  }

  if (looksLikeMPISymbol(normalized_name)) {
    const std::string normalized_mpi =
        mpi::normalizeMPISymbolName(stripAPIGlobalPrefix(normalized_name));
    for (const auto &entry : tdAPIMap) {
      StringRef configured = entry.first();
      if (!configured.startswith("MPI_")) {
        continue;
      }
      if (!mpi::equalsCaseInsensitiveASCII(configured, normalized_mpi)) {
        continue;
      }
      auto desc_it = m_api_descriptions.find(configured.str());
      if (desc_it == m_api_descriptions.end() ||
          isLibraryEnabled(desc_it->second.library)) {
        return entry.second;
      }
      return TD_DUMMY;
    }
  }

  return TD_DUMMY;
}

const ThreadAPI::APIDescription &
ThreadAPI::describeCached(const Function *F) const {
  static const APIDescription kEmptyDescription;
  if (!F) {
    return kEmptyDescription;
  }
  const StringRef raw_name = F->getName();
  auto cached = m_func_desc_cache.find(raw_name);
  if (cached != m_func_desc_cache.end()) {
    return cached->second;
  }
  APIDescription description;
  if (const APIDescription *configured = lookupDescription(F)) {
    description = *configured;
  } else {
    bool matched = false;
    for (const std::string &lookup_name : getConfiguredLookupNames(raw_name)) {
      if (const MatchRule *rule = lookupMatchRule(lookup_name)) {
        description = rule->description;
        matched = true;
        break;
      }
    }
    if (!matched) {
      description.type = getType(F);
      description.library = inferLibrary(description.type);
    }
  }
  return m_func_desc_cache.insert({raw_name, std::move(description)})
      .first->second;
}

const ThreadAPI::APIDescription &
ThreadAPI::describeCached(const CallBase *cb) const {
  static const APIDescription kEmptyDescription;
  if (!cb) {
    return kEmptyDescription;
  }
  if (const GlobalValue *called = getCalledGlobal(cb)) {
    const Function *callee = getCallee(cb);
    if (callee && callee == called) {
      return describeCached(callee);
    }
    const StringRef raw_name = called->getName();
    auto cached = m_global_desc_cache.find(raw_name);
    if (cached != m_global_desc_cache.end()) {
      if (cached->second.type != TD_DUMMY || cached->second.from_config) {
        return cached->second;
      }
      return describeCached(callee);
    }
    APIDescription description;
    if (const APIDescription *configured = lookupDescription(raw_name)) {
      description = *configured;
      return m_global_desc_cache.insert({raw_name, std::move(description)})
          .first->second;
    }
    for (const std::string &candidate : getConfiguredLookupNames(raw_name)) {
      if (const MatchRule *rule = lookupMatchRule(candidate)) {
        description = rule->description;
        return m_global_desc_cache.insert({raw_name, std::move(description)})
            .first->second;
      }
    }
    const TD_TYPE public_type = getTypeForName(raw_name);
    if (public_type != TD_DUMMY) {
      description.type = public_type;
      description.library = inferLibrary(public_type);
      return m_global_desc_cache.insert({raw_name, std::move(description)})
          .first->second;
    }
    m_global_desc_cache.insert({raw_name, description});
    return describeCached(callee);
  }
  return describeCached(getCallee(cb));
}

ThreadAPI::APIDescription ThreadAPI::describe(const Function *F) const {
  return describeCached(F);
}

ThreadAPI::APIDescription ThreadAPI::describe(const CallBase *cb) const {
  return describeCached(cb);
}

ThreadAPI::TD_TYPE ThreadAPI::getType(const CallBase *cb) const {
  if (!cb)
    return TD_DUMMY;
  if (const Function *direct = cb->getCalledFunction()) {
    if (direct->isIntrinsic()) {
      const StringRef name = direct->getName();
      if (!name.startswith("llvm.nvvm.") && !name.startswith("llvm.memcpy.")) {
        return TD_DUMMY;
      }
    }
    return getType(direct);
  }
  if (const GlobalValue *called = getCalledGlobal(cb)) {
    const Function *callee = getCallee(cb);
    if (callee && callee == called) {
      return getType(callee);
    }
    const TD_TYPE public_type = getTypeForName(called->getName());
    if (public_type != TD_DUMMY)
      return public_type;
    return getType(callee);
  }
  return getType(getCallee(cb));
}

ThreadAPI::SemanticLoweringInfo
ThreadAPI::getSemanticLoweringInfo(TD_TYPE type) const {
  return makeLoweringInfo(type, inferLibrary(type), "");
}

ThreadAPI::SemanticLoweringInfo
ThreadAPI::getSemanticLoweringInfo(const Function *F) const {
  if (!F) {
    return getSemanticLoweringInfo(TD_DUMMY);
  }
  const APIDescription &description = describeCached(F);
  SemanticLoweringInfo info = makeLoweringInfo(
      description.type, description.library, description.semantic_tag);
  const bool has_semaphore_trait =
      llvm::is_contained(description.traits, std::string("semaphore"));
  const bool is_binary_semaphore =
      llvm::is_contained(description.traits, std::string("binary-semaphore")) ||
      F->getName().contains("binary_semaphore");
  if (isSemaphoreTypeForLowering(description.type) || has_semaphore_trait) {
    if (is_binary_semaphore) {
      info.kind = SemanticLoweringKind::Modeled;
      info.reason = "modeled";
      info.owners = semanticLoweringOwnerMask(SemanticLoweringOwner::LockSet,
                                              SemanticLoweringOwner::MHP);
    } else {
      info.kind = SemanticLoweringKind::RecognizedButUnmodeled;
      info.reason = "counting-semaphore-runtime-unmodeled";
      info.owners = semanticLoweringOwnerMask(
          SemanticLoweringOwner::ExplicitFallback, SemanticLoweringOwner::HB);
    }
  }
  return info;
}

ThreadAPI::TD_TYPE ThreadAPI::getType(const Function *F) const {
  if (!F)
    return TD_DUMMY;

  const StringRef raw_name = F->getName();
  auto cached = m_func_type_cache.find(raw_name);
  if (cached != m_func_type_cache.end()) {
    return cached->second;
  }

  auto cacheAndReturn = [&](TD_TYPE result) {
    m_func_type_cache[raw_name] = result;
    return result;
  };

  // 1. Configured match using the same canonical aliases as descriptions and
  // operand layouts.
  for (const std::string &candidate : getConfiguredLookupNames(raw_name)) {
    TD_TYPE configured_type = getConfiguredType(candidate);
    if (configured_type != TD_DUMMY) {
      return cacheAndReturn(configured_type);
    }
  }

  std::string nameStr = normalizeAPIName(raw_name);
  StringRef name = nameStr;

  // 2. C++11/17/20 Support (if enabled)
  if (m_config.enable_cpp11()) {
    // Basic threading
    if (CppThreadingModel::isFork(name))
      return cacheAndReturn(TD_FORK);
    if (CppThreadingModel::isJoin(name))
      return cacheAndReturn(TD_JOIN);
    if (CppThreadingModel::isDetach(name))
      return cacheAndReturn(TD_DETACH);

    // Basic mutex operations
    if (CppThreadingModel::isTryAcquire(name))
      return cacheAndReturn(TD_TRY_ACQUIRE);
    if (CppThreadingModel::isAcquire(name))
      return cacheAndReturn(TD_ACQUIRE);
    if (CppThreadingModel::isRelease(name))
      return cacheAndReturn(TD_RELEASE);

    // Condition variables
    if (CppThreadingModel::isCondWait(name))
      return cacheAndReturn(TD_COND_WAIT);
    if (CppThreadingModel::isCondSignal(name))
      return cacheAndReturn(TD_COND_SIGNAL);
    if (CppThreadingModel::isCondBroadcast(name))
      return cacheAndReturn(TD_COND_BROADCAST);

    // Wrapper member operations must precede raw shared_mutex matching: the
    // wrapper's template argument also contains the shared_mutex spelling.
    if (CppThreadingModel::isSharedLockTryLock(name))
      return cacheAndReturn(TD_CPP_LOCK_TRY);
    if (CppThreadingModel::isSharedLockLock(name))
      return cacheAndReturn(TD_SHARED_RDLOCK);
    if (CppThreadingModel::isSharedLockUnlock(name))
      return cacheAndReturn(TD_SHARED_UNLOCK);

    // C++17 shared_mutex
    if (CppThreadingModel::isSharedLockTryAcquire(name) ||
        CppThreadingModel::isSharedTimedLockTryAcquire(name))
      return cacheAndReturn(TD_CPP_LOCK_TRY);
    if (CppThreadingModel::isSharedLockExclusiveTryAcquire(name) ||
        CppThreadingModel::isSharedTimedLockExclusiveTryAcquire(name))
      return cacheAndReturn(TD_CPP_LOCK_TRY);
    if (CppThreadingModel::isSharedLockAcquire(name) ||
        CppThreadingModel::isSharedTimedLockAcquire(name))
      return cacheAndReturn(TD_SHARED_RDLOCK);
    if (CppThreadingModel::isSharedLockExclusiveAcquire(name) ||
        CppThreadingModel::isSharedTimedLockExclusiveAcquire(name))
      return cacheAndReturn(TD_SHARED_WRLOCK);
    if (CppThreadingModel::isSharedLockRelease(name) ||
        CppThreadingModel::isSharedLockExclusiveRelease(name) ||
        CppThreadingModel::isSharedTimedLockRelease(name) ||
        CppThreadingModel::isSharedTimedLockExclusiveRelease(name))
      return cacheAndReturn(TD_SHARED_UNLOCK);

    // RAII lock wrappers
    if (CppThreadingModel::isLockWrapperMoveConstructor(name))
      return cacheAndReturn(TD_CPP_LOCK_MOVE_CTOR);
    if (CppThreadingModel::isLockWrapperMoveAssignment(name))
      return cacheAndReturn(TD_CPP_LOCK_MOVE_ASSIGN);
    if (CppThreadingModel::isLockWrapperSwap(name))
      return cacheAndReturn(TD_CPP_LOCK_SWAP);
    if (CppThreadingModel::isUniqueLockReleaseOwnership(name))
      return cacheAndReturn(TD_CPP_LOCK_RELEASE);
    if (CppThreadingModel::isLockGuardConstructor(name))
      return cacheAndReturn(TD_LOCK_GUARD_CTOR);
    if (CppThreadingModel::isLockGuardDestructor(name))
      return cacheAndReturn(TD_LOCK_GUARD_DTOR);
    if (CppThreadingModel::isUniqueLockConstructor(name) &&
        !CppThreadingModel::isUniqueLockMoveConstructor(name))
      return cacheAndReturn(TD_UNIQUE_LOCK_CTOR);
    if (CppThreadingModel::isUniqueLockDestructor(name))
      return cacheAndReturn(TD_UNIQUE_LOCK_DTOR);
    if (CppThreadingModel::isUniqueLockTryLock(name))
      return cacheAndReturn(TD_CPP_LOCK_TRY);
    if (CppThreadingModel::isUniqueLockLock(name))
      return cacheAndReturn(TD_UNIQUE_LOCK_LOCK);
    if (CppThreadingModel::isUniqueLockUnlock(name))
      return cacheAndReturn(TD_UNIQUE_LOCK_UNLOCK);
    if (CppThreadingModel::isScopedLockConstructor(name))
      return cacheAndReturn(TD_SCOPED_LOCK_CTOR);
    if (CppThreadingModel::isScopedLockDestructor(name))
      return cacheAndReturn(TD_SCOPED_LOCK_DTOR);
    if (CppThreadingModel::isSharedLockConstructor(name) &&
        !CppThreadingModel::isSharedLockMoveConstructor(name))
      return cacheAndReturn(TD_SHARED_LOCK_CTOR);
    if (CppThreadingModel::isSharedLockDestructor(name))
      return cacheAndReturn(TD_SHARED_LOCK_DTOR);
    // std::call_once
    if (CppThreadingModel::isCallOnce(name))
      return cacheAndReturn(TD_CALL_ONCE);

    // Future/Promise synchronization
    if (CppThreadingModel::isFutureGet(name))
      return cacheAndReturn(TD_FUTURE_GET);
    if (CppThreadingModel::isFutureWait(name))
      return cacheAndReturn(TD_FUTURE_WAIT);
    if (CppThreadingModel::isPromiseSetValue(name) ||
        CppThreadingModel::isPromiseSetException(name))
      return cacheAndReturn(TD_PROMISE_SET);
    if (CppThreadingModel::isAsync(name))
      return cacheAndReturn(TD_ASYNC);

    // C++20 jthread
    if (CppThreadingModel::isJthreadConstructor(name))
      return cacheAndReturn(TD_JTHREAD_FORK);
    if (CppThreadingModel::isJthreadJoin(name))
      return cacheAndReturn(TD_JTHREAD_JOIN);
    if (CppThreadingModel::isJthreadDetach(name))
      return cacheAndReturn(TD_DETACH);
    if (CppThreadingModel::isJthreadDestructor(name))
      return cacheAndReturn(TD_JTHREAD_DTOR);

    // C++20 atomic wait/notify
    if (CppThreadingModel::isAtomicWait(name))
      return cacheAndReturn(TD_ATOMIC_WAIT);
    if (CppThreadingModel::isAtomicNotifyOne(name))
      return cacheAndReturn(TD_ATOMIC_NOTIFY_ONE);
    if (CppThreadingModel::isAtomicNotifyAll(name))
      return cacheAndReturn(TD_ATOMIC_NOTIFY_ALL);

    // C++20 latch
    if (CppThreadingModel::isLatchCountDown(name))
      return cacheAndReturn(TD_LATCH_COUNT_DOWN);
    if (CppThreadingModel::isLatchWait(name))
      return cacheAndReturn(TD_LATCH_WAIT);
    if (CppThreadingModel::isLatchArriveAndWait(name))
      return cacheAndReturn(TD_LATCH_ARRIVE_WAIT);

    // C++20 barrier
    if (CppThreadingModel::isBarrierArriveAndWait(name))
      return cacheAndReturn(TD_BARRIER_ARRIVE_WAIT);
    if (CppThreadingModel::isBarrierArrive(name))
      return cacheAndReturn(TD_BARRIER_ARRIVE);
    if (CppThreadingModel::isBarrierWait(name))
      return cacheAndReturn(TD_BARRIER_WAIT_CPP20);

    // C++20 semaphore
    if (CppThreadingModel::isSemaphoreTryAcquire(name))
      return cacheAndReturn(TD_SEMAPHORE_TRY_ACQUIRE);
    if (CppThreadingModel::isSemaphoreAcquire(name))
      return cacheAndReturn(TD_SEMAPHORE_ACQUIRE);
    if (CppThreadingModel::isSemaphoreRelease(name))
      return cacheAndReturn(TD_SEMAPHORE_RELEASE);
  }

  if (m_config.enable_cuda()) {
    if (CUDAModel::isLegacyKernelConfiguration(name))
      return cacheAndReturn(TD_CUDA_KERNEL_LAUNCH);
    if (CUDAModel::isKernelLaunch(name))
      return cacheAndReturn(TD_CUDA_KERNEL_LAUNCH);
    if (CUDAModel::isAggregateKernelLaunch(name))
      return cacheAndReturn(TD_CUDA_MULTI_DEVICE_LAUNCH);
    if (CUDAModel::isKernelGraphOperation(name))
      return cacheAndReturn(TD_CUDA_STREAM);
    if (CUDAModel::isDeviceSynchronize(name))
      return cacheAndReturn(TD_CUDA_DEVICE_SYNC);
    if (CUDAModel::isBarrier(name))
      return cacheAndReturn(TD_CUDA_BARRIER);
    if (CUDAModel::isWarpBarrier(name))
      return cacheAndReturn(TD_CUDA_WARP_BARRIER);
    if (CUDAModel::isMemoryBarrier(name))
      return cacheAndReturn(TD_CUDA_MEMORY_BARRIER);
    if (CUDAModel::isAtomic(name))
      return cacheAndReturn(TD_CUDA_ATOMIC);
    if (CUDAModel::isUnifiedMemory(name))
      return cacheAndReturn(TD_CUDA_UNIFIED_MEMORY);
    if (CUDAModel::isMemcpy(name))
      return cacheAndReturn(TD_CUDA_MEMCPY);
    if (CUDAModel::isMemset(name))
      return cacheAndReturn(TD_CUDA_MEMSET);
    if (CUDAModel::isFree(name))
      return cacheAndReturn(TD_CUDA_FREE);
    if (CUDAModel::isMalloc(name))
      return cacheAndReturn(TD_CUDA_MALLOC);
    if (CUDAModel::isStreamOperation(name))
      return cacheAndReturn(TD_CUDA_STREAM);
    if (CUDAModel::isEventOperation(name))
      return cacheAndReturn(TD_CUDA_EVENT);
    if (CUDAModel::isTexture(name))
      return cacheAndReturn(TD_CUDA_TEXTURE);
    if (CUDAModel::isSurface(name))
      return cacheAndReturn(TD_CUDA_SURFACE);
    if (CUDAModel::isDeviceManagement(name))
      return cacheAndReturn(TD_CUDA_DEVICE_MGMT);
    if (CUDAModel::isErrorHandling(name))
      return cacheAndReturn(TD_CUDA_ERROR);
  }

  // 4. Linux Kernel Support (if enabled)
  if (m_config.enable_linux_kernel()) {
    return cacheAndReturn(getKernelType(name));
  }

  return cacheAndReturn(TD_DUMMY);
}

ThreadAPI::TD_TYPE ThreadAPI::getTypeForName(StringRef raw_name) const {
  if (raw_name.empty())
    return TD_DUMMY;
  auto cached = m_name_type_cache.find(raw_name);
  if (cached != m_name_type_cache.end()) {
    return cached->second;
  }
  auto cacheAndReturn = [&](TD_TYPE result) {
    m_name_type_cache[raw_name] = result;
    return result;
  };
  if (const TD_TYPE configured = getConfiguredType(raw_name);
      configured != TD_DUMMY)
    return cacheAndReturn(configured);

  const std::string normalized = normalizeAPIName(raw_name);
  const StringRef name = normalized;
  if (m_config.enable_cpp11()) {
    if (CppThreadingModel::isFork(name)) return cacheAndReturn(TD_FORK);
    if (CppThreadingModel::isJoin(name)) return cacheAndReturn(TD_JOIN);
    if (CppThreadingModel::isDetach(name)) return cacheAndReturn(TD_DETACH);
    if (CppThreadingModel::isTryAcquire(name)) return cacheAndReturn(TD_TRY_ACQUIRE);
    if (CppThreadingModel::isAcquire(name)) return cacheAndReturn(TD_ACQUIRE);
    if (CppThreadingModel::isRelease(name)) return cacheAndReturn(TD_RELEASE);
    if (CppThreadingModel::isSharedLockTryLock(name)) return cacheAndReturn(TD_CPP_LOCK_TRY);
    if (CppThreadingModel::isSharedLockLock(name)) return cacheAndReturn(TD_SHARED_RDLOCK);
    if (CppThreadingModel::isSharedLockUnlock(name)) return cacheAndReturn(TD_SHARED_UNLOCK);
    if (CppThreadingModel::isUniqueLockTryLock(name)) return cacheAndReturn(TD_CPP_LOCK_TRY);
    if (CppThreadingModel::isUniqueLockLock(name)) return cacheAndReturn(TD_UNIQUE_LOCK_LOCK);
    if (CppThreadingModel::isUniqueLockUnlock(name)) return cacheAndReturn(TD_UNIQUE_LOCK_UNLOCK);
    if (CppThreadingModel::isAsync(name)) return cacheAndReturn(TD_ASYNC);
    if (CppThreadingModel::isAtomicWait(name)) return cacheAndReturn(TD_ATOMIC_WAIT);
    if (CppThreadingModel::isAtomicNotifyOne(name)) return cacheAndReturn(TD_ATOMIC_NOTIFY_ONE);
    if (CppThreadingModel::isAtomicNotifyAll(name)) return cacheAndReturn(TD_ATOMIC_NOTIFY_ALL);
    if (CppThreadingModel::isBarrierArriveAndWait(name))
      return cacheAndReturn(TD_BARRIER_ARRIVE_WAIT);
    if (CppThreadingModel::isBarrierArrive(name)) return cacheAndReturn(TD_BARRIER_ARRIVE);
    if (CppThreadingModel::isBarrierWait(name)) return cacheAndReturn(TD_BARRIER_WAIT_CPP20);
  }
  if (m_config.enable_cuda()) {
    if (CUDAModel::isAggregateKernelLaunch(name))
      return cacheAndReturn(TD_CUDA_MULTI_DEVICE_LAUNCH);
    if (CUDAModel::isKernelLaunch(name) ||
        CUDAModel::isLegacyKernelConfiguration(name))
      return cacheAndReturn(TD_CUDA_KERNEL_LAUNCH);
  }
  if (m_config.enable_linux_kernel()) {
    return cacheAndReturn(getKernelType(name));
  }
  return cacheAndReturn(TD_DUMMY);
}

/*!
 * Get the callee function from an instruction
 */
const Function *ThreadAPI::getCallee(const Instruction *inst) const {
  return getCallee(dyn_cast_or_null<CallBase>(inst));
}

/*!
 * Get the callee function from a CallBase
 */
const Function *ThreadAPI::getCallee(const CallBase *cb) const {
  return lotus::concurrency::resolveCallTarget(cb);
}

const GlobalValue *ThreadAPI::getCalledGlobal(const CallBase *cb) const {
  return lotus::concurrency::getCalledGlobal(cb);
}

StringRef ThreadAPI::getCalledAPIName(const CallBase *cb) const {
  const GlobalValue *called = getCalledGlobal(cb);
  return called ? called->getName() : StringRef();
}

/*!
 * Get the CallBase from an instruction
 */
const CallBase *ThreadAPI::getLLVMCallSite(const Instruction *inst) const {
  return dyn_cast<CallBase>(inst);
}

/*!
 *
 */
void ThreadAPI::statInit(llvm::StringMap<u32_t> &tdAPIStatMap) {

  tdAPIStatMap["pthread_create"] = 0;

  tdAPIStatMap["pthread_join"] = 0;

  tdAPIStatMap["pthread_mutex_lock"] = 0;

  tdAPIStatMap["pthread_mutex_trylock"] = 0;

  tdAPIStatMap["pthread_mutex_unlock"] = 0;

  tdAPIStatMap["pthread_cancel"] = 0;

  tdAPIStatMap["pthread_exit"] = 0;

  tdAPIStatMap["pthread_detach"] = 0;

  tdAPIStatMap["pthread_cond_wait"] = 0;

  tdAPIStatMap["pthread_cond_signal"] = 0;

  tdAPIStatMap["pthread_cond_broadcast"] = 0;

  tdAPIStatMap["pthread_cond_init"] = 0;

  tdAPIStatMap["pthread_cond_destroy"] = 0;

  tdAPIStatMap["pthread_mutex_init"] = 0;

  tdAPIStatMap["pthread_mutex_destroy"] = 0;

  tdAPIStatMap["pthread_barrier_init"] = 0;

  tdAPIStatMap["pthread_barrier_wait"] = 0;

  tdAPIStatMap["hare_parallel_for"] = 0;
}

void ThreadAPI::performAPIStat(Module *module) {

  llvm::StringMap<u32_t> tdAPIStatMap;

  statInit(tdAPIStatMap);

  for (Module::iterator it = module->begin(), eit = module->end(); it != eit;
       ++it) {

    for (inst_iterator II = inst_begin(*it), E = inst_end(*it); II != E; ++II) {
      const Instruction *inst = &*II;
      if (!llvm::isa<CallInst>(inst) && !llvm::isa<InvokeInst>(inst))
        continue;
      const Function *fun = getCallee(inst);
      const CallBase *call = getLLVMCallSite(inst);
      TD_TYPE type = call ? getType(call) : getType(fun);
      switch (type) {
      case TD_FORK: {
        tdAPIStatMap["pthread_create"]++;
        break;
      }
      case TD_JOIN: {
        tdAPIStatMap["pthread_join"]++;
        break;
      }
      case TD_ACQUIRE: {
        tdAPIStatMap["pthread_mutex_lock"]++;
        break;
      }
      case TD_TRY_ACQUIRE: {
        tdAPIStatMap["pthread_mutex_trylock"]++;
        break;
      }
      case TD_RWLOCK_RDLOCK: {
        tdAPIStatMap["pthread_rwlock_rdlock"]++;
        break;
      }
      case TD_RWLOCK_WRLOCK: {
        tdAPIStatMap["pthread_rwlock_wrlock"]++;
        break;
      }
      case TD_RELEASE: {
        tdAPIStatMap["pthread_mutex_unlock"]++;
        break;
      }
      case TD_CANCEL: {
        tdAPIStatMap["pthread_cancel"]++;
        break;
      }
      case TD_EXIT: {
        tdAPIStatMap["pthread_exit"]++;
        break;
      }
      case TD_DETACH: {
        tdAPIStatMap["pthread_detach"]++;
        break;
      }
      case TD_COND_WAIT: {
        tdAPIStatMap["pthread_cond_wait"]++;
        break;
      }
      case TD_COND_SIGNAL: {
        tdAPIStatMap["pthread_cond_signal"]++;
        break;
      }
      case TD_COND_BROADCAST: {
        tdAPIStatMap["pthread_cond_broadcast"]++;
        break;
      }
      case TD_CONDVAR_INI: {
        tdAPIStatMap["pthread_cond_init"]++;
        break;
      }
      case TD_CONDVAR_DESTROY: {
        tdAPIStatMap["pthread_cond_destroy"]++;
        break;
      }
      case TD_MUTEX_INI: {
        tdAPIStatMap["pthread_mutex_init"]++;
        break;
      }
      case TD_MUTEX_DESTROY: {
        tdAPIStatMap["pthread_mutex_destroy"]++;
        break;
      }
      case TD_BAR_INIT: {
        tdAPIStatMap["pthread_barrier_init"]++;
        break;
      }
      case TD_BAR_WAIT: {
        tdAPIStatMap["pthread_barrier_wait"]++;
        break;
      }
      case HARE_PAR_FOR: {
        tdAPIStatMap["hare_parallel_for"]++;
        break;
      }
      case TD_DUMMY:
      default: {
        // Handle TD_DUMMY and all other thread API types (C++11/17/20, OpenMP,
        // MPI, etc.) These are not explicitly tracked in statistics
        break;
      }
      }
    }
  }

  StringRef n(module->getModuleIdentifier());
  StringRef name = n.split('/').second;
  name = name.split('.').first;
  std::string nameStr = name.str();
  std::cout << "################ (program : " << nameStr
            << ")###############\n";
  std::cout.flags(std::ios::left);
  unsigned field_width = 20;
  for (llvm::StringMap<u32_t>::iterator it = tdAPIStatMap.begin(),
                                        eit = tdAPIStatMap.end();
       it != eit; ++it) {
    std::string apiName = it->first().str();
    // format out put with width 20 space
    std::cout << std::setw(field_width) << apiName << " : " << it->second
              << "\n";
  }
  std::cout << "#######################################################"
            << "\n";
}

const char *ThreadAPI::tdTypeToString(TD_TYPE t) {
  switch (t) {
  case TD_DUMMY:
    return "TD_DUMMY";
  case TD_FORK:
    return "TD_FORK";
  case TD_JOIN:
    return "TD_JOIN";
  case TD_DETACH:
    return "TD_DETACH";
  case TD_ACQUIRE:
    return "TD_ACQUIRE";
  case TD_TRY_ACQUIRE:
    return "TD_TRY_ACQUIRE";
  case TD_RWLOCK_RDLOCK:
    return "TD_RWLOCK_RDLOCK";
  case TD_RWLOCK_WRLOCK:
    return "TD_RWLOCK_WRLOCK";
  case TD_RELEASE:
    return "TD_RELEASE";
  case TD_EXIT:
    return "TD_EXIT";
  case TD_CANCEL:
    return "TD_CANCEL";
  case TD_COND_WAIT:
    return "TD_COND_WAIT";
  case TD_COND_SIGNAL:
    return "TD_COND_SIGNAL";
  case TD_COND_BROADCAST:
    return "TD_COND_BROADCAST";
  case TD_MUTEX_INI:
    return "TD_MUTEX_INI";
  case TD_MUTEX_DESTROY:
    return "TD_MUTEX_DESTROY";
  case TD_CONDVAR_INI:
    return "TD_CONDVAR_INI";
  case TD_CONDVAR_DESTROY:
    return "TD_CONDVAR_DESTROY";
  case TD_BAR_INIT:
    return "TD_BAR_INIT";
  case TD_BAR_WAIT:
    return "TD_BAR_WAIT";
  case HARE_PAR_FOR:
    return "HARE_PAR_FOR";
  case TD_SHARED_RDLOCK:
    return "TD_SHARED_RDLOCK";
  case TD_SHARED_WRLOCK:
    return "TD_SHARED_WRLOCK";
  case TD_SHARED_UNLOCK:
    return "TD_SHARED_UNLOCK";
  case TD_CALL_ONCE:
    return "TD_CALL_ONCE";
  case TD_FUTURE_GET:
    return "TD_FUTURE_GET";
  case TD_FUTURE_WAIT:
    return "TD_FUTURE_WAIT";
  case TD_PROMISE_SET:
    return "TD_PROMISE_SET";
  case TD_ASYNC:
    return "TD_ASYNC";
  case TD_LOCK_GUARD_CTOR:
    return "TD_LOCK_GUARD_CTOR";
  case TD_LOCK_GUARD_DTOR:
    return "TD_LOCK_GUARD_DTOR";
  case TD_UNIQUE_LOCK_CTOR:
    return "TD_UNIQUE_LOCK_CTOR";
  case TD_UNIQUE_LOCK_DTOR:
    return "TD_UNIQUE_LOCK_DTOR";
  case TD_UNIQUE_LOCK_LOCK:
    return "TD_UNIQUE_LOCK_LOCK";
  case TD_UNIQUE_LOCK_UNLOCK:
    return "TD_UNIQUE_LOCK_UNLOCK";
  case TD_SCOPED_LOCK_CTOR:
    return "TD_SCOPED_LOCK_CTOR";
  case TD_SCOPED_LOCK_DTOR:
    return "TD_SCOPED_LOCK_DTOR";
  case TD_SHARED_LOCK_CTOR:
    return "TD_SHARED_LOCK_CTOR";
  case TD_SHARED_LOCK_DTOR:
    return "TD_SHARED_LOCK_DTOR";
  case TD_CPP_LOCK_TRY:
    return "TD_CPP_LOCK_TRY";
  case TD_CPP_LOCK_MOVE_CTOR:
    return "TD_CPP_LOCK_MOVE_CTOR";
  case TD_CPP_LOCK_MOVE_ASSIGN:
    return "TD_CPP_LOCK_MOVE_ASSIGN";
  case TD_CPP_LOCK_SWAP:
    return "TD_CPP_LOCK_SWAP";
  case TD_CPP_LOCK_RELEASE:
    return "TD_CPP_LOCK_RELEASE";
  case TD_JTHREAD_FORK:
    return "TD_JTHREAD_FORK";
  case TD_JTHREAD_JOIN:
    return "TD_JTHREAD_JOIN";
  case TD_JTHREAD_DTOR:
    return "TD_JTHREAD_DTOR";
  case TD_ATOMIC_WAIT:
    return "TD_ATOMIC_WAIT";
  case TD_ATOMIC_NOTIFY_ONE:
    return "TD_ATOMIC_NOTIFY_ONE";
  case TD_ATOMIC_NOTIFY_ALL:
    return "TD_ATOMIC_NOTIFY_ALL";
  case TD_LATCH_COUNT_DOWN:
    return "TD_LATCH_COUNT_DOWN";
  case TD_LATCH_WAIT:
    return "TD_LATCH_WAIT";
  case TD_LATCH_ARRIVE_WAIT:
    return "TD_LATCH_ARRIVE_WAIT";
  case TD_BARRIER_ARRIVE_WAIT:
    return "TD_BARRIER_ARRIVE_WAIT";
  case TD_BARRIER_ARRIVE:
    return "TD_BARRIER_ARRIVE";
  case TD_BARRIER_WAIT_CPP20:
    return "TD_BARRIER_WAIT_CPP20";
  case TD_SEMAPHORE_ACQUIRE:
    return "TD_SEMAPHORE_ACQUIRE";
  case TD_SEMAPHORE_RELEASE:
    return "TD_SEMAPHORE_RELEASE";
  case TD_SEMAPHORE_TRY_ACQUIRE:
    return "TD_SEMAPHORE_TRY_ACQUIRE";
  case TD_OMP_TASK:
    return "TD_OMP_TASK";
  case TD_OMP_TASKWAIT:
    return "TD_OMP_TASKWAIT";
  case TD_OMP_TASKWAIT_DEPS:
    return "TD_OMP_TASKWAIT_DEPS";
  case TD_OMP_TASKYIELD:
    return "TD_OMP_TASKYIELD";
  case TD_OMP_TASKGROUP_START:
    return "TD_OMP_TASKGROUP_START";
  case TD_OMP_TASKGROUP_END:
    return "TD_OMP_TASKGROUP_END";
  case TD_OMP_TASK_WITH_DEPS:
    return "TD_OMP_TASK_WITH_DEPS";
  case TD_OMP_TASKLOOP:
    return "TD_OMP_TASKLOOP";
  case TD_OMP_TASK_COMPLETE:
    return "TD_OMP_TASK_COMPLETE";
  case TD_OMP_SINGLE_START:
    return "TD_OMP_SINGLE_START";
  case TD_OMP_SINGLE_END:
    return "TD_OMP_SINGLE_END";
  case TD_OMP_MASTER_START:
    return "TD_OMP_MASTER_START";
  case TD_OMP_MASTER_END:
    return "TD_OMP_MASTER_END";
  case TD_OMP_ORDERED_START:
    return "TD_OMP_ORDERED_START";
  case TD_OMP_ORDERED_END:
    return "TD_OMP_ORDERED_END";
  case TD_OMP_REDUCE_START:
    return "TD_OMP_REDUCE_START";
  case TD_OMP_REDUCE_END:
    return "TD_OMP_REDUCE_END";
  case TD_OMP_REDUCE_NOWAIT_START:
    return "TD_OMP_REDUCE_NOWAIT_START";
  case TD_OMP_REDUCE_NOWAIT_END:
    return "TD_OMP_REDUCE_NOWAIT_END";
  case TD_OMP_FOR_STATIC_INIT:
    return "TD_OMP_FOR_STATIC_INIT";
  case TD_OMP_FOR_STATIC_FINI:
    return "TD_OMP_FOR_STATIC_FINI";
  case TD_OMP_FOR_DISPATCH_INIT:
    return "TD_OMP_FOR_DISPATCH_INIT";
  case TD_OMP_FOR_DISPATCH_NEXT:
    return "TD_OMP_FOR_DISPATCH_NEXT";
  case TD_OMP_FOR_DISPATCH_FINI:
    return "TD_OMP_FOR_DISPATCH_FINI";
  case TD_OMP_SECTIONS_INIT:
    return "TD_OMP_SECTIONS_INIT";
  case TD_OMP_SECTIONS_NEXT:
    return "TD_OMP_SECTIONS_NEXT";
  case TD_OMP_SECTIONS_END:
    return "TD_OMP_SECTIONS_END";
  case TD_OMP_ATOMIC_START:
    return "TD_OMP_ATOMIC_START";
  case TD_OMP_ATOMIC_END:
    return "TD_OMP_ATOMIC_END";
  case TD_OMP_FLUSH:
    return "TD_OMP_FLUSH";
  case TD_OMP_CANCEL:
    return "TD_OMP_CANCEL";
  case TD_OMP_TARGET:
    return "TD_OMP_TARGET";
  case TD_OMP_TARGET_DATA_BEGIN:
    return "TD_OMP_TARGET_DATA_BEGIN";
  case TD_OMP_TARGET_DATA_END:
    return "TD_OMP_TARGET_DATA_END";
  case TD_OMP_TARGET_DATA_UPDATE:
    return "TD_OMP_TARGET_DATA_UPDATE";
  case TD_OMP_TEAMS:
    return "TD_OMP_TEAMS";
  case TD_OMP_TEAMS_HOST:
    return "TD_OMP_TEAMS_HOST";
  case TD_OMP_TEAMS_DISTRIBUTE:
    return "TD_OMP_TEAMS_DISTRIBUTE";
  case TD_OMP_DISTRIBUTE:
    return "TD_OMP_DISTRIBUTE";
  case TD_OMP_DISTRIBUTE_STATIC:
    return "TD_OMP_DISTRIBUTE_STATIC";
  case TD_OMP_DISTRIBUTE_DYNAMIC:
    return "TD_OMP_DISTRIBUTE_DYNAMIC";
  case TD_OMP_DISTRIBUTE_GUIDANCE:
    return "TD_OMP_DISTRIBUTE_GUIDANCE";
  case TD_OMP_LOOP_STATIC_INIT:
    return "TD_OMP_LOOP_STATIC_INIT";
  case TD_OMP_LOOP_DYNAMIC_INIT:
    return "TD_OMP_LOOP_DYNAMIC_INIT";
  case TD_OMP_LOOP_GUIDANCE_INIT:
    return "TD_OMP_LOOP_GUIDANCE_INIT";
  case TD_OMP_AFFINITY:
    return "TD_OMP_AFFINITY";
  case TD_OMP_SCOPE_START:
    return "TD_OMP_SCOPE_START";
  case TD_OMP_SCOPE_END:
    return "TD_OMP_SCOPE_END";
  case TD_OMP_TASKLOOP_SIMD:
    return "TD_OMP_TASKLOOP_SIMD";
  case TD_OMP_TASKLOOP_FINI:
    return "TD_OMP_TASKLOOP_FINI";
  case TD_OMP_INTEROP_INIT:
    return "TD_OMP_INTEROP_INIT";
  case TD_OMP_INTEROP_FINI:
    return "TD_OMP_INTEROP_FINI";
  case TD_OMP_DOACROSS_INIT:
    return "TD_OMP_DOACROSS_INIT";
  case TD_OMP_DOACROSS_WAIT:
    return "TD_OMP_DOACROSS_WAIT";
  case TD_OMP_DOACROSS_SUBMIT:
    return "TD_OMP_DOACROSS_SUBMIT";
  case TD_CUDA_KERNEL_LAUNCH:
    return "TD_CUDA_KERNEL_LAUNCH";
  case TD_CUDA_MULTI_DEVICE_LAUNCH:
    return "TD_CUDA_MULTI_DEVICE_LAUNCH";
  case TD_CUDA_DEVICE_SYNC:
    return "TD_CUDA_DEVICE_SYNC";
  case TD_CUDA_BARRIER:
    return "TD_CUDA_BARRIER";
  case TD_CUDA_WARP_BARRIER:
    return "TD_CUDA_WARP_BARRIER";
  case TD_CUDA_MEMORY_BARRIER:
    return "TD_CUDA_MEMORY_BARRIER";
  case TD_CUDA_ATOMIC:
    return "TD_CUDA_ATOMIC";
  case TD_CUDA_MEMCPY:
    return "TD_CUDA_MEMCPY";
  case TD_CUDA_MEMSET:
    return "TD_CUDA_MEMSET";
  case TD_CUDA_MALLOC:
    return "TD_CUDA_MALLOC";
  case TD_CUDA_FREE:
    return "TD_CUDA_FREE";
  case TD_CUDA_UNIFIED_MEMORY:
    return "TD_CUDA_UNIFIED_MEMORY";
  case TD_CUDA_STREAM:
    return "TD_CUDA_STREAM";
  case TD_CUDA_EVENT:
    return "TD_CUDA_EVENT";
  case TD_CUDA_TEXTURE:
    return "TD_CUDA_TEXTURE";
  case TD_CUDA_SURFACE:
    return "TD_CUDA_SURFACE";
  case TD_CUDA_DEVICE_MGMT:
    return "TD_CUDA_DEVICE_MGMT";
  case TD_CUDA_ERROR:
    return "TD_CUDA_ERROR";
  case TD_KERNEL_ATOMIC_READ:
    return "TD_KERNEL_ATOMIC_READ";
  case TD_KERNEL_ATOMIC_WRITE:
    return "TD_KERNEL_ATOMIC_WRITE";
  case TD_KERNEL_ATOMIC_RMW:
    return "TD_KERNEL_ATOMIC_RMW";
  case TD_MPI_SESSION_INIT:
    return "TD_MPI_SESSION_INIT";
  case TD_MPI_SESSION_FINALIZE:
    return "TD_MPI_SESSION_FINALIZE";
  case TD_MPI_SESSION_GET_INFO:
    return "TD_MPI_SESSION_GET_INFO";
  case TD_MPI_SESSION_GET_NUM_ERRCODES:
    return "TD_MPI_SESSION_GET_NUM_ERRCODES";
  case TD_MPI_SESSION_GET_ERRHANDLER:
    return "TD_MPI_SESSION_GET_ERRHANDLER";
  case TD_MPI_SESSION_SET_ERRHANDLER:
    return "TD_MPI_SESSION_SET_ERRHANDLER";
  case TD_MPI_ERRHANDLER_CREATE:
    return "TD_MPI_ERRHANDLER_CREATE";
  case TD_MPI_ERRHANDLER_FREE:
    return "TD_MPI_ERRHANDLER_FREE";
  case TD_MPI_COMM_GET_ERRHANDLER:
    return "TD_MPI_COMM_GET_ERRHANDLER";
  case TD_MPI_COMM_SET_ERRHANDLER:
    return "TD_MPI_COMM_SET_ERRHANDLER";
  case TD_MPI_COMM_CALL_ERRHANDLER:
    return "TD_MPI_COMM_CALL_ERRHANDLER";
  case TD_MPI_WIN_GET_ERRHANDLER:
    return "TD_MPI_WIN_GET_ERRHANDLER";
  case TD_MPI_WIN_SET_ERRHANDLER:
    return "TD_MPI_WIN_SET_ERRHANDLER";
  case TD_MPI_FILE_GET_ERRHANDLER:
    return "TD_MPI_FILE_GET_ERRHANDLER";
  case TD_MPI_FILE_SET_ERRHANDLER:
    return "TD_MPI_FILE_SET_ERRHANDLER";
  case TD_MPI_ERROR_CLASS:
    return "TD_MPI_ERROR_CLASS";
  case TD_MPI_ERROR_STRING:
    return "TD_MPI_ERROR_STRING";
  case TD_MPI_INFO_CREATE:
    return "TD_MPI_INFO_CREATE";
  case TD_MPI_INFO_DUP:
    return "TD_MPI_INFO_DUP";
  case TD_MPI_INFO_FREE:
    return "TD_MPI_INFO_FREE";
  case TD_MPI_INFO_GET:
    return "TD_MPI_INFO_GET";
  case TD_MPI_INFO_GET_VALUELEN:
    return "TD_MPI_INFO_GET_VALUELEN";
  case TD_MPI_INFO_GET_NKEYS:
    return "TD_MPI_INFO_GET_NKEYS";
  case TD_MPI_INFO_GET_NTHKEY:
    return "TD_MPI_INFO_GET_NTHKEY";
  case TD_MPI_INFO_GET_KEYVAL:
    return "TD_MPI_INFO_GET_KEYVAL";
  case TD_MPI_INFO_SET:
    return "TD_MPI_INFO_SET";
  case TD_MPI_INFO_DELETE:
    return "TD_MPI_INFO_DELETE";
  case TD_MPI_INFO_C2F:
    return "TD_MPI_INFO_C2F";
  case TD_MPI_INFO_CREATE_ENV:
    return "TD_MPI_INFO_CREATE_ENV";
  case TD_MPI_INFO_FREE_ENV:
    return "TD_MPI_INFO_FREE_ENV";
  case TD_MPI_GET_COUNT:
    return "TD_MPI_GET_COUNT";
  case TD_MPI_GET_ELEMENTS:
    return "TD_MPI_GET_ELEMENTS";
  case TD_MPI_GET_ELEMENTS_X:
    return "TD_MPI_GET_ELEMENTS_X";
  case TD_MPI_STATUS_SIZE:
    return "TD_MPI_STATUS_SIZE";
  case TD_MPI_STATUS_SET_ELEMENTS:
    return "TD_MPI_STATUS_SET_ELEMENTS";
  case TD_MPI_STATUS_SET_ELEMENTS_X:
    return "TD_MPI_STATUS_SET_ELEMENTS_X";
  case TD_MPI_INIT:
    return "TD_MPI_INIT";
  case TD_MPI_FINALIZE:
    return "TD_MPI_FINALIZE";
  case TD_MPI_SEND:
    return "TD_MPI_SEND";
  case TD_MPI_RECV:
    return "TD_MPI_RECV";
  case TD_MPI_SENDRECV:
    return "TD_MPI_SENDRECV";
  case TD_MPI_PROBE:
    return "TD_MPI_PROBE";
  case TD_MPI_ISEND:
    return "TD_MPI_ISEND";
  case TD_MPI_IRECV:
    return "TD_MPI_IRECV";
  case TD_MPI_IPROBE:
    return "TD_MPI_IPROBE";
  case TD_MPI_MPROBE:
    return "TD_MPI_MPROBE";
  case TD_MPI_IMPROBE:
    return "TD_MPI_IMPROBE";
  case TD_MPI_IMRECV:
    return "TD_MPI_IMRECV";
  case TD_MPI_MRECV:
    return "TD_MPI_MRECV";
  case TD_MPI_PERSISTENT_SEND_INIT:
    return "TD_MPI_PERSISTENT_SEND_INIT";
  case TD_MPI_PERSISTENT_RECV_INIT:
    return "TD_MPI_PERSISTENT_RECV_INIT";
  case TD_MPI_REQUEST_START:
    return "TD_MPI_REQUEST_START";
  case TD_MPI_WAIT:
    return "TD_MPI_WAIT";
  case TD_MPI_WAITALL:
    return "TD_MPI_WAITALL";
  case TD_MPI_WAITANY:
    return "TD_MPI_WAITANY";
  case TD_MPI_WAITSOME:
    return "TD_MPI_WAITSOME";
  case TD_MPI_TEST:
    return "TD_MPI_TEST";
  case TD_MPI_TESTALL:
    return "TD_MPI_TESTALL";
  case TD_MPI_TESTANY:
    return "TD_MPI_TESTANY";
  case TD_MPI_TESTSOME:
    return "TD_MPI_TESTSOME";
  case TD_MPI_BARRIER:
    return "TD_MPI_BARRIER";
  case TD_MPI_BCAST:
    return "TD_MPI_BCAST";
  case TD_MPI_SCATTER:
    return "TD_MPI_SCATTER";
  case TD_MPI_GATHER:
    return "TD_MPI_GATHER";
  case TD_MPI_ALLGATHER:
    return "TD_MPI_ALLGATHER";
  case TD_MPI_ALLTOALL:
    return "TD_MPI_ALLTOALL";
  case TD_MPI_REDUCE:
    return "TD_MPI_REDUCE";
  case TD_MPI_ALLREDUCE:
    return "TD_MPI_ALLREDUCE";
  case TD_MPI_REDUCE_SCATTER:
    return "TD_MPI_REDUCE_SCATTER";
  case TD_MPI_SCAN:
    return "TD_MPI_SCAN";
  case TD_MPI_WIN_CREATE:
    return "TD_MPI_WIN_CREATE";
  case TD_MPI_WIN_FREE:
    return "TD_MPI_WIN_FREE";
  case TD_MPI_PUT:
    return "TD_MPI_PUT";
  case TD_MPI_GET:
    return "TD_MPI_GET";
  case TD_MPI_ACCUMULATE:
    return "TD_MPI_ACCUMULATE";
  case TD_MPI_WIN_FENCE:
    return "TD_MPI_WIN_FENCE";
  case TD_MPI_WIN_LOCK:
    return "TD_MPI_WIN_LOCK";
  case TD_MPI_WIN_UNLOCK:
    return "TD_MPI_WIN_UNLOCK";
  case TD_MPI_WIN_FLUSH:
    return "TD_MPI_WIN_FLUSH";
  case TD_MPI_WIN_SYNC:
    return "TD_MPI_WIN_SYNC";
  case TD_MPI_WIN_POST:
    return "TD_MPI_WIN_POST";
  case TD_MPI_WIN_START:
    return "TD_MPI_WIN_START";
  case TD_MPI_WIN_COMPLETE:
    return "TD_MPI_WIN_COMPLETE";
  case TD_MPI_WIN_WAIT:
    return "TD_MPI_WIN_WAIT";
  case TD_MPI_WIN_TEST:
    return "TD_MPI_WIN_TEST";
  case TD_MPI_COMM_DUP:
    return "TD_MPI_COMM_DUP";
  case TD_MPI_COMM_SPLIT:
    return "TD_MPI_COMM_SPLIT";
  case TD_MPI_COMM_CREATE:
    return "TD_MPI_COMM_CREATE";
  case TD_MPI_COMM_FREE:
    return "TD_MPI_COMM_FREE";
  case TD_MPI_REQUEST_FREE:
    return "TD_MPI_REQUEST_FREE";
  case TD_MPI_CANCEL:
    return "TD_MPI_CANCEL";
  case TD_MPI_TYPE_CONTIGUOUS:
    return "TD_MPI_TYPE_CONTIGUOUS";
  case TD_MPI_TYPE_VECTOR:
    return "TD_MPI_TYPE_VECTOR";
  case TD_MPI_TYPE_HVECTOR:
    return "TD_MPI_TYPE_HVECTOR";
  case TD_MPI_TYPE_INDEXED:
    return "TD_MPI_TYPE_INDEXED";
  case TD_MPI_TYPE_HINDEXED:
    return "TD_MPI_TYPE_HINDEXED";
  case TD_MPI_TYPE_STRUCT:
    return "TD_MPI_TYPE_STRUCT";
  case TD_MPI_TYPE_CREATE_DLPACK:
    return "TD_MPI_TYPE_CREATE_DLPACK";
  case TD_MPI_TYPE_CREATE_SUBARRAY:
    return "TD_MPI_TYPE_CREATE_SUBARRAY";
  case TD_MPI_TYPE_CREATE_DARRAY:
    return "TD_MPI_TYPE_CREATE_DARRAY";
  case TD_MPI_TYPE_CREATE_RESIZED:
    return "TD_MPI_TYPE_CREATE_RESIZED";
  case TD_MPI_TYPE_CREATE_HINDEXED:
    return "TD_MPI_TYPE_CREATE_HINDEXED";
  case TD_MPI_TYPE_CREATE_HVECTOR:
    return "TD_MPI_TYPE_CREATE_HVECTOR";
  case TD_MPI_TYPE_GET_EXTENT:
    return "TD_MPI_TYPE_GET_EXTENT";
  case TD_MPI_TYPE_GET_TRUE_EXTENT:
    return "TD_MPI_TYPE_GET_TRUE_EXTENT";
  case TD_MPI_TYPE_SIZE:
    return "TD_MPI_TYPE_SIZE";
  case TD_MPI_TYPE_COMMIT:
    return "TD_MPI_TYPE_COMMIT";
  case TD_MPI_CART_CREATE:
    return "TD_MPI_CART_CREATE";
  case TD_MPI_CART_DIMS_CREATE:
    return "TD_MPI_CART_DIMS_CREATE";
  case TD_MPI_CART_GET:
    return "TD_MPI_CART_GET";
  case TD_MPI_CART_SHIFT:
    return "TD_MPI_CART_SHIFT";
  case TD_MPI_CART_COORDS:
    return "TD_MPI_CART_COORDS";
  case TD_MPI_CART_RANK:
    return "TD_MPI_CART_RANK";
  case TD_MPI_CART_SUB:
    return "TD_MPI_CART_SUB";
  case TD_MPI_DIST_GRAPH_CREATE:
    return "TD_MPI_DIST_GRAPH_CREATE";
  case TD_MPI_DIST_GRAPH_CREATE_ADJACENT:
    return "TD_MPI_DIST_GRAPH_CREATE_ADJACENT";
  case TD_MPI_DIST_GRAPH_NEIGHBORS:
    return "TD_MPI_DIST_GRAPH_NEIGHBORS";
  case TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT:
    return "TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT";
  case TD_MPI_GRAPH_CREATE:
    return "TD_MPI_GRAPH_CREATE";
  case TD_MPI_GRAPH_GET:
    return "TD_MPI_GRAPH_GET";
  case TD_MPI_GRAPH_NEIGHBORS:
    return "TD_MPI_GRAPH_NEIGHBORS";
  case TD_MPI_GRAPH_NEIGHBORS_COUNT:
    return "TD_MPI_GRAPH_NEIGHBORS_COUNT";
  case TD_MPI_GRAPH_DIMS_GET:
    return "TD_MPI_GRAPH_DIMS_GET";
  case TD_MPI_GRAPH_MAP:
    return "TD_MPI_GRAPH_MAP";
  case TD_KERNEL_SPIN_LOCK_INIT:
    return "TD_KERNEL_SPIN_LOCK_INIT";
  case TD_KERNEL_SPIN_LOCK:
    return "TD_KERNEL_SPIN_LOCK";
  case TD_KERNEL_SPIN_UNLOCK:
    return "TD_KERNEL_SPIN_UNLOCK";
  case TD_KERNEL_SPIN_TRYLOCK:
    return "TD_KERNEL_SPIN_TRYLOCK";
  case TD_KERNEL_MUTEX_INIT:
    return "TD_KERNEL_MUTEX_INIT";
  case TD_KERNEL_MUTEX_LOCK:
    return "TD_KERNEL_MUTEX_LOCK";
  case TD_KERNEL_MUTEX_UNLOCK:
    return "TD_KERNEL_MUTEX_UNLOCK";
  case TD_KERNEL_MUTEX_TRYLOCK:
    return "TD_KERNEL_MUTEX_TRYLOCK";
  case TD_KERNEL_SEMA_INIT:
    return "TD_KERNEL_SEMA_INIT";
  case TD_KERNEL_DOWN:
    return "TD_KERNEL_DOWN";
  case TD_KERNEL_UP:
    return "TD_KERNEL_UP";
  case TD_KERNEL_READ_LOCK:
    return "TD_KERNEL_READ_LOCK";
  case TD_KERNEL_READ_UNLOCK:
    return "TD_KERNEL_READ_UNLOCK";
  case TD_KERNEL_WRITE_LOCK:
    return "TD_KERNEL_WRITE_LOCK";
  case TD_KERNEL_WRITE_UNLOCK:
    return "TD_KERNEL_WRITE_UNLOCK";
  case TD_KERNEL_DOWN_READ:
    return "TD_KERNEL_DOWN_READ";
  case TD_KERNEL_UP_READ:
    return "TD_KERNEL_UP_READ";
  case TD_KERNEL_DOWN_WRITE:
    return "TD_KERNEL_DOWN_WRITE";
  case TD_KERNEL_UP_WRITE:
    return "TD_KERNEL_UP_WRITE";
  case TD_KERNEL_INIT_RWSEM:
    return "TD_KERNEL_INIT_RWSEM";
  case TD_KERNEL_RCU_READ_LOCK:
    return "TD_KERNEL_RCU_READ_LOCK";
  case TD_KERNEL_RCU_READ_UNLOCK:
    return "TD_KERNEL_RCU_READ_UNLOCK";
  case TD_KERNEL_SYNCHRONIZE_RCU:
    return "TD_KERNEL_SYNCHRONIZE_RCU";
  case TD_KERNEL_CALL_RCU:
    return "TD_KERNEL_CALL_RCU";
  case TD_KERNEL_RCU_DEREFERENCE:
    return "TD_KERNEL_RCU_DEREFERENCE";
  case TD_KERNEL_RCU_ASSIGN_POINTER:
    return "TD_KERNEL_RCU_ASSIGN_POINTER";
  case TD_KERNEL_SEQLOCK_INIT:
    return "TD_KERNEL_SEQLOCK_INIT";
  case TD_KERNEL_READ_SEQBEGIN:
    return "TD_KERNEL_READ_SEQBEGIN";
  case TD_KERNEL_READ_SEQRETRY:
    return "TD_KERNEL_READ_SEQRETRY";
  case TD_KERNEL_WRITE_SEQLOCK:
    return "TD_KERNEL_WRITE_SEQLOCK";
  case TD_KERNEL_WRITE_SEQUNLOCK:
    return "TD_KERNEL_WRITE_SEQUNLOCK";
  case TD_KERNEL_INIT_COMPLETION:
    return "TD_KERNEL_INIT_COMPLETION";
  case TD_KERNEL_WAIT_FOR_COMPLETION:
    return "TD_KERNEL_WAIT_FOR_COMPLETION";
  case TD_KERNEL_COMPLETE:
    return "TD_KERNEL_COMPLETE";
  case TD_KERNEL_COMPLETE_ALL:
    return "TD_KERNEL_COMPLETE_ALL";
  case TD_KERNEL_INIT_WAITQUEUE_HEAD:
    return "TD_KERNEL_INIT_WAITQUEUE_HEAD";
  case TD_KERNEL_WAIT_EVENT:
    return "TD_KERNEL_WAIT_EVENT";
  case TD_KERNEL_WAKE_UP:
    return "TD_KERNEL_WAKE_UP";
  case TD_KERNEL_PREPARE_TO_WAIT:
    return "TD_KERNEL_PREPARE_TO_WAIT";
  case TD_KERNEL_FINISH_WAIT:
    return "TD_KERNEL_FINISH_WAIT";
  case TD_KERNEL_MEMORY_BARRIER:
    return "TD_KERNEL_MEMORY_BARRIER";
  default:
    return "<unknown TD_TYPE>";
  }
}
