#include "Checker/Pulse/Interproc/PulseModels.h"

#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLatentIssue.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <cctype>
#include <cstring>
#include <limits>
#include <optional>

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

namespace pulse {

namespace {

static constexpr unsigned kWeakPtrField = 0x7FFFFFF0u;
static constexpr unsigned kWeakCbField = 0x7FFFFFF1u;
static constexpr unsigned kSharedCbField = 0x7FFFFFF2u;

// Smart pointer layout helper: store raw pointers at
// `owner --Field(0, 0)--> slot_cell --Dereference--> raw_ptr`
// matching libstdc++/libc++ IR where `GEP(this, 0, 0)` yields `&_M_ptr` (T**)
// and `load` reads the underlying `T*`. This prevents unmodeled methods like
// `__shared_ptr::operator bool()` (`gep %this, 0, 0; load`) from dereferencing
// `nullptr` when the smart pointer holds null.
static Address *readSmartPtrSlot(AbductiveDomain &astate, AbstractValue owner) {
  AbstractValue owner_c = astate.getCanonical(owner);
  Address *cell = astate.getPostHeap().findEdge(owner_c, Access::field(0, 0));
  if (!cell)
    return nullptr;
  AbstractValue cell_c = astate.getCanonical(cell->addr);
  return astate.getPostHeap().findEdge(cell_c, Access(AccessKind::Dereference));
}

static void writeSmartPtrSlot(AbductiveDomain &astate, AbstractValue owner,
                              const Address &val, AbstractValueFactory &factory,
                              const llvm::Instruction *loc) {
  AbstractValue owner_c = astate.getCanonical(owner);
  AbstractValue cell_c;
  if (Address *existing =
          astate.getPostHeap().findEdge(owner_c, Access::field(0, 0))) {
    AbstractValue ec = astate.getCanonical(existing->addr);
    if (!astate.getPathFormula().isNull(ec) &&
        !astate.getPostAttrs().has(ec, Attribute::Null)) {
      cell_c = ec;
    }
  }
  if (cell_c.getId() == 0) {
    cell_c = factory.createFresh(loc);
    astate.getPostAttrs().add(cell_c, Attribute::Allocated);
    (void)astate.getPathFormula().addNonNull(cell_c);
    astate.getPostHeap().addEdge(owner_c, Access::field(0, 0), Address(cell_c));
  }
  astate.getPostAttrs().remove(cell_c, Attribute::Uninitialized);
  astate.markWrittenTo(cell_c, loc);
  astate.getPostHeap().addEdge(cell_c, Access(AccessKind::Dereference), val);
}

static Address readOrAbduceSmartPtrSlot(AbductiveDomain &astate,
                                        AbstractValue owner,
                                        AbstractValueFactory &factory,
                                        const llvm::Instruction *loc) {
  AbstractValue owner_c = astate.getCanonical(owner);
  AbstractValue cell_c;
  if (Address *existing =
          astate.getPostHeap().findEdge(owner_c, Access::field(0, 0))) {
    cell_c = astate.getCanonical(existing->addr);
  } else {
    cell_c = factory.createFresh(loc);
    Address cell_addr(cell_c);
    astate.getPostHeap().addEdge(owner_c, Access::field(0, 0), cell_addr);
    astate.abduceToPre(owner_c, Access::field(0, 0), cell_addr);
  }
  if (Address *raw = astate.getPostHeap().findEdge(
          cell_c, Access(AccessKind::Dereference))) {
    return *raw;
  }
  AbstractValue fresh = factory.createFresh(loc);
  Address ret_addr(fresh);
  astate.getPostHeap().addEdge(cell_c, Access(AccessKind::Dereference),
                               ret_addr);
  astate.abduceToPre(cell_c, Access(AccessKind::Dereference), ret_addr);
  return ret_addr;
}

static bool globMatch(const std::string &pattern, const std::string &text) {
  size_t p = 0, t = 0, star = std::string::npos, match = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      match = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++match;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

static std::optional<uint64_t> getConstUInt64(const llvm::Value *v) {
  if (!v)
    return std::nullopt;
  if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    if (CI->getBitWidth() <= 64 && !CI->isNegative()) {
      return CI->getZExtValue();
    }
  }
  return std::nullopt;
}

static std::optional<uint64_t> mulConstU64(uint64_t a, uint64_t b) {
  __int128 prod = static_cast<__int128>(a) * static_cast<__int128>(b);
  if (prod < 0 || prod > std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(prod);
}

static void reportOutOfBoundsAccess(const AbductiveDomain &astate,
                                    const llvm::Instruction *loc,
                                    const Address &addr, const char *detail) {
  AbstractValue canon = astate.getCanonical(addr.addr);
  if (!LatentIssue::isManifest(OperationResult::OutOfBounds, astate, canon)) {
    return;
  }
  Trace trace = Trace::fromValueHistory(addr.history);
  trace.addEvent(loc, detail);
  auto diag = std::make_unique<AccessToInvalidAddress>(
      loc, "Out of bounds access", detail,
      "Ensure indices and lengths stay within the allocated object",
      IssueType::OutOfBounds, std::move(trace));
  DiagnosticManager::getInstance().report(std::move(diag));
}

static void checkLengthAgainstAllocation(AbductiveDomain &astate,
                                         const Address &addr,
                                         const llvm::Value *len_value,
                                         const llvm::Instruction *loc,
                                         const char *detail) {
  auto len_opt = getConstUInt64(len_value);
  if (!len_opt)
    return;
  AbstractValue canon = astate.getCanonical(addr.addr);
  auto size_opt = astate.getAllocationSize(canon);
  if (!size_opt)
    return;
  if (*len_opt > *size_opt) {
    reportOutOfBoundsAccess(astate, loc, addr, detail);
    astate.getPostAttrs().add(canon, Attribute::OutOfBounds);
  }
}

static void reportStackEscapeOnCall(AbductiveDomain &astate,
                                    const llvm::CallBase *call,
                                    const Address &escaped_addr,
                                    const char *reason) {
  AbstractValue canon = astate.getCanonical(escaped_addr.addr);
  if (!astate.getPostAttrs().has(canon, Attribute::Stack)) {
    return;
  }
  if (!LatentIssue::isManifest(astate)) {
    return;
  }

  Trace trace = Trace::fromValueHistory(escaped_addr.history);
  trace.addEvent(call, reason);
  auto diag = std::make_unique<StackVariableAddressEscape>(
      call, canon, "Stack address escapes via call",
      "Do not pass addresses of local variables to APIs that store them for "
      "later use.",
      std::move(trace));
  DiagnosticManager::getInstance().report(std::move(diag));
}

static void markBufferInitialized(AbductiveDomain &astate, AbstractValue av,
                                  const llvm::Instruction *loc) {
  AbstractValue c = astate.getCanonical(av);
  AbstractValue root = astate.getAllocationRoot(c);
  astate.getPostAttrs().remove(c, Attribute::Uninitialized);
  astate.markWrittenTo(c, loc);
  astate.getPostAttrs().remove(root, Attribute::Uninitialized);
  astate.markWrittenTo(root, loc);
}

static ExecutionDomain reportModelAccessError(PulseChecker &checker,
                                              AbductiveDomain &astate,
                                              OperationResult err,
                                              const Address &addr,
                                              const llvm::CallBase *call,
                                              const char *detail) {
  Trace trace = Trace::fromValueHistory(addr.history);
  trace.addEvent(call, detail);
  AbstractValue canon = astate.getCanonical(addr.addr);
  if (LatentIssue::isManifest(err, astate, canon)) {
    checker.reportBug(err, call, canon, trace, &astate);
    return ExecutionDomain::abortProgram(
        std::make_unique<AbductiveDomain>(astate.clone()), err,
        std::move(trace));
  }
  return ExecutionDomain::latentAbortProgram(
      std::make_unique<AbductiveDomain>(astate.clone()), nullptr, err,
      std::move(trace));
}

} // namespace

PulseModels::PulseModels(PulseChecker &checker)
    : checker_(checker), ops_(checker.getOperations()),
      factory_(checker.getFactory()) {
  registerStandardModels();
  registerCppModels();
}

const PulseModel *
PulseModels::findModelByName(const std::string &funcName) const {
  auto cit = name_model_cache_.find(funcName);
  if (cit != name_model_cache_.end()) {
    return cit->second;
  }

  auto it = models_.find(funcName);
  if (it != models_.end()) {
    name_model_cache_.emplace(funcName, &it->second);
    return &it->second;
  }

  const std::string demangled = llvm::demangle(funcName);
  const std::string demangled_no_args =
      demangled.substr(0, demangled.find('('));

  for (const auto &pm : pattern_models_) {
    if (!pm.mangled_glob.empty() && globMatch(pm.mangled_glob, funcName)) {
      name_model_cache_.emplace(funcName, &pm.model);
      return &pm.model;
    }
    if (!pm.class_substr.empty() && !pm.method_substr.empty()) {
      if (demangled_no_args.find(pm.class_substr) != std::string::npos &&
          demangled_no_args.find(pm.method_substr) != std::string::npos) {
        name_model_cache_.emplace(funcName, &pm.model);
        return &pm.model;
      }
    }
  }

  name_model_cache_.emplace(funcName, nullptr);
  return nullptr;
}

const PulseModel *PulseModels::findModel(const llvm::Function *func) const {
  if (!func || !func->hasName()) {
    return nullptr;
  }
  auto it = fn_model_cache_.find(func);
  if (it != fn_model_cache_.end()) {
    return it->second;
  }
  const PulseModel *found = findModelByName(func->getName().str());
  fn_model_cache_.emplace(func, found);
  return found;
}

ModelResult PulseModels::dispatch(const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred,
                                  const llvm::Function *resolved_callee) {
  const llvm::Function *func =
      resolved_callee ? resolved_callee : call->getCalledFunction();
  if (!func && call && call->getCalledOperand()) {
    func = llvm::dyn_cast<llvm::Function>(
        call->getCalledOperand()->stripPointerCastsAndAliases());
  }
  if (const PulseModel *model = findModel(func)) {
    return (*model)(checker_, call, state, pred);
  }

  return ModelResult::notHandled();
}

bool PulseModels::hasModel(const llvm::Function *func) const {
  return findModel(func) != nullptr;
}

bool PulseModels::isTaintSource(const std::string &func_name) const {
  return !TaintOperations::getConfig().matchSourcesByName(func_name).empty();
}

bool PulseModels::isTaintSink(const std::string &func_name) const {
  return !TaintOperations::getConfig().matchSinksByName(func_name).empty();
}

bool PulseModels::isTaintSanitizer(const std::string &func_name) const {
  return !TaintOperations::getConfig().matchSanitizersByName(func_name).empty();
}

void PulseModels::registerStandardModels() {
  models_["malloc"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelMalloc(call, state, pred, AllocatorKind::CMalloc);
  };
  models_["calloc"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelCalloc(call, state, pred);
  };
  models_["posix_memalign"] = [this](PulseChecker &, const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
    return modelPosixMemalign(call, state, pred);
  };
  models_["aligned_alloc"] = [this](PulseChecker &, const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
    return modelAlignedAlloc(call, state, pred);
  };
  models_["mmap"] = [this](PulseChecker &, const llvm::CallBase *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelMmap(call, state, pred);
  };
  models_["munmap"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelMunmap(call, state, pred);
  };
  models_["free"] = [this](PulseChecker &, const llvm::CallBase *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelFree(call, state, pred, InvalidationKind::CFree);
  };
  models_["realloc"] = [this](PulseChecker &, const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelRealloc(call, state, pred);
  };

  // Exception runtime
  models_["__cxa_throw"] = [this](PulseChecker &, const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred) {
    return modelCxaThrow(call, state, pred);
  };
  models_["_Unwind_Resume"] = [this](PulseChecker &, const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
    return modelCxaThrow(call, state, pred);
  };

  models_["pthread_create"] = [this](PulseChecker &, const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
    return modelPthreadCreate(call, state, pred);
  };
  models_["thrd_create"] = [this](PulseChecker &, const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred) {
    return modelThrdCreate(call, state, pred);
  };
  models_["dispatch_async_f"] =
      [this](PulseChecker &, const llvm::CallBase *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelDispatchAsyncF(call, state, pred);
      };

  // File & socket resources
  models_["fopen"] = [this](PulseChecker &, const llvm::CallBase *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred, AllocatorKind::FileHandle);
  };
  models_["fclose"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFileClose(call, state, pred);
  };
  models_["open"] = [this](PulseChecker &, const llvm::CallBase *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred, AllocatorKind::FileHandle);
  };
  models_["close"] = [this](PulseChecker &, const llvm::CallBase *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFileClose(call, state, pred);
  };
  models_["fdopen"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred, AllocatorKind::FileHandle);
  };
  models_["opendir"] = [this](PulseChecker &, const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred, AllocatorKind::FileHandle);
  };
  models_["closedir"] = [this](PulseChecker &, const llvm::CallBase *call,
                               ExecutionDomain &state,
                               const llvm::BasicBlock *pred) {
    return modelFileClose(call, state, pred);
  };
  models_["socket"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelSocket(call, state, pred);
  };
  models_["accept"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred, AllocatorKind::Socket);
  };
  models_["pipe"] = [this](PulseChecker &, const llvm::CallBase *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelPipe(call, state, pred);
  };
  models_["dup"] = [this](PulseChecker &, const llvm::CallBase *call,
                          ExecutionDomain &state,
                          const llvm::BasicBlock *pred) {
    return modelFileOpen(call, state, pred, AllocatorKind::FileHandle);
  };
  models_["dup2"] = [this](PulseChecker &, const llvm::CallBase *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelDup2(call, state, pred);
  };

  // String & memory operations
  models_["strlen"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrlen(call, state, pred);
  };
  models_["strcpy"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStringCopy(call, state, pred);
  };
  models_["strncpy"] = [this](PulseChecker &, const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelStringCopy(call, state, pred);
  };
  models_["strcat"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrcat(call, state, pred, false);
  };
  models_["strncat"] = [this](PulseChecker &, const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelStrcat(call, state, pred, true);
  };
  models_["sprintf"] = [this](PulseChecker &, const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelSprintf(call, state, pred, false);
  };
  models_["snprintf"] = [this](PulseChecker &, const llvm::CallBase *call,
                               ExecutionDomain &state,
                               const llvm::BasicBlock *pred) {
    return modelSprintf(call, state, pred, true);
  };
  models_["strdup"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrdup(call, state, pred);
  };
  models_["strchr"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrchr(call, state, pred);
  };
  models_["strstr"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrstr(call, state, pred);
  };
  models_["strcmp"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrcmp(call, state, pred, false, false, false);
  };
  models_["strncmp"] = [this](PulseChecker &, const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelStrcmp(call, state, pred, true, false, false);
  };
  models_["strcasecmp"] = [this](PulseChecker &, const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred) {
    return modelStrcmp(call, state, pred, false, true, false);
  };
  models_["strncasecmp"] = [this](PulseChecker &, const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred) {
    return modelStrcmp(call, state, pred, true, true, false);
  };
  models_["memcmp"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelStrcmp(call, state, pred, true, false, true);
  };
  models_["memcpy"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelMemcpy(call, state, pred);
  };
  models_["memmove"] = [this](PulseChecker &, const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred) {
    return modelMemmove(call, state, pred);
  };
  models_["memset"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelMemset(call, state, pred);
  };
  models_["bzero"] = [this](PulseChecker &, const llvm::CallBase *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelMemset(call, state, pred);
  };

  // I/O
  models_["read"] = [this](PulseChecker &, const llvm::CallBase *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelRead(call, state, pred);
  };
  models_["write"] = [this](PulseChecker &, const llvm::CallBase *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelWrite(call, state, pred);
  };
  models_["fread"] = [this](PulseChecker &, const llvm::CallBase *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFread(call, state, pred);
  };
  models_["fwrite"] = [this](PulseChecker &, const llvm::CallBase *call,
                             ExecutionDomain &state,
                             const llvm::BasicBlock *pred) {
    return modelFwrite(call, state, pred);
  };
  models_["fgets"] = [this](PulseChecker &, const llvm::CallBase *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelFgets(call, state, pred);
  };
  models_["gets"] = [this](PulseChecker &, const llvm::CallBase *call,
                           ExecutionDomain &state,
                           const llvm::BasicBlock *pred) {
    return modelGets(call, state, pred);
  };

  // Locking
  models_["pthread_mutex_lock"] =
      [this](PulseChecker &, const llvm::CallBase *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelLock(call, state, pred);
      };
  models_["pthread_mutex_unlock"] =
      [this](PulseChecker &, const llvm::CallBase *call, ExecutionDomain &state,
             const llvm::BasicBlock *pred) {
        return modelUnlock(call, state, pred);
      };
}

