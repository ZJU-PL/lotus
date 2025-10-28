#ifndef UNDERAPPROX_CANONICAL_H
#define UNDERAPPROX_CANONICAL_H

#include <llvm/IR/Operator.h>

namespace llvm {
class DataLayout;
class Value;
}

namespace UnderApprox {

/// “Strip everything that does **not** change the run-time address”
/// (bit-cast, no-op addrspacecast, launder/strip.invariant.group, …).
const llvm::Value *stripNoopCasts(const llvm::Value *V);

/// Same base + identical in-bounds constant offset?
bool sameConstOffset(const llvm::DataLayout &DL,
                     const llvm::Value *A, const llvm::Value *B);

bool isZeroGEP(const llvm::Value *V);        // all indices = 0
bool isRoundTripCast(const llvm::Value *A,
                     const llvm::Value *B);  // inttoptr(ptrtoint X) ⇔ X
bool isNoopAddrSpaceCast(const llvm::Value *V);

} // end namespace UnderApprox
#endif