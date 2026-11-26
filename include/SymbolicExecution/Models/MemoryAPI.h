/** @file MemoryAPI.h @brief Allocator, deallocator, and known library API models. */
#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Type.h"

#include <string>
#include <utility>
#include <vector>

namespace SymbolicExecution {

class AllocatorAPI {
public:
  AllocatorAPI(std::string Name, std::vector<int> SizeArgs,
               bool ArrayAllocation = false, int GFPFlag = -1,
               bool Zeroed = false, llvm::Type *ObjTy = nullptr)
      : Name(std::move(Name)), SizeArgs(std::move(SizeArgs)),
        ArrayAllocation(ArrayAllocation), GFPFlag(GFPFlag), Zeroed(Zeroed),
        ObjTy(ObjTy) {}

  const std::string &getName() const { return Name; }
  const std::vector<int> &getSizeArgs() const { return SizeArgs; }
  bool isArrayAlloc() const { return ArrayAllocation; }
  bool hasValidGFPFlag() const { return GFPFlag != -1; }
  int getGFPFlag() const { return GFPFlag; }
  bool isZeroed() const { return Zeroed; }
  llvm::Type *getObjTy() const { return ObjTy; }

  std::string toString() const;

  static const AllocatorAPI *get(llvm::Function *F);

private:
  std::string Name;
  std::vector<int> SizeArgs;
  bool ArrayAllocation = false;
  int GFPFlag = -1;
  bool Zeroed = false;
  llvm::Type *ObjTy = nullptr;

  AllocatorAPI() = default;
};

/// Returns heap allocation size argument indices for a call site, if recognized.
std::vector<int> getHeapAllocSizeArgs(const llvm::CallBase *CB);

/// Returns true if F is a recognized heap deallocator (free / operator delete).
bool isFreeLikeFunction(const llvm::Function *F);

/// Returns true if LibName is modeled or explicitly known by the engine.
bool isKnownLib(const std::string &LibName);

} // namespace SymbolicExecution