void PulseModels::registerCppModels() {
  auto cppNew = [this](PulseChecker &, const llvm::CallBase *call,
                       ExecutionDomain &state, const llvm::BasicBlock *pred) {
    return modelMalloc(call, state, pred, AllocatorKind::CppNew);
  };
  auto cppNewArray = [this](PulseChecker &, const llvm::CallBase *call,
                            ExecutionDomain &state,
                            const llvm::BasicBlock *pred) {
    return modelMalloc(call, state, pred, AllocatorKind::CppNewArray);
  };
  auto cppDelete = [this](PulseChecker &, const llvm::CallBase *call,
                          ExecutionDomain &state,
                          const llvm::BasicBlock *pred) {
    return modelFree(call, state, pred, InvalidationKind::CppDelete);
  };
  auto cppDeleteArray = [this](PulseChecker &, const llvm::CallBase *call,
                               ExecutionDomain &state,
                               const llvm::BasicBlock *pred) {
    return modelFree(call, state, pred, InvalidationKind::CppDeleteArray);
  };

  for (const char *name : {"_Znwm", "_Znwj", "_ZnwmRKSt9nothrow_t"}) {
    models_[name] = cppNew;
  }
  for (const char *name : {"_Znam", "_Znaj", "_ZnamRKSt9nothrow_t"}) {
    models_[name] = cppNewArray;
  }
  for (const char *name : {"_ZdlPv", "_ZdlPvm", "_ZdlPvj"}) {
    models_[name] = cppDelete;
  }
  for (const char *name : {"_ZdaPv", "_ZdaPvm", "_ZdaPvj"}) {
    models_[name] = cppDeleteArray;
  }

  // std::move / std::forward (identity pass-through)
  pattern_models_.push_back(
      {"std::", "move<", "_ZSt4move*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdIdentityPassThrough(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZNSt3__14move*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdIdentityPassThrough(call, state, pred);
       }});
  pattern_models_.push_back(
      {"std::", "forward<", "_ZSt7forward*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdIdentityPassThrough(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZNSt3__17forward*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdIdentityPassThrough(call, state, pred);
       }});

  // std::make_unique / std::make_shared (must precede unique_ptr/shared_ptr!)
  pattern_models_.push_back(
      {"std::", "make_unique", "_ZSt11make_unique*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMakeUnique(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZNSt3__111make_unique*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMakeUnique(call, state, pred);
       }});
  pattern_models_.push_back(
      {"std::", "make_shared", "_ZSt11make_shared*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMakeShared(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZNSt3__111make_shared*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMakeShared(call, state, pred);
       }});

  // std::weak_ptr (must precede std::shared_ptr!)
  pattern_models_.push_back(
      {"weak_ptr<", "::lock", "_ZN*weak_ptr*4lock*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdWeakPtrLock(call, state, pred);
       }});
  pattern_models_.push_back(
      {"weak_ptr<", "::expired", "_ZN*weak_ptr*expired*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdWeakPtrExpired(call, state, pred);
       }});
  pattern_models_.push_back(
      {"weak_ptr<", "::reset", "_ZN*weak_ptr*reset*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdWeakPtrReset(call, state, pred);
       }});
  pattern_models_.push_back(
      {"weak_ptr<", "::~weak_ptr", "_ZN*weak_ptr*D1Ev",
       [](PulseChecker &, const llvm::CallBase *call,
          ExecutionDomain &state, const llvm::BasicBlock *pred) {
         (void)call;
         (void)pred;
         return ModelResult::success(std::move(state));
       }});
  pattern_models_.push_back(
      {"weak_ptr<", "::~weak_ptr", "_ZN*weak_ptr*D2Ev",
       [](PulseChecker &, const llvm::CallBase *call,
          ExecutionDomain &state, const llvm::BasicBlock *pred) {
         (void)call;
         (void)pred;
         return ModelResult::success(std::move(state));
       }});
  pattern_models_.push_back(
      {"weak_ptr<", "::operator=", "_ZN*weak_ptr*aSE*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdWeakPtrCtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"weak_ptr<", "::weak_ptr", "_ZN*weak_ptr*C1*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdWeakPtrCtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"weak_ptr<", "::weak_ptr", "_ZN*weak_ptr*C2*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdWeakPtrCtor(call, state, pred);
       }});

  // std::vector
  pattern_models_.push_back(
      {"vector<", "::push_back", "_ZN*vector*push_back*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorPushBack(call, state, pred);
       }});
  pattern_models_.push_back(
      {"vector<", "::emplace_back", "_ZN*vector*emplace_back*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorPushBack(call, state, pred);
       }});
  pattern_models_.push_back(
      {"vector<", "::reserve", "_ZN*vector*reserve*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorPushBack(call, state, pred);
       }});
  pattern_models_.push_back(
      {"vector<", "::clear", "_ZN*vector*clear*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorClear(call, state, pred);
       }});
  pattern_models_.push_back(
      {"vector<", "::data", "_ZN*vector*data*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorData(call, state, pred);
       }});
  pattern_models_.push_back(
      {"vector<", "::operator[]", "_ZN*vector*ixE*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorAccess(call, state, pred);
       }});
  pattern_models_.push_back(
      {"vector<", "::at", "_ZN*vector*2atE*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorAccess(call, state, pred);
       }});

  // std::unique_ptr (move ctor/assign MUST precede generic ctor!)
  pattern_models_.push_back(
      {"unique_ptr<", "::operator=", "_ZN*unique_ptr*aSEO*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrMove(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZN*unique_ptr*C1EO*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrMove(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZN*unique_ptr*C2EO*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrMove(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::release", "_ZN*unique_ptr*release*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrRelease(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::reset", "_ZN*unique_ptr*reset*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrReset(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::get", "_ZN*unique_ptr*3get*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrGet(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::operator->", "_ZN*unique_ptr*ptEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrGet(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::operator*", "_ZN*unique_ptr*deEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrGet(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::operator bool", "_ZN*unique_ptr*cvbEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSmartPtrBool(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::~unique_ptr", "_ZN*unique_ptr*D1Ev",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrDtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::~unique_ptr", "_ZN*unique_ptr*D2Ev",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrDtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::unique_ptr", "_ZN*unique_ptr*C1E*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrCtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"unique_ptr<", "::unique_ptr", "_ZN*unique_ptr*C2E*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdUniquePtrCtor(call, state, pred);
       }});

  // std::shared_ptr (copy assign & copy ctor must precede generic ctor!)
  pattern_models_.push_back(
      {"shared_ptr<", "::operator=", "_ZN*shared_ptr*aSERKS*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrCopyAssign(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZN*shared_ptr*C1ERKS*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrCopyCtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZN*shared_ptr*C2ERKS*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrCopyCtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::get", "_ZN*shared_ptr*3get*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrGet(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::operator->", "_ZN*shared_ptr*ptEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrGet(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::operator*", "_ZN*shared_ptr*deEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrGet(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::operator bool", "_ZN*shared_ptr*cvbEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSmartPtrBool(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::reset", "_ZN*shared_ptr*reset*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrReset(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::~shared_ptr", "_ZN*shared_ptr*D1Ev",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrDtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::~shared_ptr", "_ZN*shared_ptr*D2Ev",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrDtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::shared_ptr", "_ZN*shared_ptr*C1E*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrCtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"shared_ptr<", "::shared_ptr", "_ZN*shared_ptr*C2E*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdSharedPtrCtor(call, state, pred);
       }});

  // std::optional
  pattern_models_.push_back(
      {"optional<", "::reset", "_ZN*optional*reset*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdOptionalReset(call, state, pred);
       }});
  pattern_models_.push_back(
      {"optional<", "::value", "_ZN*optional*value*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdOptionalValue(call, state, pred);
       }});
  pattern_models_.push_back(
      {"optional<", "::operator*", "_ZN*optional*deEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdOptionalValue(call, state, pred);
       }});
  pattern_models_.push_back(
      {"optional<", "::has_value", "_ZN*optional*has_value*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdOptionalHasValue(call, state, pred);
       }});

  // std::basic_string_view / std::string_view (must precede basic_string!)
  pattern_models_.push_back(
      {"string_view", "::data", "_ZN*string_view*4data*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringViewData(call, state, pred);
       }});
  pattern_models_.push_back(
      {"string_view", "::begin", "_ZN*string_view*5begin*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringViewData(call, state, pred);
       }});
  pattern_models_.push_back(
      {"string_view", "::basic_string_view", "_ZN*string_view*C1*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringViewCtor(call, state, pred);
       }});
  pattern_models_.push_back(
      {"string_view", "::string_view", "_ZN*string_view*C2*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringViewCtor(call, state, pred);
       }});

  // std::basic_string
  pattern_models_.push_back(
      {"basic_string<", "::c_str", "_ZN*basic_string*c_str*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringCStr(call, state, pred);
       }});
  pattern_models_.push_back(
      {"basic_string<", "::data", "_ZN*basic_string*4data*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringCStr(call, state, pred);
       }});
  pattern_models_.push_back(
      {"basic_string<", "::append", "_ZN*basic_string*append*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringMutate(call, state, pred);
       }});
  pattern_models_.push_back(
      {"basic_string<", "::clear", "_ZN*basic_string*clear*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdStringMutate(call, state, pred);
       }});
  pattern_models_.push_back(
      {"basic_string<", "::~basic_string", "_ZN*basic_string*D1Ev",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorClear(call, state, pred);
       }});
  pattern_models_.push_back(
      {"basic_string<", "::~basic_string", "_ZN*basic_string*D2Ev",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVectorClear(call, state, pred);
       }});

  // std::map / std::unordered_map
  pattern_models_.push_back(
      {"map<", "::operator[]", "_ZN*map*ixE*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMapAccess(call, state, pred);
       }});
  pattern_models_.push_back(
      {"map<", "::find", "_ZN*map*4find*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMapAccess(call, state, pred);
       }});
  pattern_models_.push_back(
      {"map<", "::at", "_ZN*map*2atE*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMapAccess(call, state, pred);
       }});
  pattern_models_.push_back(
      {"map<", "::erase", "_ZN*map*erase*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMapErase(call, state, pred);
       }});
  pattern_models_.push_back(
      {"map<", "::clear", "_ZN*map*clear*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdMapErase(call, state, pred);
       }});

  // std::variant / std::get
  pattern_models_.push_back(
      {"std::", "get<", "_ZSt3get*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVariantGet(call, state, pred);
       }});
  pattern_models_.push_back(
      {"", "", "_ZNSt3__13get*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelStdVariantGet(call, state, pred);
       }});

  // std::mutex & std::lock_guard
  pattern_models_.push_back(
      {"mutex", "::lock", "_ZN*mutex*4lockEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelLock(call, state, pred);
       }});
  pattern_models_.push_back(
      {"mutex", "::unlock", "_ZN*mutex*6unlockEv",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelUnlock(call, state, pred);
       }});
  pattern_models_.push_back(
      {"lock_guard<", "::lock_guard", "_ZN*lock_guard*C*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelLock(call, state, pred);
       }});
  pattern_models_.push_back(
      {"lock_guard<", "::~lock_guard", "_ZN*lock_guard*D*",
       [this](PulseChecker &, const llvm::CallBase *call,
              ExecutionDomain &state, const llvm::BasicBlock *pred) {
         return modelUnlock(call, state, pred);
       }});
}

