#pragma once

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DebugInfoMetadata.h"

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace llvm {
class CallBase;
class Constant;
class Value;
class Type;
class Function;
class Instruction;
class DIType;
class DILocalVariable;
} // namespace llvm

namespace lotus {

class LLVMVFTableProvider;

std::optional<unsigned> getVFTIndex(const llvm::CallBase *CallSite);

std::optional<std::pair<const llvm::Value *, uint64_t>>
getVFTIndexAndVT(const llvm::CallBase *CallSite);

std::optional<std::tuple<const llvm::Value *, llvm::SmallVector<uint64_t, 3>,
                         llvm::Type *>>
getConstGEPFieldAccess(const llvm::Value *PtrOperand);

std::optional<std::tuple<const llvm::Value *, llvm::SmallVector<uint64_t, 3>,
                         llvm::Type *>>
getStructVCallInfo(const llvm::CallBase *CallSite);

bool isConsistentCall(const llvm::CallBase *CallSite,
                      const llvm::Function *DestFun);

llvm::DILocalVariable *getDILocalVariable(const llvm::Value *V);

llvm::DIType *getVarTypeFromIR(const llvm::Value *V);

const llvm::DIType *stripPointerTypes(const llvm::DIType *DITy);

const llvm::DIType *getReceiverType(const llvm::CallBase *CallSite);

std::string getReceiverTypeName(const llvm::CallBase *CallSite);

bool isAddressTakenFunction(const llvm::Function *F);

bool isVirtualCall(const llvm::Instruction *Inst,
                   const LLVMVFTableProvider &VTP);

bool isHeapAllocatingFunction(const llvm::Function *Fun) noexcept;
bool isHeapAllocatingFunction(llvm::StringRef FunName) noexcept;

const llvm::Function *walkConstInitPath(const llvm::Constant *Init,
                                        llvm::ArrayRef<uint64_t> Indices);

} // namespace lotus
