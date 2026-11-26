/** @file PulseModels.h @brief Modeled function specifications for Pulse interprocedural analysis. */
#pragma once

#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <functional>
#include <map>
#include <set>
#include <string>
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

  struct PatternModel {
    std::string class_substr;
    std::string method_substr;
    std::string mangled_glob;
    PulseModel model;
  };
  std::vector<PatternModel> pattern_models_;

  void registerStandardModels();
  void registerCppModels();

  const PulseModel *findModelByName(const std::string &funcName) const;

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
  ModelResult modelStrdup(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrchr(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelStrstr(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelMemcpy(const llvm::CallBase *call, ExecutionDomain &state,
                          const llvm::BasicBlock *pred);
  ModelResult modelMemmove(const llvm::CallBase *call, ExecutionDomain &state,
                           const llvm::BasicBlock *pred);
  ModelResult modelMemset(const llvm::CallBase *call, ExecutionDomain &state,
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
  ModelResult modelStdSharedPtrGet(const llvm::CallBase *call,
                                   ExecutionDomain &state,
                                   const llvm::BasicBlock *pred);
  ModelResult modelStdSharedPtrReset(const llvm::CallBase *call,
                                     ExecutionDomain &state,
                                     const llvm::BasicBlock *pred);
  ModelResult modelStdSharedPtrDtor(const llvm::CallBase *call,
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