//===----------------------------------------------------------------------===//
// Model Implementations
//===----------------------------------------------------------------------===//

ModelResult PulseModels::modelCxaThrow(const llvm::CallBase *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate) {
    return ModelResult::success(std::move(state));
  }
  auto exc_astate = std::make_unique<AbductiveDomain>(astate->clone());
  std::optional<AbstractValue> exc_val;
  std::string exc_type;
  if (call->arg_size() >= 1) {
    if (auto obj_opt = ops_.eval(*exc_astate, call->getArgOperand(0), call,
                                 pred)) {
      AbstractValue c = exc_astate->getCanonical(obj_opt->addr);
      exc_val = c;
      if (auto dt = exc_astate->getDynamicType(c)) {
        exc_type = *dt;
      }
    }
  }
  if (call->arg_size() >= 2) {
    std::string rtti_name = detail::normalizeRttiTypeName(call->getArgOperand(1));
    if (!rtti_name.empty()) {
      exc_type = std::move(rtti_name);
    }
  }
  if (!exc_val) {
    AbstractValue fresh_exc = factory_.createFresh(call);
    exc_astate->addNonNull(fresh_exc);
    exc_val = fresh_exc;
  }
  if (!exc_type.empty() && exc_val) {
    exc_astate->setDynamicType(*exc_val, exc_type);
  }
  return ModelResult::success({ExecutionDomain::exceptionRaised(
      std::move(exc_astate), exc_val, std::move(exc_type))});
}

