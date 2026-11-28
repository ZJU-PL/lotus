/** @file PulseModels.h @brief Modeled function specifications for Pulse interprocedural analysis. */
#pragma once

#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>

namespace pulse {

class PulseChecker;

struct ModelResult {
  bool handled;
  std::vector<ExecutionDomain> states;

  static ModelResult notHandled() { return {false, {}}; }
  static ModelResult success(std::vector<ExecutionDomain> s) {
    return {true, std::move(s)};
  }
  static ModelResult success(ExecutionDomain &&s) {
    std::vector<ExecutionDomain> v;
    v.reserve(1);
    v.push_back(std::move(s));
    return {true, std::move(v)};
  }
};

using PulseModel = std::function<ModelResult(
    PulseChecker &checker, const llvm::CallBase *call, ExecutionDomain &state,
    const llvm::BasicBlock *pred)>;

/**
 * PulseModels: Manages C/C++/POSIX library models.
 * Taint queries delegate directly to TaintOperations::getConfig().
 */
class PulseModels {
public:
  explicit PulseModels(PulseChecker &checker);

  ModelResult dispatch(const llvm::CallBase *call, ExecutionDomain &state,
                       const llvm::BasicBlock *pred,
                       const llvm::Function *resolved_callee = nullptr);

  bool hasModel(const llvm::Function *func) const;

  bool isTaintSource(const std::string &func_name) const;
  bool isTaintSink(const std::string &func_name) const;
  bool isTaintSanitizer(const std::string &func_name) const;

private:
  PulseChecker &checker_;
  PulseOperations &ops_;
  AbstractValueFactory &factory_;

  std::map<std::string, PulseModel> models_;
  // External-declaration-only C/POSIX/runtime models (never override a
  // user-defined function body in the module).
  std::map<std::string, PulseModel> decl_models_;

  struct PatternModel {
    std::string class_substr;
    std::string method_substr;
    std::string mangled_glob;
    PulseModel model;
  };
  std::vector<PatternModel> pattern_models_;
  mutable std::unordered_map<const llvm::Function *, const PulseModel *>
      fn_model_cache_;
  mutable std::unordered_map<std::string, const PulseModel *>
      name_model_cache_;

  void registerStandardModels();
  void registerCppModels();

  const PulseModel *findModelByName(const std::string &funcName,
                                    bool allow_pattern_models = true) const;
  const PulseModel *findModel(const llvm::Function *func) const;

