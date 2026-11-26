#include "Checker/Pulse/Interproc/PulseModels.h"

#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLatentIssue.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <limits>
#include <optional>

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace pulse {

namespace {

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

static void reportOutOfBoundsAccess(const llvm::Instruction *loc,
                                    const Address &addr, const char *detail) {
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
    reportOutOfBoundsAccess(loc, addr, detail);
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

  Trace trace = Trace::fromValueHistory(escaped_addr.history);
  trace.addEvent(call, reason);
  auto diag = std::make_unique<StackVariableAddressEscape>(
      call, canon, "Stack address escapes via call",
      "Do not pass addresses of local variables to APIs that store them for "
      "later use.",
      std::move(trace));
  DiagnosticManager::getInstance().report(std::move(diag));
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
  auto it = models_.find(funcName);
  if (it != models_.end()) {
    return &it->second;
  }

  const std::string demangled = llvm::demangle(funcName);
  const std::string demangled_no_args =
      demangled.substr(0, demangled.find('('));

  for (const auto &pm : pattern_models_) {
    if (!pm.mangled_glob.empty() && globMatch(pm.mangled_glob, funcName)) {
      return &pm.model;
    }
    if (!pm.class_substr.empty() && !pm.method_substr.empty()) {
      if (demangled_no_args.find(pm.class_substr) != std::string::npos &&
          demangled_no_args.find(pm.method_substr) != std::string::npos) {
        return &pm.model;
      }
    }
  }

  return nullptr;
}

ModelResult PulseModels::dispatch(const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred,
                                  const llvm::Function *resolved_callee) {
  const llvm::Function *func =
      resolved_callee ? resolved_callee : call->getCalledFunction();
  if (!func || !func->hasName()) {
    return ModelResult::notHandled();
  }

  std::string funcName = func->getName().str();
  if (const PulseModel *model = findModelByName(funcName)) {
    return (*model)(checker_, call, state, pred);
  }

  return ModelResult::notHandled();
}

bool PulseModels::hasModel(const llvm::Function *func) const {
  if (!func || !func->hasName()) {
    return false;
  }
  return findModelByName(func->getName().str()) != nullptr;
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

  // String & memory operations
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

  // std::shared_ptr (copy ctor must precede generic ctor!)
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

  // std::basic_string
  pattern_models_.push_back(
      {"basic_string<", "::c_str", "_ZN*basic_string*c_str*",
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
    return ModelResult::success({state});
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
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto arg_opt = ops_.eval(*astate, call->getArgOperand(3), call, pred);
  if (arg_opt) {
    reportStackEscapeOnCall(*astate, call, *arg_opt,
                            "Passing stack-derived context to pthread_create "
                            "(escapes to new thread)");
  }
  return ModelResult::success({state});
}

ModelResult PulseModels::modelThrdCreate(const llvm::CallBase *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto arg_opt = ops_.eval(*astate, call->getArgOperand(2), call, pred);
  if (arg_opt) {
    reportStackEscapeOnCall(
        *astate, call, *arg_opt,
        "Passing stack-derived context to thrd_create (escapes to new thread)");
  }
  return ModelResult::success({state});
}

ModelResult PulseModels::modelDispatchAsyncF(const llvm::CallBase *call,
                                             ExecutionDomain &state,
                                             const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto ctx_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (ctx_opt) {
    reportStackEscapeOnCall(*astate, call, *ctx_opt,
                            "Passing stack-derived context to dispatch_async_f "
                            "(escapes asynchronously)");
  }
  return ModelResult::success({state});
}

ModelResult PulseModels::modelMalloc(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred,
                                     AllocatorKind kind) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

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
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});
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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelAlignedAlloc(const llvm::CallBase *call,
                                           ExecutionDomain &state,
                                           const llvm::BasicBlock *pred) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});
  AbstractValue ret_val = factory_.createFresh(call);
  ops_.allocate(*astate, ret_val, call, AllocatorKind::CMalloc);
  astate->addNonNull(ret_val);
  if (call->arg_size() >= 2) {
    if (auto sz = getConstUInt64(call->getArgOperand(1))) {
      astate->setAllocationSize(ret_val, *sz);
    }
  }
  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success({state});
}