ModelResult PulseModels::modelPthreadCreate(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  if (call->arg_size() < 4)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto arg_opt = ops_.eval(*astate, call->getArgOperand(3), call, pred);
  if (arg_opt) {
    reportStackEscapeOnCall(*astate, call, *arg_opt,
                            "Passing stack-derived context to pthread_create "
                            "(escapes to new thread)");
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelThrdCreate(const llvm::CallBase *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto arg_opt = ops_.eval(*astate, call->getArgOperand(2), call, pred);
  if (arg_opt) {
    reportStackEscapeOnCall(
        *astate, call, *arg_opt,
        "Passing stack-derived context to thrd_create (escapes to new thread)");
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelDispatchAsyncF(const llvm::CallBase *call,
                                             ExecutionDomain &state,
                                             const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto ctx_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (ctx_opt) {
    reportStackEscapeOnCall(*astate, call, *ctx_opt,
                            "Passing stack-derived context to dispatch_async_f "
                            "(escapes asynchronously)");
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelMalloc(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred,
                                     AllocatorKind kind) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  ExecutionDomain failure_state;
  const bool fork_null =
      options::modelMallocFailure() && kind == AllocatorKind::CMalloc;
  if (fork_null) {
    failure_state = state.clone();
  }

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ops_.allocate(*astate, ret_val, call, kind);
  astate->addNonNull(ret_val);
  ret_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction());
  if (call->arg_size() > 0) {
    if (auto size_opt = getConstUInt64(call->getArgOperand(0))) {
      astate->setAllocationSize(ret_val, *size_opt);
    } else if (auto sym_opt =
                   ops_.eval(*astate, call->getArgOperand(0), call, pred)) {
      astate->setSymbolicAllocationSize(ret_val,
                                        astate->getCanonical(sym_opt->addr));
    }
  }
  astate->getPostStack().add(call, ret_addr);

  std::vector<ExecutionDomain> out;
  out.push_back(std::move(state));

  if (fork_null && failure_state.getAstate()) {
    auto *fail_astate = failure_state.getAstate();
    AbstractValue null_val = factory_.createFresh(call);
    Address null_addr(null_val);
    null_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, call,
                               call->getFunction(), "malloc returned null");
    fail_astate->getPostAttrs().add(null_val, Attribute::Null);
    (void)fail_astate->getPathFormula().addNull(null_val);
    fail_astate->getPostStack().add(call, null_addr);
    out.push_back(std::move(failure_state));
  }

  return ModelResult::success(std::move(out));
}

ModelResult PulseModels::modelPosixMemalign(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto out_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (out_opt) {
    AbstractValue mem = factory_.createFresh(call);
    ops_.allocate(*astate, mem, call, AllocatorKind::CMalloc);
    astate->addNonNull(mem);
    if (auto sz = getConstUInt64(call->getArgOperand(2))) {
      astate->setAllocationSize(mem, *sz);
    }
    ops_.writeDeref(*astate, *out_opt, Address(mem), call);
  }
  AbstractValue zero = factory_.createFresh(call);
  (void)astate->getPathFormula().addBounds(zero, 0, 0);
  astate->getPostStack().add(call, Address(zero));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelAlignedAlloc(const llvm::CallBase *call,
                                           ExecutionDomain &state,
                                           const llvm::BasicBlock *pred) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  AbstractValue ret_val = factory_.createFresh(call);
  ops_.allocate(*astate, ret_val, call, AllocatorKind::CMalloc);
  astate->addNonNull(ret_val);
  if (call->arg_size() >= 2) {
    if (auto sz = getConstUInt64(call->getArgOperand(1))) {
      astate->setAllocationSize(ret_val, *sz);
    }
  }
  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelMmap(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  AbstractValue ret_val = factory_.createFresh(call);
  ops_.allocate(*astate, ret_val, call, AllocatorKind::CustomResource);
  astate->addNonNull(ret_val);
  if (call->arg_size() >= 2) {
    if (auto sz = getConstUInt64(call->getArgOperand(1))) {
      astate->setAllocationSize(ret_val, *sz);
    }
  }
  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelMunmap(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  return modelFree(call, state, pred, InvalidationKind::CFree);
}

ModelResult PulseModels::modelFree(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred,
                                   InvalidationKind inv_kind) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  const llvm::Value *arg = call->getArgOperand(0);
  if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(arg)) {
    arg = BC->getOperand(0);
  } else if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(arg)) {
    if (CE->getOpcode() == llvm::Instruction::BitCast) {
      arg = CE->getOperand(0);
    }
  }

  auto ptr_opt = ops_.eval(*astate, arg, call, pred);
  if (!ptr_opt) {
    ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
    if (!ptr_opt)
      return ModelResult::success(std::move(state));
  }

  AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);

  if (astate->getPathFormula().isNull(canon_ptr) ||
      (astate->getPostAttrs().has(canon_ptr, Attribute::Null) &&
       !astate->getPathFormula().isNonNull(canon_ptr))) {
    return ModelResult::success(std::move(state));
  }

  if (astate->getPostAttrs().has(canon_ptr, Attribute::Stack) ||
      astate->getPostAttrs().has(canon_ptr, Attribute::Global)) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call, "Invalid free: non-heap pointer");
    if (LatentIssue::isManifest(OperationResult::InvalidAccess, *astate,
                                canon_ptr)) {
      std::string msg = "Invalid free of ";
      msg += astate->getPostAttrs().has(canon_ptr, Attribute::Stack) ? "stack"
                                                                     : "global";
      msg += " memory";
      auto diag = std::make_unique<InvalidFree>(
          call, canon_ptr, msg,
          "Only free heap allocations (malloc/new). Do not free stack/global "
          "addresses.",
          std::move(trace));
      DiagnosticManager::getInstance().report(std::move(diag));
      astate->setPathFormula(
          std::make_unique<PulseFormula>(PulseFormula::contradiction()));
      return ModelResult::success(std::move(state));
    }
    return ModelResult::success({ExecutionDomain::latentAbortProgram(
        std::make_unique<AbductiveDomain>(astate->clone()), nullptr,
        OperationResult::InvalidAccess, std::move(trace))});
  }

  // Detect freeing an interior pointer (offset != 0 from a heap allocation).
  AbstractValue alloc_root = astate->getAllocationRoot(canon_ptr);
  if (!(alloc_root == canon_ptr) &&
      astate->getAllocationOffset(canon_ptr).value_or(1) != 0 &&
      (astate->getPostAttrs().has(alloc_root, Attribute::Allocated) ||
       astate->getAllocationInfo(alloc_root).has_value())) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call,
                   "Invalid free: interior pointer not at start of allocation");
    if (LatentIssue::isManifest(OperationResult::InvalidAccess, *astate,
                                canon_ptr)) {
      auto diag = std::make_unique<InvalidFree>(
          call, canon_ptr, "Invalid free of interior pointer",
          "Pass the original pointer returned by malloc/new to free/delete.",
          std::move(trace));
      DiagnosticManager::getInstance().report(std::move(diag));
      astate->setPathFormula(
          std::make_unique<PulseFormula>(PulseFormula::contradiction()));
      return ModelResult::success(std::move(state));
    }
    return ModelResult::success({ExecutionDomain::latentAbortProgram(
        std::make_unique<AbductiveDomain>(astate->clone()), nullptr,
        OperationResult::InvalidAccess, std::move(trace))});
  }

  if (astate->getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call, "Double free: freeing already freed pointer");
    if (auto inv_info = astate->getInvalidationInfo(canon_ptr)) {
      trace.addEvent(inv_info->second, "First free");
    }
    if (LatentIssue::isManifest(OperationResult::UseAfterFree, *astate,
                                canon_ptr)) {
      auto diag = std::make_unique<AccessToInvalidAddress>(
          call, "Double free detected", "Freeing already freed memory",
          "Ensure each allocation is freed exactly once",
          IssueType::UseAfterFree, std::move(trace), inv_kind);
      DiagnosticManager::getInstance().report(std::move(diag));
      return ModelResult::success({ExecutionDomain::abortProgram(
          std::make_unique<AbductiveDomain>(astate->clone()),
          OperationResult::UseAfterFree, Trace())});
    }
    return ModelResult::success({ExecutionDomain::latentAbortProgram(
        std::make_unique<AbductiveDomain>(astate->clone()), nullptr,
        OperationResult::UseAfterFree, std::move(trace))});
  }

  ops_.invalidate(*astate, *ptr_opt, call, inv_kind);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelRealloc(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto size_opt = getConstUInt64(call->getArgOperand(1));

  // realloc(ptr, 0) frees ptr and returns NULL.
  if (size_opt && *size_opt == 0) {
    if (ptr_opt) {
      AbstractValue c = astate->getCanonical(ptr_opt->addr);
      if (!astate->getPathFormula().isNull(c) &&
          !astate->getPostAttrs().has(c, Attribute::Null)) {
        ops_.invalidate(*astate, *ptr_opt, call, InvalidationKind::Realloc);
      }
    }
    AbstractValue null_val = factory_.createFresh(call);
    astate->getPostAttrs().add(null_val, Attribute::Null);
    (void)astate->getPathFormula().addNull(null_val);
    astate->getPostStack().add(call, Address(null_val));
    return ModelResult::success(std::move(state));
  }

  ExecutionDomain failure_state;
  const bool fork_null = options::modelMallocFailure();
  if (fork_null) {
    failure_state = state.clone();
  }

  if (ptr_opt) {
    // On the success branch, realloc consumes/invalidates the old non-null
    // buffer (when null-forking is enabled, failure_state was cloned above so
    // it retains the original valid buffer).
    AbstractValue c = astate->getCanonical(ptr_opt->addr);
    if (!astate->getPathFormula().isNull(c) &&
        !astate->getPostAttrs().has(c, Attribute::Null)) {
      ops_.invalidate(*astate, *ptr_opt, call, InvalidationKind::Realloc);
    }
  }

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ops_.allocate(*astate, ret_val, call, AllocatorKind::CRealloc);
  astate->addNonNull(ret_val);
  if (size_opt) {
    astate->setAllocationSize(ret_val, *size_opt);
  }
  ret_addr.history.addAllocationEvent(call, &ret_val);
  astate->getPostStack().add(call, ret_addr);

  std::vector<ExecutionDomain> out;
  out.push_back(std::move(state));

  if (fork_null && failure_state.getAstate()) {
    auto *fail_astate = failure_state.getAstate();
    AbstractValue null_val = factory_.createFresh(call);
    Address null_addr(null_val);
    null_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, call,
                               call->getFunction(), "realloc returned null");
    fail_astate->getPostAttrs().add(null_val, Attribute::Null);
    (void)fail_astate->getPathFormula().addNull(null_val);
    fail_astate->getPostStack().add(call, null_addr);
    out.push_back(std::move(failure_state));
  }

  return ModelResult::success(std::move(out));
}

ModelResult PulseModels::modelFileOpen(const llvm::CallBase *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred,
                                       AllocatorKind kind) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ops_.allocate(*astate, ret_val, call, kind);
  astate->getPostAttrs().add(ret_val, Attribute::FileHandle);
  astate->addNonNull(ret_val);
  ret_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction(), "Resource acquired");
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelFileClose(const llvm::CallBase *call,
                                        ExecutionDomain &state,
                                        const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!ptr_opt)
    return ModelResult::success(std::move(state));

  AbstractValue canon = astate->getCanonical(ptr_opt->addr);
  if (astate->getPostAttrs().has(canon, Attribute::Invalid)) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, OperationResult::UseAfterFree, *ptr_opt, call,
        "Closing already closed resource")});
  }

  ops_.invalidate(*astate, *ptr_opt, call, InvalidationKind::FClose);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelLock(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  (void)call;
  (void)pred;
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelUnlock(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  (void)call;
  (void)pred;
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStringCopy(const llvm::CallBase *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (dest_opt && src_opt) {
    auto read_res = ops_.readDeref(*astate, *src_opt, call);
    if (read_res.first != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, read_res.first, *src_opt, call,
          "Invalid source pointer in string copy")});
    }

    AbstractValue content =
        read_res.second ? read_res.second->addr : factory_.createFresh(call);
    auto write_res = ops_.writeDeref(*astate, *dest_opt, Address(content), call);
    if (write_res != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, write_res, *dest_opt, call,
          "Invalid destination pointer in string copy")});
    }

    AbstractValue c_src = astate->getCanonical(src_opt->addr);
    AbstractValue c_dest = astate->getCanonical(dest_opt->addr);
    markBufferInitialized(*astate, c_dest, call);
    TaintOperations::propagate(*astate, c_src, c_dest, call);
    if (astate->isInPre(c_src)) {
      astate->addPropagateTaintFrom(c_dest, {{c_src}, "strcpy"});
    }
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success(std::move(state));
}

//===----------------------------------------------------------------------===//
// C++ Container & Smart Pointer Models
//===----------------------------------------------------------------------===//