  // Memory & resource models
  ModelResult modelMalloc(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred,
                          AllocatorKind kind = AllocatorKind::CMalloc);
  ModelResult modelCalloc(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelPosixMemalign(const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelAlignedAlloc(const llvm::CallBase *call,
                                ExecutionDomain &state,
                                const llvm::BasicBlock *pred);
  ModelResult modelMmap(const llvm::CallBase *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelMunmap(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelFree(const llvm::CallBase *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred,
                        InvalidationKind inv_kind = InvalidationKind::CFree);
  ModelResult modelRealloc(const llvm::CallBase *call, ExecutionDomain &state,
                           const llvm::BasicBlock *pred);
  ModelResult modelFileOpen(const llvm::CallBase *call, ExecutionDomain &state,
                            const llvm::BasicBlock *pred,
                            AllocatorKind kind = AllocatorKind::FileHandle);
  ModelResult modelFileClose(const llvm::CallBase *call, ExecutionDomain &state,
                             const llvm::BasicBlock *pred);
  ModelResult modelLock(const llvm::CallBase *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelUnlock(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStringCopy(const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred);
  ModelResult modelStrlen(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrcat(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred, bool bounded);
  ModelResult modelSprintf(const llvm::CallBase *call, ExecutionDomain &state,
                           const llvm::BasicBlock *pred, bool bounded);
  ModelResult modelStrdup(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrchr(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrstr(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrcmp(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred, bool bounded,
                          bool case_insensitive, bool is_memcmp = false);
  ModelResult modelMemcpy(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelMemmove(const llvm::CallBase *call, ExecutionDomain &state,
                           const llvm::BasicBlock *pred);
  ModelResult modelMemset(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelBzero(const llvm::CallBase *call, ExecutionDomain &state,
                         const llvm::BasicBlock *pred);
  ModelResult modelMemchr(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelCheckedPtrCall(const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred,
                                  const std::vector<unsigned> &read_ptr_args,
                                  const std::vector<unsigned> &nullable_ptr_args,
                                  const std::vector<unsigned> &out_ptr_args,
                                  int oob_buf_arg = -1, int oob_len_arg = -1,
                                  bool returns_nonnull_ptr = false,
                                  int vararg_out_start = -1);
  ModelResult modelErrnoOrCtypeLoc(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelPureFunction(const llvm::CallBase *call,
                                ExecutionDomain &state,
                                const llvm::BasicBlock *pred);
  ModelResult modelRead(const llvm::CallBase *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelWrite(const llvm::CallBase *call, ExecutionDomain &state,
                         const llvm::BasicBlock *pred);
  ModelResult modelFread(const llvm::CallBase *call, ExecutionDomain &state,
                         const llvm::BasicBlock *pred);
  ModelResult modelFwrite(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelFgets(const llvm::CallBase *call, ExecutionDomain &state,
                         const llvm::BasicBlock *pred);
  ModelResult modelGets(const llvm::CallBase *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelPipe(const llvm::CallBase *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);
  ModelResult modelDup2(const llvm::CallBase *call, ExecutionDomain &state,
                        const llvm::BasicBlock *pred);

  // Exception runtime
  ModelResult modelCxaThrow(const llvm::CallBase *call, ExecutionDomain &state,
                            const llvm::BasicBlock *pred);

  // C++ Container & Smart Pointer Models
  ModelResult modelStdVectorPushBack(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdVectorClear(const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred);
  ModelResult modelStdVectorAccess(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdVectorData(const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelStdStringCStr(const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelStdStringMutate(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);

  // std::unique_ptr
  ModelResult modelStdUniquePtrCtor(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrMove(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrRelease(const llvm::CallBase *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrReset(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrGet(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdSmartPtrBool(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdUniquePtrDtor(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);

  // std::shared_ptr
  ModelResult modelStdSharedPtrCtor(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);
  ModelResult modelStdSharedPtrCopyCtor(const llvm::CallBase *call,
                                        ExecutionDomain &state,
                                        const llvm::BasicBlock *pred);
  ModelResult modelStdSharedPtrCopyAssign(const llvm::CallBase *call,
                                          ExecutionDomain &state,
                                          const llvm::BasicBlock *pred);
  ModelResult modelStdSharedPtrGet(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdSharedPtrReset(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdSharedPtrDtor(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);

  // std::make_unique / std::make_shared / std::weak_ptr / std::string_view /
  // std::map / std::variant / std::move / std::forward
  ModelResult modelStdMakeUnique(const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelStdMakeShared(const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelStdWeakPtrCtor(const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred);
  ModelResult modelStdWeakPtrExpired(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdWeakPtrLock(const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred);
  ModelResult modelStdWeakPtrReset(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdStringViewCtor(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdStringViewData(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdMapAccess(const llvm::CallBase *call,
                                ExecutionDomain &state,
                                const llvm::BasicBlock *pred);
  ModelResult modelStdMapErase(const llvm::CallBase *call,
                               ExecutionDomain &state,
                               const llvm::BasicBlock *pred);
  ModelResult modelStdVariantGet(const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelStdIdentityPassThrough(const llvm::CallBase *call,
                                          ExecutionDomain &state,
                                          const llvm::BasicBlock *pred);

  // std::optional
  ModelResult modelStdOptionalReset(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);
  ModelResult modelStdOptionalValue(const llvm::CallBase *call,
                                    ExecutionDomain &state,
                                    const llvm::BasicBlock *pred);
  ModelResult modelStdOptionalHasValue(const llvm::CallBase *call,
                                       ExecutionDomain &state,
                                       const llvm::BasicBlock *pred);

  // POSIX & async escape sinks
  ModelResult modelSocket(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelPthreadCreate(const llvm::CallBase *call,
                                 ExecutionDomain &state,
                                 const llvm::BasicBlock *pred);
  ModelResult modelThrdCreate(const llvm::CallBase *call,
                              ExecutionDomain &state,
                              const llvm::BasicBlock *pred);
  ModelResult modelDispatchAsyncF(const llvm::CallBase *call,
                                  ExecutionDomain &state,
                                  const llvm::BasicBlock *pred);

  // Taint models
  ModelResult modelTaintSource(const llvm::CallBase *call,
                               ExecutionDomain &state,
                               const llvm::BasicBlock *pred,
                               const std::string &kind);
  ModelResult modelTaintSink(const llvm::CallBase *call, ExecutionDomain &state,
                             const llvm::BasicBlock *pred,
                             const std::string &kind);
};

} // namespace pulse