ModelResult PulseModels::modelMmap(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});
  AbstractValue ret_val = factory_.createFresh(call);
  ops_.allocate(*astate, ret_val, call, AllocatorKind::CustomResource);
  astate->addNonNull(ret_val);
  if (call->arg_size() >= 2) {
    if (auto sz = getConstUInt64(call->getArgOperand(1))) {
      astate->setAllocationSize(ret_val, *sz);
    }
  }
  astate->getPostStack().add(call, Address(ret_val));
  return ModelResult::success({state});
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
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

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
      return ModelResult::success({state});
  }

  AbstractValue canon_ptr = astate->getCanonical(ptr_opt->addr);

  if (astate->getPathFormula().isNull(canon_ptr) ||
      (astate->getPostAttrs().has(canon_ptr, Attribute::Null) &&
       !astate->getPathFormula().isNonNull(canon_ptr))) {
    return ModelResult::success({state});
  }

  if (astate->getPostAttrs().has(canon_ptr, Attribute::Stack) ||
      astate->getPostAttrs().has(canon_ptr, Attribute::Global)) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call, "Invalid free: non-heap pointer");
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
    return ModelResult::success({state});
  }

  // Detect freeing an interior pointer (offset > 0 from a heap allocation).
  AbstractValue alloc_root = astate->getAllocationRoot(canon_ptr);
  if (!(alloc_root == canon_ptr) &&
      (astate->getPostAttrs().has(alloc_root, Attribute::Allocated) ||
       astate->getAllocationInfo(alloc_root).has_value())) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call,
                   "Invalid free: interior pointer not at start of allocation");
    auto diag = std::make_unique<InvalidFree>(
        call, canon_ptr, "Invalid free of interior pointer",
        "Pass the original pointer returned by malloc/new to free/delete.",
        std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
    astate->setPathFormula(
        std::make_unique<PulseFormula>(PulseFormula::contradiction()));
    return ModelResult::success({state});
  }

  if (astate->getPostAttrs().has(canon_ptr, Attribute::Invalid)) {
    Trace trace = Trace::fromValueHistory(ptr_opt->history);
    trace.addEvent(call, "Double free: freeing already freed pointer");
    if (auto inv_info = astate->getInvalidationInfo(canon_ptr)) {
      trace.addEvent(inv_info->second, "First free");
    }
    auto diag = std::make_unique<AccessToInvalidAddress>(
        call, "Double free detected", "Freeing already freed memory",
        "Ensure each allocation is freed exactly once", IssueType::UseAfterFree,
        std::move(trace), inv_kind);
    DiagnosticManager::getInstance().report(std::move(diag));
  }

  ops_.invalidate(*astate, *ptr_opt, call, inv_kind);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelRealloc(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});
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
    return ModelResult::success({state});
  }

  ExecutionDomain failure_state;
  const bool fork_null = options::modelMallocFailure();
  if (fork_null) {
    failure_state = state.clone();
  }

  if (fork_null && ptr_opt) {
    // On the explicit success branch when null-forking is enabled, invalidate
    // the old buffer immediately (failure branch keeps old buffer valid).
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
    return ModelResult::success({state});

  AbstractValue ret_val = factory_.createFresh(call);
  Address ret_addr(ret_val);
  ops_.allocate(*astate, ret_val, call, kind);
  astate->getPostAttrs().add(ret_val, Attribute::FileHandle);
  astate->addNonNull(ret_val);
  ret_addr.history.addEvent(ValueHistory::EventKind::Allocation, call,
                            call->getFunction(), "Resource acquired");
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFileClose(const llvm::CallBase *call,
                                        ExecutionDomain &state,
                                        const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!ptr_opt)
    return ModelResult::success({state});

  AbstractValue canon = astate->getCanonical(ptr_opt->addr);
  if (astate->getPostAttrs().has(canon, Attribute::Invalid)) {
    return ModelResult::success({reportModelAccessError(
        checker_, *astate, OperationResult::UseAfterFree, *ptr_opt, call,
        "Closing already closed resource")});
  }

  ops_.invalidate(*astate, *ptr_opt, call, InvalidationKind::FClose);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelLock(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  (void)call;
  (void)pred;
  return ModelResult::success({state});
}

ModelResult PulseModels::modelUnlock(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  (void)call;
  (void)pred;
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStringCopy(const llvm::CallBase *call,
                                         ExecutionDomain &state,
                                         const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

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
    TaintOperations::propagate(*astate, c_src, c_dest, call);
    if (astate->isInPre(c_src)) {
      astate->addPropagateTaintFrom(c_dest, {{c_src}, "strcpy"});
    }
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success({state});
}

//===----------------------------------------------------------------------===//
// C++ Container & Smart Pointer Models
//===----------------------------------------------------------------------===//

ModelResult PulseModels::modelStdVectorPushBack(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdVectorClear(const llvm::CallBase *call,
                                             ExecutionDomain &state,
                                             const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access data_field(0);
  if (Address *old_buf = astate->getPostHeap().findEdge(this_av, data_field)) {
    ops_.invalidate(*astate, *old_buf, call, InvalidationKind::StdVectorClear);
  }
  astate->markWrittenTo(this_av, call);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdVectorData(const llvm::CallBase *call,
                                            ExecutionDomain &state,
                                            const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
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
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access ptr_access(0);
  if (call->arg_size() >= 2) {
    if (auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      astate->getPostHeap().addEdge(this_av, ptr_access, *val_opt);
      astate->markWrittenTo(this_av, call);
      return ModelResult::success({state});
    }
  }
  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  astate->getPostHeap().addEdge(this_av, ptr_access, Address(null_val));
  astate->markWrittenTo(this_av, call);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdUniquePtrMove(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto dst_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (!dst_opt || !src_opt)
    return ModelResult::success({state});

  AbstractValue dst_av = astate->getCanonical(dst_opt->addr);
  AbstractValue src_av = astate->getCanonical(src_opt->addr);
  Access ptr_access(0);

  // Transfer ownership from src to dst, leaving src holding null.
  Address moved_ptr;
  if (Address *src_raw = astate->getPostHeap().findEdge(src_av, ptr_access)) {
    moved_ptr = *src_raw;
  } else {
    moved_ptr = Address(factory_.createFresh(call));
    astate->abduceToPre(src_av, ptr_access, moved_ptr);
  }

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);

  astate->getPostHeap().addEdge(dst_av, ptr_access, moved_ptr);
  astate->getPostHeap().addEdge(src_av, ptr_access, Address(null_val));
  astate->markWrittenTo(dst_av, call);
  astate->markWrittenTo(src_av, call);
  if (call->getType()->isPointerTy()) {
    astate->getPostStack().add(call, *dst_opt);
  }
  return ModelResult::success({state});
}

ModelResult
PulseModels::modelStdUniquePtrRelease(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);

  Address *raw_ptr = astate->getPostHeap().findEdge(this_av, ptr_access);
  Address ret_addr = raw_ptr ? *raw_ptr : Address(factory_.createFresh(call));

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  astate->getPostHeap().addEdge(this_av, ptr_access, Address(null_val));
  astate->markWrittenTo(this_av, call);
  astate->getPostStack().add(call, ret_addr);

  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdUniquePtrReset(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);
  if (Address *old_ptr = astate->getPostHeap().findEdge(this_av, ptr_access)) {
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

  astate->getPostHeap().addEdge(this_av, ptr_access, new_addr);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdUniquePtrGet(const llvm::CallBase *call,
                                              ExecutionDomain &state,
                                              const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);

  Address *raw_ptr = astate->getPostHeap().findEdge(this_av, ptr_access);
  Address ret_addr;

  if (!raw_ptr) {
    AbstractValue new_ptr = factory_.createFresh(call);
    ret_addr = Address(new_ptr);
    astate->getPostHeap().addEdge(this_av, ptr_access, ret_addr);
    astate->abduceToPre(this_av, ptr_access, ret_addr);
  } else {
    ret_addr = *raw_ptr;
  }

  astate->getPostStack().add(call, ret_addr);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdUniquePtrDtor(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_ptr_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_ptr_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_ptr_opt->addr);
  Access ptr_access(0);
  if (Address *old_ptr = astate->getPostHeap().findEdge(this_av, ptr_access)) {
    AbstractValue old_c = astate->getCanonical(old_ptr->addr);
    if (!astate->getPathFormula().isNull(old_c) &&
        !astate->getPostAttrs().has(old_c, Attribute::Null)) {
      ops_.invalidate(*astate, *old_ptr, call, InvalidationKind::CppDelete);
    }
  }
  return ModelResult::success({state});
}

//===----------------------------------------------------------------------===//
// std::shared_ptr Models (field 0 = raw ptr, field 1 = control block -> rc)
//===----------------------------------------------------------------------===//

ModelResult PulseModels::modelStdSharedPtrCtor(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access ptr_field(0);
  Access cb_field(1);
  Access deref(AccessKind::Dereference);

  if (call->arg_size() >= 2) {
    if (auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      astate->getPostHeap().addEdge(this_av, ptr_field, *val_opt);
      AbstractValue cb = factory_.createFresh(call);
      AbstractValue one = factory_.createFresh(call);
      (void)astate->getPathFormula().addBounds(one, 1, 1);
      astate->getPostHeap().addEdge(cb, deref, Address(one));
      astate->getPostHeap().addEdge(this_av, cb_field, Address(cb));
      astate->markWrittenTo(this_av, call);
      return ModelResult::success({state});
    }
  }

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  astate->getPostHeap().addEdge(this_av, ptr_field, Address(null_val));
  astate->markWrittenTo(this_av, call);
  return ModelResult::success({state});
}

ModelResult
PulseModels::modelStdSharedPtrCopyCtor(const llvm::CallBase *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto dst_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto src_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);
  if (!dst_opt || !src_opt)
    return ModelResult::success({state});

  AbstractValue dst_av = astate->getCanonical(dst_opt->addr);
  AbstractValue src_av = astate->getCanonical(src_opt->addr);
  Access ptr_field(0);
  Access cb_field(1);
  Access deref(AccessKind::Dereference);

  if (Address *raw = astate->getPostHeap().findEdge(src_av, ptr_field)) {
    astate->getPostHeap().addEdge(dst_av, ptr_field, *raw);
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
  return ModelResult::success({state});
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
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access ptr_field(0);
  Access cb_field(1);
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
    if (Address *old_ptr = astate->getPostHeap().findEdge(this_av, ptr_field)) {
      AbstractValue old_c = astate->getCanonical(old_ptr->addr);
      if (!astate->getPathFormula().isNull(old_c) &&
          !astate->getPostAttrs().has(old_c, Attribute::Null)) {
        ops_.invalidate(*astate, *old_ptr, call, InvalidationKind::CppDelete);
      }
    }
  }
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdSharedPtrReset(const llvm::CallBase *call,
                                                ExecutionDomain &state,
                                                const llvm::BasicBlock *pred) {
  modelStdSharedPtrDtor(call, state, pred);
  auto *astate = state.getAstate();
  if (!astate || call->arg_size() < 1)
    return ModelResult::success({state});
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access ptr_field(0);
  Access cb_field(1);
  Access deref(AccessKind::Dereference);

  if (call->arg_size() > 1) {
    if (auto val_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred)) {
      astate->getPostHeap().addEdge(this_av, ptr_field, *val_opt);
      AbstractValue cb = factory_.createFresh(call);
      AbstractValue one = factory_.createFresh(call);
      (void)astate->getPathFormula().addBounds(one, 1, 1);
      astate->getPostHeap().addEdge(cb, deref, Address(one));
      astate->getPostHeap().addEdge(this_av, cb_field, Address(cb));
      astate->markWrittenTo(this_av, call);
      return ModelResult::success({state});
    }
  }

  AbstractValue null_val = factory_.createFresh(call);
  astate->getPostAttrs().add(null_val, Attribute::Null);
  (void)astate->getPathFormula().addNull(null_val);
  astate->getPostHeap().addEdge(this_av, ptr_field, Address(null_val));
  astate->getPostHeap().removeEdge(this_av, cb_field);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdOptionalReset(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

  AbstractValue this_av = astate->getCanonical(this_opt->addr);
  Access engaged_field(1);
  AbstractValue zero_val = factory_.createFresh(call);
  (void)astate->getPathFormula().addBounds(zero_val, 0, 0);
  (void)astate->getPathFormula().addNull(zero_val);
  astate->getPostHeap().addEdge(this_av, engaged_field, Address(zero_val));
  astate->getPostAttrs().add(this_av, Attribute::Null);
  astate->markWrittenTo(this_av, call);
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStdOptionalValue(const llvm::CallBase *call,
                                               ExecutionDomain &state,
                                               const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
}

ModelResult
PulseModels::modelStdOptionalHasValue(const llvm::CallBase *call,
                                      ExecutionDomain &state,
                                      const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});
  auto *astate = state.getAstate();
  auto this_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!this_opt)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
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
    return ModelResult::success({state});
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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelTaintSink(const llvm::CallBase *call,
                                        ExecutionDomain &state,
                                        const llvm::BasicBlock *pred,
                                        const std::string &kind) {
  auto *astate = state.getAstate();
  if (!astate) {
    return ModelResult::success({state});
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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelCalloc(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  (void)pred;
  auto *astate = state.getAstate();
  if (!astate)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStrdup(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto src_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!src_opt)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStrchr(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  if (!str_opt)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelStrstr(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 2)
    return ModelResult::success({state});

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
  return ModelResult::success({state});
}

ModelResult PulseModels::modelMemcpy(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

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
    ops_.copySubgraph(*astate, c_src, c_dest);
    TaintOperations::propagate(*astate, c_src, c_dest, call);
    if (astate->isInPre(c_src)) {
      astate->addPropagateTaintFrom(c_dest, {{c_src}, "memcpy"});
    }
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success({state});
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
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto dest_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);
  auto value_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (dest_opt && value_opt) {
    checkLengthAgainstAllocation(*astate, *dest_opt, call->getArgOperand(2),
                                 call,
                                 "memset writes beyond destination buffer");
    ops_.writeDeref(*astate, *dest_opt, *value_opt, call);
    astate->getPostStack().add(call, *dest_opt);
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelRead(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto buf_opt = ops_.eval(*astate, call->getArgOperand(1), call, pred);

  if (buf_opt) {
    checkLengthAgainstAllocation(*astate, *buf_opt, call->getArgOperand(2),
                                 call, "read writes beyond destination buffer");
    AbstractValue dummy = factory_.createFresh(call);
    ops_.writeDeref(*astate, *buf_opt, Address(dummy), call);

    AbstractValue ret_val = factory_.createFresh(call);
    astate->getPostStack().add(call, Address(ret_val));
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelWrite(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

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

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFread(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 4)
    return ModelResult::success({state});

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
          reportOutOfBoundsAccess(call, *ptr_opt,
                                  "fread writes beyond destination buffer");
          astate->getPostAttrs().add(canon, Attribute::OutOfBounds);
        }
      }
    }
    auto stream_read = ops_.readDeref(*astate, *stream_opt, call);
    if (stream_read.first == OperationResult::Success) {
      AbstractValue dummy = factory_.createFresh(call);
      ops_.writeDeref(*astate, *ptr_opt, Address(dummy), call);
      AbstractValue ret_val = factory_.createFresh(call);
      astate->getPostStack().add(call, Address(ret_val));
    }
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFwrite(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred) {
  if (call->arg_size() < 4)
    return ModelResult::success({state});

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
          reportOutOfBoundsAccess(call, *ptr_opt,
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

  return ModelResult::success({state});
}

ModelResult PulseModels::modelFgets(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred) {
  if (call->arg_size() < 3)
    return ModelResult::success({state});

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
      astate->getPostStack().add(call, *str_opt);
    }
  }

  return ModelResult::success({state});
}

ModelResult PulseModels::modelGets(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred) {
  if (call->arg_size() < 1)
    return ModelResult::success({state});

  auto *astate = state.getAstate();
  auto str_opt = ops_.eval(*astate, call->getArgOperand(0), call, pred);

  if (str_opt) {
    AbstractValue dummy = factory_.createFresh(call);
    ops_.writeDeref(*astate, *str_opt, Address(dummy), call);
    astate->getPostStack().add(call, *str_opt);
  }

  return ModelResult::success({state});
}

} // namespace pulse