ModelResult PulseModels::modelStdVectorPushBack(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access data_field(0);
  if (Address *old_buf = astate->getPostHeap().findEdge(this_av, data_field)) {
    ops_.invalidate(*astate, *old_buf, call,
                    InvalidationKind::StdVectorPushBack);
  }

  AbstractValue new_buf = factory_.createFresh(call);
  ops_.allocate(*astate, new_buf, call, AllocatorKind::Stack);
  astate->getPostHeap().addEdge(this_av, data_field, Address(new_buf));
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdVectorClear(const llvm::CallBase *call,
                                             ExecutionDomain &state,
                                             const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access data_field(0);
  if (Address *old_buf = astate->getPostHeap().findEdge(this_av, data_field)) {
    ops_.invalidate(*astate, *old_buf, call, InvalidationKind::StdVectorClear);
  }
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdVectorData(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access data_field(0);
  Address ret_addr;
  if (Address *buf = astate->getPostHeap().findEdge(this_av, data_field)) {
    ret_addr = *buf;
  } else {
    AbstractValue fresh_buf = factory_.createFresh(call);
    ret_addr = Address(fresh_buf);
    ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, call,
                              call->getFunction(), "vector::data");
    astate->getPostHeap().addEdge(this_av, data_field, ret_addr);
    astate->abduceToPre(this_av, data_field, ret_addr);
  }
  astate->getPostStack().add(call, ret_addr);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdVectorAccess(const llvm::CallBase *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  return modelStdVectorData(call, state, pred);
}

ModelResult PulseModels::modelStdStringCStr(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  return modelStdVectorData(call, state, pred);
}

ModelResult PulseModels::modelStdStringMutate(const llvm::CallBase *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  return modelStdVectorPushBack(call, state, pred);
}

ModelResult PulseModels::modelStdUniquePtrCtor(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  if (call->arg_size() >= 2) {
    if (auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      writeSmartPtrSlot(*astate, this_av, *val_opt, factory_, call);
      astate->markWrittenTo(this_av, call);
      return ModelResult::success(std::move(state));
    }
  }
  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  writeSmartPtrSlot(*astate, this_av, Address(null_val), factory_, call);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdUniquePtrMove(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto dst_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (!dst_opt || !src_opt)
    return ModelResult::success(std::move(state));

  AbstractValue dst_av = astate->getCanonical(dst_opt->addr);
  AbstractValue src_av = astate->getCanonical(src_opt->addr);

  // Transfer ownership from src to dst, leaving src holding null.
  Address moved_ptr = readOrAbduceSmartPtrSlot(*astate, src_av, factory_, call);

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);

  writeSmartPtrSlot(*astate, dst_av, moved_ptr, factory_, call);
  writeSmartPtrSlot(*astate, src_av, Address(null_val), factory_, call);
  astate->markWrittenTo(dst_av, call);
  astate->markWrittenTo(src_av, call);
  if (call->getType()->isPointerTy()) {
    astate->getPostStack().add(call, *dst_opt);
  }
  return ModelResult::success(std::move(state));
}

ModelResult
PulseModels::modelStdUniquePtrRelease(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Address *raw_ptr = readSmartPtrSlot(*astate, this_av);
  Address ret_addr = raw_ptr ? *raw_ptr : Address(factory_.createFresh(call));

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  writeSmartPtrSlot(*astate, this_av, Address(null_val), factory_, call);
  astate->markWrittenTo(this_av, call);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdUniquePtrReset(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  if (Address *old_ptr = readSmartPtrSlot(*astate, this_av)) {
    AbstractValue old_c = astate->getCanonical(old_ptr->addr);
    if (!astate->getPathFormula().isNull(old_c) &&
        !astate->getPostAttrs().has(old_c, Attribute::Null)) {
      ops_.invalidate(*astate, *old_ptr, call, InvalidationKind::CppDelete);
    }
  }

  Address new_addr;
  if (call->arg_size() > 1) {
    auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
    new_addr = val_opt ? *val_opt : Address(factory_.createFresh(call));
  } else {
    AbstractValue null_val = factory_.createFresh(call);
    astate->getPostAttrs().add(null_val, Attribute::Null);
    (void)astate->getPathFormula().addNull(null_val);
    new_addr = Address(null_val);
  }

  writeSmartPtrSlot(*astate, this_av, new_addr, factory_, call);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdUniquePtrGet(const llvm::CallBase *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Address ret_addr = readOrAbduceSmartPtrSlot(*astate, this_av, factory_, call);
  astate->getPostStack().add(call, ret_addr);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdSmartPtrBool(const llvm::CallBase *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Address raw_addr = readOrAbduceSmartPtrSlot(*astate, this_av, factory_, call);
  AbstractValue raw_c = astate->getCanonical(raw_addr.addr);

  const bool definitely_null =
      astate->getPathFormula().isNull(raw_c) ||
      (astate->getPostAttrs().has(raw_c, Attribute::Null) &&
       !astate->getPathFormula().isNonNull(raw_c));
  const bool definitely_non_null =
      astate->getPathFormula().isNonNull(raw_c) ||
      (astate->getPostAttrs().has(raw_c, Attribute::Allocated) &&
       !astate->getPostAttrs().has(raw_c, Attribute::Null));

  if (definitely_null || definitely_non_null) {
    AbstractValue ret = factory_.createFresh(call);
    astate->getPathFormula().addIntegerConstraint(ret);
    int64_t bval = definitely_non_null ? 1 : 0;
    (void)astate->getPathFormula().addBounds(ret, bval, bval);
    astate->getPostStack().add(call, Address(ret));
    return ModelResult::success(std::move(state));
  }

  std::vector<ExecutionDomain> out;
  out.reserve(2);
  // True branch: raw_ptr != nullptr -> returns 1
  {
    ExecutionDomain t_state = state.clone();
    if (auto *ta = t_state.getAstate()) {
      if (ta->getPathFormula().addNonNull(raw_c) &&
          ta->incorporatePendingNewEqs().isOk() &&
          !ta->getPathFormula().isUnsat()) {
        AbstractValue ret = factory_.createFresh(call);
        ta->getPathFormula().addIntegerConstraint(ret);
        (void)ta->getPathFormula().addBounds(ret, 1, 1);
        ta->getPostStack().add(call, Address(ret));
        out.push_back(std::move(t_state));
      }
    }
  }
  // False branch: raw_ptr == nullptr -> returns 0
  {
    ExecutionDomain f_state = std::move(state);
    if (auto *fa = f_state.getAstate()) {
      if (fa->getPathFormula().addNull(raw_c) &&
          fa->incorporatePendingNewEqs().isOk() &&
          !fa->getPathFormula().isUnsat()) {
        fa->getPostAttrs().add(raw_c, Attribute::Null);
        AbstractValue ret = factory_.createFresh(call);
        fa->getPathFormula().addIntegerConstraint(ret);
        (void)fa->getPathFormula().addBounds(ret, 0, 0);
        fa->getPostStack().add(call, Address(ret));
        out.push_back(std::move(f_state));
      }
    }
  }
  return ModelResult::success(std::move(out));
}

ModelResult PulseModels::modelStdUniquePtrDtor(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  if (Address *old_ptr = readSmartPtrSlot(*astate, this_av)) {
    AbstractValue old_c = astate->getCanonical(old_ptr->addr);
    if (!astate->getPathFormula().isNull(old_c) &&
        !astate->getPostAttrs().has(old_c, Attribute::Null)) {
      ops_.invalidate(*astate, *old_ptr, call, InvalidationKind::CppDelete);
    }
  }
  return ModelResult::success(std::move(state));
}

//===----------------------------------------------------------------------===//
// std::shared_ptr Models (field 0 -> Deref = raw ptr, kSharedCbField = cb -> rc)
//===----------------------------------------------------------------------===//

ModelResult PulseModels::modelStdSharedPtrCtor(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access cb_field(kSharedCbField);
  Access deref(AccessKind::Dereference);

  if (call->arg_size() >= 2) {
    if (auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      AbstractValue val_c = astate->getCanonical(val_opt->addr);
      if (!astate->getPathFormula().isNull(val_c) &&
          !astate->getPostAttrs().has(val_c, Attribute::Null)) {
        astate->addSharedPtrTarget(val_c);
      }
      writeSmartPtrSlot(*astate, this_av, *val_opt, factory_, call);
      AbstractValue cb = factory_.createFresh(call);
      AbstractValue one = factory_.createFresh(call);
      (void)astate->getPathFormula().addBounds(one, 1, 1);
      astate->getPostHeap().addEdge(cb, deref, Address(one));
      astate->getPostHeap().addEdge(this_av, cb_field, Address(cb));
      astate->markWrittenTo(this_av, call);
      return ModelResult::success(std::move(state));
    }
  }

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  writeSmartPtrSlot(*astate, this_av, Address(null_val), factory_, call);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult
PulseModels::modelStdSharedPtrCopyCtor(const llvm::CallBase *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto dst_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (!dst_opt || !src_opt)
    return ModelResult::success(std::move(state));

  AbstractValue dst_av = astate->getCanonical(dst_opt->addr);
  AbstractValue src_av = astate->getCanonical(src_opt->addr);
  Access cb_field(kSharedCbField);
  Access deref(AccessKind::Dereference);

  if (Address *raw = readSmartPtrSlot(*astate, src_av)) {
    AbstractValue raw_c = astate->getCanonical(raw->addr);
    if (!astate->getPathFormula().isNull(raw_c) &&
        !astate->getPostAttrs().has(raw_c, Attribute::Null)) {
      astate->addSharedPtrTarget(raw_c);
    }
    writeSmartPtrSlot(*astate, dst_av, *raw, factory_, call);
  }
  if (Address *cb = astate->getPostHeap().findEdge(src_av, cb_field)) {
    AbstractValue cb_c = astate->getCanonical(cb->addr);
    astate->getPostHeap().addEdge(dst_av, cb_field, *cb);
    if (Address *rc_edge = astate->getPostHeap().findEdge(cb_c, deref)) {
      if (auto cur_rc = astate->getPathFormula().asConstantInt(
              astate->getCanonical(rc_edge->addr))) {
        AbstractValue next_rc = factory_.createFresh(call);
        (void)astate->getPathFormula().addBounds(next_rc, *cur_rc + 1,
                                                 *cur_rc + 1);
        astate->getPostHeap().addEdge(cb_c, deref, Address(next_rc));
      }
    }
  }
  astate->markWrittenTo(dst_av, call);
  if (call->getType()->isPointerTy()) {
    astate->getPostStack().add(call, *dst_opt);
  }
  return ModelResult::success(std::move(state));
}

ModelResult
PulseModels::modelStdSharedPtrCopyAssign(const llvm::CallBase *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto dst_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (!dst_opt || !src_opt)
    return ModelResult::success(std::move(state));
  AbstractValue dst_av = astate->getCanonical(dst_opt->addr);
  AbstractValue src_av = astate->getCanonical(src_opt->addr);
  if (dst_av == src_av) {
    if (call->getType()->isPointerTy()) {
      astate->getPostStack().add(call, *dst_opt);
    }
    return ModelResult::success(std::move(state));
  }
  // Release previous target owned by dst if any.
  Access cb_field(kSharedCbField);
  if (astate->getPostHeap().findEdge(dst_av, cb_field) != nullptr ||
      readSmartPtrSlot(*astate, dst_av) != nullptr) {
    auto dtor_res = modelStdSharedPtrDtor(call, state, pred);
    if (!dtor_res.states.empty()) {
      state = std::move(dtor_res.states[0]);
    }
  }
  return modelStdSharedPtrCopyCtor(call, state, pred);
}

ModelResult PulseModels::modelStdSharedPtrGet(const llvm::CallBase *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  return modelStdUniquePtrGet(call, state, pred);
}

ModelResult PulseModels::modelStdSharedPtrDtor(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access cb_field(kSharedCbField);
  Access deref(AccessKind::Dereference);

  bool should_free = true;
  if (Address *cb = astate->getPostHeap().findEdge(this_av, cb_field)) {
    AbstractValue cb_c = astate->getCanonical(cb->addr);
    if (Address *rc_edge = astate->getPostHeap().findEdge(cb_c, deref)) {
      if (auto cur_rc = astate->getPathFormula().asConstantInt(
              astate->getCanonical(rc_edge->addr))) {
        if (*cur_rc > 1) {
          should_free = false;
          AbstractValue dec_rc = factory_.createFresh(call);
          (void)astate->getPathFormula().addBounds(dec_rc, *cur_rc - 1,
                                                   *cur_rc - 1);
          astate->getPostHeap().addEdge(cb_c, deref, Address(dec_rc));
        }
      }
    }
  }

  if (should_free) {
    if (Address *old_ptr = readSmartPtrSlot(*astate, this_av)) {
      AbstractValue old_c = astate->getCanonical(old_ptr->addr);
      if (!astate->getPathFormula().isNull(old_c) &&
          !astate->getPostAttrs().has(old_c, Attribute::Null)) {
        ops_.invalidate(*astate, *old_ptr, call, InvalidationKind::CppDelete);
      }
    }
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdSharedPtrReset(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  auto dtor_res = modelStdSharedPtrDtor(call, state, pred);
  if (!dtor_res.states.empty()) {
    state = std::move(dtor_res.states[0]);
  }
  auto *astate = state.getAstate();
  if (!astate || call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access cb_field(kSharedCbField);
  Access deref(AccessKind::Dereference);

  if (call->arg_size() > 1) {
    if (auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      AbstractValue val_c = astate->getCanonical(val_opt->addr);
      if (!astate->getPathFormula().isNull(val_c) &&
          !astate->getPostAttrs().has(val_c, Attribute::Null)) {
        astate->addSharedPtrTarget(val_c);
      }
      writeSmartPtrSlot(*astate, this_av, *val_opt, factory_, call);
      AbstractValue cb = factory_.createFresh(call);
      AbstractValue one = factory_.createFresh(call);
      (void)astate->getPathFormula().addBounds(one, 1, 1);
      astate->getPostHeap().addEdge(cb, deref, Address(one));
      astate->getPostHeap().addEdge(this_av, cb_field, Address(cb));
      astate->markWrittenTo(this_av, call);
      return ModelResult::success(std::move(state));
    }
  }

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  writeSmartPtrSlot(*astate, this_av, Address(null_val), factory_, call);
  astate->getPostHeap().removeEdge(this_av, cb_field);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdOptionalReset(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access engaged_field(1);
  AbstractValue zero_val = factory_.createFresh(call);
  (void)astate->getPathFormula().addBounds(zero_val, 0, 0);
  (void)astate->getPathFormula().addNull(zero_val);
  astate->getPostHeap().addEdge(this_av, engaged_field, Address(zero_val));
  astate->getPostAttrs().add(this_av, Attribute::Null);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdOptionalValue(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access engaged_field(1);
  bool known_empty = astate->getPostAttrs().has(this_av, Attribute::Null);
  if (Address *eng = astate->getPostHeap().findEdge(this_av, engaged_field)) {
    if (astate->getPathFormula().isNull(astate->getCanonical(eng->addr))) {
      known_empty = true;
    }
  }
  if (known_empty) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, OperationResult::NullDereference, *this_opt, call,
        "Dereference of empty std::optional")});
  }

  Access val_field(0);
  Address ret_addr;
  if (Address *v = astate->getPostHeap().findEdge(this_av, val_field)) {
    ret_addr = *v;
  } else {
    ret_addr = Address(factory_.createFresh(call));
    astate->getPostHeap().addEdge(this_av, val_field, ret_addr);
    astate->abduceToPre(this_av, val_field, ret_addr);
  }
  astate->getPostStack().add(call, ret_addr);
  return ModelResult::success(std::move(state));
}

ModelResult
PulseModels::modelStdOptionalHasValue(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access engaged_field(1);
  if (Address *eng = astate->getPostHeap().findEdge(this_av, engaged_field)) {
    astate->getPostStack().add(call, *eng);
  } else {
    AbstractValue fresh = factory_.createFresh(call);
    astate->getPathFormula().addIntegerConstraint(fresh);
    astate->getPostHeap().addEdge(this_av, engaged_field, Address(fresh));
    astate->getPostStack().add(call, Address(fresh));
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelSocket(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  return modelFileOpen(call, state, pred, AllocatorKind::Socket);
}

ModelResult PulseModels::modelTaintSource(const llvm::CallBase *call,
                                          ExecutionDomain &state,
                                          const llvm::BasicBlock *pred,
                                          const std::string &kind) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate || !call->getType()->isPointerTy()) {
    return ModelResult::success(std::move(state));
  }

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, call,
                            call->getFunction());
  astate->getPostStack().add(call, ret_addr);

  TaintKind source_kind = TaintKind::UserInput();
  if (kind == "network") {
    source_kind = TaintKind::Network();
  } else if (kind == "environment") {
    source_kind = TaintKind::Environment();
  } else if (kind == "filesystem") {
    source_kind = TaintKind::FileSystem();
  } else if (kind == "sensitive") {
    source_kind = TaintKind::Sensitive();
  }
  TaintOperations::taint(*astate, ret_val, source_kind,
                         call->getCalledFunction()
                             ? call->getCalledFunction()->getName().str()
                             : kind);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelTaintSink(const llvm::CallBase *call,
                                        ExecutionDomain &state,
                                        const llvm::BasicBlock *pred,
                                        const std::string &kind) {
  auto *astate = state.getAstate();
  if (!astate) {
    return ModelResult::success(std::move(state));
  }
  const std::string sink_name =
      call->getCalledFunction() ? call->getCalledFunction()->getName().str()
                                : kind;
  for (unsigned i = 0; i < call->arg_size(); ++i) {
    auto arg_opt = ops_.eval(*astate, call->getArgOperand(i), call, pred);
    if (!arg_opt) {
      continue;
    }
    TaintOperations::checkSink(*astate, astate->getCanonical(arg_opt->addr),
                               sink_name, call);
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelCalloc(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);

  ops_.allocate(*astate, ret_val, call, AllocatorKind::CMalloc);
  ops_.initialize(*astate, ret_val);
  astate->addNonNull(ret_val);
  if (call->arg_size() >= 2) {
    auto n_opt = getConstUInt64(call->getArgOperand(0));
    auto sz_opt = getConstUInt64(call->getArgOperand(1));
    if (n_opt && sz_opt) {
      if (auto total_opt = mulConstU64(*n_opt, *sz_opt)) {
        astate->setAllocationSize(ret_val, *total_opt);
      }
    }
  }

  ret_addr.history.addAllocationEvent(call, &ret_val);
  astate->getPostStack().add(call, ret_addr);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStrdup(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto src_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!src_opt)
    return ModelResult::success(std::move(state));

  auto read_res = ops_.readDeref(*astate, *src_opt, call);
  if (read_res.first != OperationResult::Success) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, read_res.first, *src_opt, call,
        "Invalid pointer in strdup")});
  }

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ops_.allocate(*astate, ret_val, call, AllocatorKind::CMalloc);
  astate->addNonNull(ret_val);
  ret_addr.history.addAllocationEvent(call, &ret_val);
  astate->getPostStack().add(call, ret_addr);

  AbstractValue c_src = astate->getCanonical(src_opt->addr);
  TaintOperations::propagate(*astate, c_src, ret_val, call);
  if (astate->isInPre(c_src)) {
    astate->addPropagateTaintFrom(ret_val, {{c_src}, "strdup"});
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStrchr(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!str_opt)
    return ModelResult::success(std::move(state));

  auto read_res = ops_.readDeref(*astate, *str_opt, call);
  if (read_res.first != OperationResult::Success) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, read_res.first, *str_opt, call,
        "Invalid pointer in strchr")});
  }

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  astate->getPostStack().add(call, ret_addr);
  TaintOperations::propagate(*astate, astate->getCanonical(str_opt->addr),
                             ret_val, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStrstr(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto haystack_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto needle_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (haystack_opt) {
    auto read_res = ops_.readDeref(*astate, *haystack_opt, call);
    if (read_res.first != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, read_res.first, *haystack_opt, call,
          "Invalid haystack pointer in strstr")});
    }
  }
  if (needle_opt) {
    auto read_res = ops_.readDeref(*astate, *needle_opt, call);
    if (read_res.first != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, read_res.first, *needle_opt, call,
          "Invalid needle pointer in strstr")});
    }
  }

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  astate->getPostStack().add(call, ret_addr);
  if (haystack_opt) {
    TaintOperations::propagate(*astate, astate->getCanonical(haystack_opt->addr),
                               ret_val, call);
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStrcmp(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred,
                                     bool bounded, bool case_insensitive,
                                     bool is_memcmp) {
  const unsigned min_args = bounded ? 3u : 2u;
  if (call->arg_size() < min_args)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  std::optional<uint64_t> n_opt;
  if (bounded) {
    n_opt = getConstUInt64(call->getArgOperand(2));
    if (n_opt && *n_opt == 0) {
      AbstractValue zero = factory_.createFresh(call);
      astate->getPathFormula().addIntegerConstraint(zero);
      (void)astate->getPathFormula().addBounds(zero, 0, 0);
      astate->getPostStack().add(call, Address(zero));
      return ModelResult::success(std::move(state));
    }
  }

  auto s0_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto s1_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (s0_opt) {
    auto r0 = ops_.readDeref(*astate, *s0_opt, call);
    if (r0.first != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, r0.first, *s0_opt, call,
          "Invalid first pointer in string/memory compare")});
    }
  }
  if (s1_opt) {
    auto r1 = ops_.readDeref(*astate, *s1_opt, call);
    if (r1.first != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, r1.first, *s1_opt, call,
          "Invalid second pointer in string/memory compare")});
    }
  }

  AbstractValue ret_val = factory_.createFresh(call);
  astate->getPathFormula().addIntegerConstraint(ret_val);

  if (s0_opt && s1_opt) {
    AbstractValue c0 = astate->getCanonical(s0_opt->addr);
    AbstractValue c1 = astate->getCanonical(s1_opt->addr);
    if (c0 == c1 || astate->getPathFormula().areEqual(c0, c1)) {
      (void)astate->getPathFormula().addBounds(ret_val, 0, 0);
    } else if (!is_memcmp) {
      auto str0 = astate->getPathFormula().asConstantString(c0);
      auto str1 = astate->getPathFormula().asConstantString(c1);
      if (str0 && str1 && (!bounded || n_opt.has_value())) {
        size_t limit = bounded ? static_cast<size_t>(*n_opt)
                               : std::max(str0->size(), str1->size()) + 1;
        int cmp_res = 0;
        for (size_t i = 0; i < limit; ++i) {
          unsigned char ch0 =
              i < str0->size() ? static_cast<unsigned char>((*str0)[i]) : 0;
          unsigned char ch1 =
              i < str1->size() ? static_cast<unsigned char>((*str1)[i]) : 0;
          if (case_insensitive) {
            ch0 = static_cast<unsigned char>(std::tolower(ch0));
            ch1 = static_cast<unsigned char>(std::tolower(ch1));
          }
          if (ch0 != ch1) {
            cmp_res = static_cast<int>(ch0) - static_cast<int>(ch1);
            break;
          }
          if (ch0 == 0)
            break;
        }
        (void)astate->getPathFormula().addBounds(ret_val, cmp_res, cmp_res);
      }
    }
  }

  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelMemcpy(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (dest_opt && src_opt) {
    checkLengthAgainstAllocation(*astate, *dest_opt, call->getArgOperand(2),
                                 call,
                                 "memcpy writes beyond destination buffer");
    checkLengthAgainstAllocation(*astate, *src_opt, call->getArgOperand(2),
                                 call, "memcpy reads beyond source buffer");
    auto src_read = ops_.readDeref(*astate, *src_opt, call);
    if (src_read.first != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, src_read.first, *src_opt, call,
          "Invalid source buffer in memcpy")});
    }
    AbstractValue content =
        src_read.second ? src_read.second->addr : factory_.createFresh(call);
    auto write_res = ops_.writeDeref(*astate, *dest_opt, Address(content), call);
    if (write_res != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, write_res, *dest_opt, call,
          "Invalid destination buffer in memcpy")});
    }
    AbstractValue c_src = astate->getCanonical(src_opt->addr);
    AbstractValue c_dest = astate->getCanonical(dest_opt->addr);
    markBufferInitialized(*astate, c_dest, call);
    ops_.copySubgraph(*astate, c_src, c_dest);
    TaintOperations::propagate(*astate, c_src, c_dest, call);
    if (astate->isInPre(c_src)) {
      astate->addPropagateTaintFrom(c_dest, {{c_src}, "memcpy"});
    }
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelMemmove(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  return modelMemcpy(call, state, pred);
}

ModelResult PulseModels::modelMemset(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto value_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (dest_opt && value_opt) {
    checkLengthAgainstAllocation(*astate, *dest_opt, call->getArgOperand(2),
                                 call,
                                 "memset writes beyond destination buffer");
    ops_.writeDeref(*astate, *dest_opt, *value_opt, call);
    markBufferInitialized(*astate, dest_opt->addr, call);
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelRead(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto buf_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (buf_opt) {
    checkLengthAgainstAllocation(*astate, *buf_opt, call->getArgOperand(2),
                                 call, "read writes beyond destination buffer");
    AbstractValue dummy = factory_.createFresh(call);
    ops_.writeDeref(*astate, *buf_opt, Address(dummy), call);
    markBufferInitialized(*astate, buf_opt->addr, call);

    AbstractValue ret_val = factory_.createFresh(call);
    astate->getPostStack().add(call, Address(ret_val));
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelWrite(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto buf_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (buf_opt) {
    checkLengthAgainstAllocation(*astate, *buf_opt, call->getArgOperand(2),
                                 call, "write reads beyond source buffer");
    auto read_res = ops_.readDeref(*astate, *buf_opt, call);
    if (read_res.first == OperationResult::Success) {
      AbstractValue ret_val = factory_.createFresh(call);
      astate->getPostStack().add(call, Address(ret_val));
    }
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelFread(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 4)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto stream_opt = ops_.eval(*astate, call->getArgOperand(3), call, pred);
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  if (stream_opt && ptr_opt) {
    auto size_opt = getConstUInt64(call->getArgOperand(1));
    auto count_opt = getConstUInt64(call->getArgOperand(2));
    if (size_opt && count_opt) {
      if (auto total_opt = mulConstU64(*size_opt, *count_opt)) {
        AbstractValue canon = astate->getCanonical(ptr_opt->addr);
        auto alloc_size_opt = astate->getAllocationSize(canon);
        if (alloc_size_opt && *total_opt > *alloc_size_opt) {
          reportOutOfBoundsAccess(*astate, call, *ptr_opt,
                                  "fread writes beyond destination buffer");
          astate->getPostAttrs().add(canon, Attribute::OutOfBounds);
        }
      }
    }
    auto stream_read = ops_.readDeref(*astate, *stream_opt, call);
    if (stream_read.first == OperationResult::Success) {
      AbstractValue dummy = factory_.createFresh(call);
      ops_.writeDeref(*astate, *ptr_opt, Address(dummy), call);
      markBufferInitialized(*astate, ptr_opt->addr, call);
      AbstractValue ret_val = factory_.createFresh(call);
      astate->getPostStack().add(call, Address(ret_val));
    }
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelFwrite(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 4)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto stream_opt = ops_.eval(*astate, call->getArgOperand(3), call, pred);
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  if (stream_opt && ptr_opt) {
    auto size_opt = getConstUInt64(call->getArgOperand(1));
    auto count_opt = getConstUInt64(call->getArgOperand(2));
    if (size_opt && count_opt) {
      if (auto total_opt = mulConstU64(*size_opt, *count_opt)) {
        AbstractValue canon = astate->getCanonical(ptr_opt->addr);
        auto alloc_size_opt = astate->getAllocationSize(canon);
        if (alloc_size_opt && *total_opt > *alloc_size_opt) {
          reportOutOfBoundsAccess(*astate, call, *ptr_opt,
                                  "fwrite reads beyond source buffer");
          astate->getPostAttrs().add(canon, Attribute::OutOfBounds);
        }
      }
    }
    auto stream_read = ops_.readDeref(*astate, *stream_opt, call);
    auto ptr_read = ops_.readDeref(*astate, *ptr_opt, call);
    if (stream_read.first == OperationResult::Success &&
        ptr_read.first == OperationResult::Success) {
      AbstractValue ret_val = factory_.createFresh(call);
      astate->getPostStack().add(call, Address(ret_val));
    }
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelFgets(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto stream_opt = ops_.eval(*astate, call->getArgOperand(2), call, pred);

  if (str_opt && stream_opt) {
    checkLengthAgainstAllocation(*astate, *str_opt, call->getArgOperand(1),
                                 call,
                                 "fgets writes beyond destination buffer");
    auto stream_read = ops_.readDeref(*astate, *stream_opt, call);
    if (stream_read.first == OperationResult::Success) {
      AbstractValue dummy = factory_.createFresh(call);
      ops_.writeDeref(*astate, *str_opt, Address(dummy), call);
      markBufferInitialized(*astate, str_opt->addr, call);
      astate->getPostStack().add(call, *str_opt);
    }
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelGets(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  if (str_opt) {
    AbstractValue dummy = factory_.createFresh(call);
    ops_.writeDeref(*astate, *str_opt, Address(dummy), call);
    markBufferInitialized(*astate, str_opt->addr, call);
    astate->getPostStack().add(call, *str_opt);
  }

  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStrlen(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!str_opt)
    return ModelResult::success(std::move(state));

  auto read_res = ops_.readDeref(*astate, *str_opt, call);
  if (read_res.first != OperationResult::Success) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, read_res.first, *str_opt, call,
        "Invalid pointer in strlen")});
  }

  AbstractValue ret_val = factory_.createFresh(call);
  astate->getPathFormula().addIntegerConstraint(ret_val);
  AbstractValue c_str = astate->getCanonical(str_opt->addr);
  (void)astate->getPathFormula().addStringLength(c_str, ret_val);
  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStrcat(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred,
                                     bool bounded) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (!dest_opt || !src_opt)
    return ModelResult::success(std::move(state));

  if (bounded && call->arg_size() >= 3) {
    checkLengthAgainstAllocation(*astate, *dest_opt, call->getArgOperand(2),
                                 call,
                                 "strncat writes beyond destination buffer");
  }

  auto src_read = ops_.readDeref(*astate, *src_opt, call);
  if (src_read.first != OperationResult::Success) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, src_read.first, *src_opt, call,
        "Invalid source pointer in strcat/strncat")});
  }
  AbstractValue content =
      src_read.second ? src_read.second->addr : factory_.createFresh(call);
  auto write_res = ops_.writeDeref(*astate, *dest_opt, Address(content), call);
  if (write_res != OperationResult::Success) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, write_res, *dest_opt, call,
        "Invalid destination pointer in strcat/strncat")});
  }

  AbstractValue c_src = astate->getCanonical(src_opt->addr);
  AbstractValue c_dest = astate->getCanonical(dest_opt->addr);
  if (auto d_str = astate->getPathFormula().asConstantString(c_dest)) {
    if (auto s_str = astate->getPathFormula().asConstantString(c_src)) {
      if (auto alloc_sz = astate->getAllocationSize(c_dest)) {
        if (d_str->size() + s_str->size() + 1 > *alloc_sz) {
          reportOutOfBoundsAccess(
              *astate, call, *dest_opt,
              "strcat writes beyond destination buffer capacity");
          astate->getPostAttrs().add(c_dest, Attribute::OutOfBounds);
        }
      }
      AbstractValue concat_av = factory_.createFresh(call);
      (void)astate->getPathFormula().addStringConcat(concat_av, c_dest, c_src);
    }
  }
  TaintOperations::propagate(*astate, c_src, c_dest, call);
  if (astate->isInPre(c_src)) {
    astate->addPropagateTaintFrom(c_dest,
                                  {{c_src}, bounded ? "strncat" : "strcat"});
  }
  astate->getPostStack().add(call, *dest_opt);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelSprintf(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred,
                                      bool bounded) {
  const unsigned fmt_idx = bounded ? 2u : 1u;
  if (call->arg_size() <= fmt_idx)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto fmt_opt = ops_.eval(*astate, call->getArgOperand(fmt_idx), call, pred);
  if (!dest_opt)
    return ModelResult::success(std::move(state));

  if (bounded) {
    checkLengthAgainstAllocation(*astate, *dest_opt, call->getArgOperand(1),
                                 call,
                                 "snprintf writes beyond destination buffer");
  }
  if (fmt_opt) {
    auto fmt_read = ops_.readDeref(*astate, *fmt_opt, call);
    if (fmt_read.first != OperationResult::Success) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, fmt_read.first, *fmt_opt, call,
          "Invalid format string in sprintf/snprintf")});
    }
  }

  AbstractValue dummy = factory_.createFresh(call);
  auto write_res = ops_.writeDeref(*astate, *dest_opt, Address(dummy), call);
  if (write_res != OperationResult::Success) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, write_res, *dest_opt, call,
        "Invalid destination buffer in sprintf/snprintf")});
  }

  AbstractValue c_dest = astate->getCanonical(dest_opt->addr);
  markBufferInitialized(*astate, c_dest, call);
  std::vector<AbstractValue> pre_srcs;
  for (unsigned i = fmt_idx; i < call->arg_size(); ++i) {
    if (auto arg_opt = ops_.eval(*astate, call->getArgOperand(i), call, pred)) {
      AbstractValue c_arg = astate->getCanonical(arg_opt->addr);
      TaintOperations::propagate(*astate, c_arg, c_dest, call);
      if (astate->isInPre(c_arg)) {
        pre_srcs.push_back(c_arg);
      }
    }
  }
  if (!pre_srcs.empty()) {
    astate->addPropagateTaintFrom(
        c_dest, {std::move(pre_srcs), bounded ? "snprintf" : "sprintf"});
  }

  AbstractValue ret_val = factory_.createFresh(call);
  astate->getPathFormula().addIntegerConstraint(ret_val);
  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelPipe(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  auto fds_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!fds_opt)
    return ModelResult::success(std::move(state));

  AbstractValue fd0 = factory_.createFresh(call);
  AbstractValue fd1 = factory_.createFresh(call);
  ops_.allocate(*astate, fd0, call, AllocatorKind::FileHandle);
  ops_.allocate(*astate, fd1, call, AllocatorKind::FileHandle);
  astate->getPostAttrs().add(fd0, Attribute::FileHandle);
  astate->getPostAttrs().add(fd1, Attribute::FileHandle);
  astate->addNonNull(fd0);
  astate->addNonNull(fd1);

  Address fd0_addr(fd0);
  fd0_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction(), "Pipe read fd acquired");
  Address fd1_addr(fd1);
  fd1_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction(), "Pipe write fd acquired");

  auto w0 = ops_.writeDeref(*astate, *fds_opt, fd0_addr, call);
  if (w0 != OperationResult::Success) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, w0, *fds_opt, call, "Invalid fd array in pipe()")});
  }

  // Also bind fds[1] at canonical constant index 1 (stride 4) so
  // `close(fds[0]); close(fds[1]);` cleanly releases both descriptors.
  AbstractValue slot1 = factory_.createFresh(call);
  astate->getPostHeap().addEdge(slot1, Access(AccessKind::Dereference),
                                fd1_addr);
  AbstractValue fds_c = astate->getCanonical(fds_opt->addr);
  markBufferInitialized(*astate, fds_c, call);
  astate->getPostHeap().addEdge(fds_c, Access::constArrayIndex(1, 4),
                                Address(slot1));

  // If pipe() was called with `gep [2 x i32], %fds, 0, 0`, also wire index 1
  // directly onto the array base node for `gep [2 x i32], %fds, 0, 1`.
  if (const auto *GEP = llvm::dyn_cast<llvm::GEPOperator>(
          call->getArgOperand(0)->stripPointerCasts())) {
    if (GEP->getNumIndices() >= 2) {
      if (auto arr_base =
              ops_.eval(*astate, GEP->getPointerOperand(), call, pred)) {
        AbstractValue arr_c = astate->getCanonical(arr_base->addr);
        markBufferInitialized(*astate, arr_c, call);
        astate->getPostHeap().addEdge(arr_c, Access::constArrayIndex(1, 4),
                                      Address(slot1));
      }
    }
  }

  AbstractValue zero = factory_.createFresh(call);
  astate->getPathFormula().addIntegerConstraint(zero);
  (void)astate->getPathFormula().addBounds(zero, 0, 0);
  astate->getPostStack().add(call, Address(zero));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelDup2(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  if (auto old_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred)) {
    AbstractValue old_c = astate->getCanonical(old_opt->addr);
    if (astate->getPostAttrs().has(old_c, Attribute::Invalid)) {
      return ModelResult::success({reportModelAccessError(
          checker_, *astate, OperationResult::UseAfterFree, *old_opt, call,
          "dup2 on already closed file descriptor")});
    }
  }
  if (auto new_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
    astate->getPostStack().add(call, *new_opt);
  } else {
    AbstractValue ret = factory_.createFresh(call);
    astate->getPathFormula().addIntegerConstraint(ret);
    astate->getPostStack().add(call, Address(ret));
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdMakeUnique(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  AbstractValue obj = factory_.createFresh(call);
  Address obj_addr(obj);
  ops_.allocate(*astate, obj, call, AllocatorKind::CppNew);
  astate->addNonNull(obj);
  obj_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction(), "std::make_unique");

  if (call->getType()->isVoidTy() && call->arg_size() >= 1) {
    if (auto sret_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred)) {
      AbstractValue sret_c = astate->getCanonical(sret_opt->addr);
      writeSmartPtrSlot(*astate, sret_c, obj_addr, factory_, call);
      astate->markWrittenTo(sret_c, call);
    }
  } else {
    AbstractValue wrapper = factory_.createFresh(call);
    writeSmartPtrSlot(*astate, wrapper, obj_addr, factory_, call);
    astate->getPostStack().add(call, Address(wrapper));
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdMakeShared(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));

  AbstractValue obj = factory_.createFresh(call);
  Address obj_addr(obj);
  ops_.allocate(*astate, obj, call, AllocatorKind::CppNew);
  astate->addNonNull(obj);
  astate->addSharedPtrTarget(obj);
  obj_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction(), "std::make_shared");

  Access cb_field(kSharedCbField);
  Access deref(AccessKind::Dereference);
  AbstractValue cb = factory_.createFresh(call);
  AbstractValue one = factory_.createFresh(call);
  (void)astate->getPathFormula().addBounds(one, 1, 1);
  astate->getPostHeap().addEdge(cb, deref, Address(one));

  if (call->getType()->isVoidTy() && call->arg_size() >= 1) {
    if (auto sret_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred)) {
      AbstractValue sret_c = astate->getCanonical(sret_opt->addr);
      writeSmartPtrSlot(*astate, sret_c, obj_addr, factory_, call);
      astate->getPostHeap().addEdge(sret_c, cb_field, Address(cb));
      astate->markWrittenTo(sret_c, call);
    }
  } else {
    AbstractValue wrapper = factory_.createFresh(call);
    writeSmartPtrSlot(*astate, wrapper, obj_addr, factory_, call);
    astate->getPostHeap().addEdge(wrapper, cb_field, Address(cb));
    astate->getPostStack().add(call, Address(wrapper));
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdWeakPtrCtor(const llvm::CallBase *call,
                                             ExecutionDomain &state,
                                             const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access weak_ptr_field(kWeakPtrField);
  Access weak_cb_field(kWeakCbField);

  if (call->arg_size() >= 2) {
    if (auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      AbstractValue src_av = astate->getCanonical(src_opt->addr);
      // Copy pointer & control block WITHOUT incrementing strong refcount and
      // WITHOUT adding a strong ownership edge (uses kWeakPtrField).
      Address *raw = readSmartPtrSlot(*astate, src_av);
      if (!raw) {
        raw = astate->getPostHeap().findEdge(src_av, weak_ptr_field);
      }
      if (raw) {
        astate->getPostHeap().addEdge(this_av, weak_ptr_field, *raw);
      }
      Address *cb =
          astate->getPostHeap().findEdge(src_av, Access(kSharedCbField));
      if (!cb) {
        cb = astate->getPostHeap().findEdge(src_av, weak_cb_field);
      }
      if (cb) {
        astate->getPostHeap().addEdge(this_av, weak_cb_field, *cb);
      }
      astate->markWrittenTo(this_av, call);
      if (call->getType()->isPointerTy()) {
        astate->getPostStack().add(call, *this_opt);
      }
      return ModelResult::success(std::move(state));
    }
  }

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  astate->getPostHeap().addEdge(this_av, weak_ptr_field, Address(null_val));
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdWeakPtrExpired(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  AbstractValue ret_val = factory_.createFresh(call);
  astate->getPathFormula().addIntegerConstraint(ret_val);

  if (Address *raw =
          astate->getPostHeap().findEdge(this_av, Access(kWeakPtrField))) {
    AbstractValue raw_c = astate->getCanonical(raw->addr);
    const bool expired =
        astate->getPostAttrs().has(raw_c, Attribute::Invalid) ||
        astate->getPostAttrs().has(raw_c, Attribute::Null) ||
        astate->getPathFormula().isNull(raw_c);
    (void)astate->getPathFormula().addBounds(ret_val, expired ? 1 : 0,
                                             expired ? 1 : 0);
  }
  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdWeakPtrLock(const llvm::CallBase *call,
                                             ExecutionDomain &state,
                                             const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate || call->arg_size() < 1)
    return ModelResult::success(std::move(state));

  const bool is_sret = call->getType()->isVoidTy() && call->arg_size() >= 2;
  auto this_opt =
      ops_.eval(*astate, call->getArgOperand(is_sret ? 1 : 0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));
  AbstractValue this_av = astate->getCanonical(this_opt->addr);

  AbstractValue out_sp = factory_.createFresh(call);
  if (is_sret) {
    if (auto sret_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred)) {
      out_sp = astate->getCanonical(sret_opt->addr);
    }
  }

  Access cb_field(kSharedCbField);
  Access deref(AccessKind::Dereference);
  Address *raw =
      astate->getPostHeap().findEdge(this_av, Access(kWeakPtrField));
  bool expired = true;
  if (raw) {
    AbstractValue raw_c = astate->getCanonical(raw->addr);
    expired = astate->getPostAttrs().has(raw_c, Attribute::Invalid) ||
              astate->getPostAttrs().has(raw_c, Attribute::Null) ||
              astate->getPathFormula().isNull(raw_c);
  }

  if (expired || !raw) {
    AbstractValue null_val = factory_.createFresh(call);
    astate->getPostAttrs().add(null_val, Attribute::Null);
    (void)astate->getPathFormula().addNull(null_val);
    writeSmartPtrSlot(*astate, out_sp, Address(null_val), factory_, call);
  } else {
    AbstractValue raw_c = astate->getCanonical(raw->addr);
    astate->addSharedPtrTarget(raw_c);
    writeSmartPtrSlot(*astate, out_sp, *raw, factory_, call);
    if (Address *cb =
            astate->getPostHeap().findEdge(this_av, Access(kWeakCbField))) {
      AbstractValue cb_c = astate->getCanonical(cb->addr);
      astate->getPostHeap().addEdge(out_sp, cb_field, *cb);
      if (Address *rc_edge = astate->getPostHeap().findEdge(cb_c, deref)) {
        if (auto cur_rc = astate->getPathFormula().asConstantInt(
                astate->getCanonical(rc_edge->addr))) {
          AbstractValue next_rc = factory_.createFresh(call);
          (void)astate->getPathFormula().addBounds(next_rc, *cur_rc + 1,
                                                   *cur_rc + 1);
          astate->getPostHeap().addEdge(cb_c, deref, Address(next_rc));
        }
      }
    }
  }
  astate->markWrittenTo(out_sp, call);
  if (!is_sret) {
    astate->getPostStack().add(call, Address(out_sp));
  }
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdWeakPtrReset(const llvm::CallBase *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));
  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  astate->getPostHeap().addEdge(this_av, Access(kWeakPtrField),
                                Address(null_val));
  astate->getPostHeap().removeEdge(this_av, Access(kWeakCbField));
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdStringViewCtor(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success(std::move(state));
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success(std::move(state));

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access data_field(0);
  if (call->arg_size() >= 2) {
    if (auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      AbstractValue src_av = astate->getCanonical(src_opt->addr);
      // If constructed from a std::string / std::string_view that already
      // exposes its borrowed buffer at field 0, borrow that buffer directly.
      if (Address *buf = astate->getPostHeap().findEdge(src_av, data_field)) {
        astate->getPostHeap().addEdge(this_av, data_field, *buf);
      } else {
        astate->getPostHeap().addEdge(this_av, data_field, *src_opt);
      }
      astate->markWrittenTo(this_av, call);
      return ModelResult::success(std::move(state));
    }
  }
  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  astate->getPostHeap().addEdge(this_av, data_field, Address(null_val));
  astate->markWrittenTo(this_av, call);
  return ModelResult::success(std::move(state));
}

ModelResult PulseModels::modelStdStringViewData(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  return modelStdVectorData(call, state, pred);
}

ModelResult PulseModels::modelStdMapAccess(const llvm::CallBase *call,
                                           ExecutionDomain &state,
                                           const llvm::BasicBlock *pred) {
  return modelStdVectorData(call, state, pred);
}

ModelResult PulseModels::modelStdMapErase(const llvm::CallBase *call,
                                          ExecutionDomain &state,
                                          const llvm::BasicBlock *pred) {
  return modelStdVectorClear(call, state, pred);
}

ModelResult PulseModels::modelStdVariantGet(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  return modelStdVectorData(call, state, pred);
}

ModelResult
PulseModels::modelStdIdentityPassThrough(const llvm::CallBase *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  auto *astate = state.getAstate();
  if (!astate || call->arg_size() < 1)
    return ModelResult::success(std::move(state));
  if (auto arg_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred)) {
    astate->getPostStack().add(call, *arg_opt);
  }
  return ModelResult::success(std::move(state));
}

} // namespace pulse
