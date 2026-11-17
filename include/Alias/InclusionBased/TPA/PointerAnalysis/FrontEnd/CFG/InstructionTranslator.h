#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/FrontEnd/Type/TypeMap.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/InstVisitor.h>

namespace llvm {
class DataLayout;
} // namespace llvm

namespace tpa {

class CFG;
class CFGNode;

class InstructionTranslator
    : public llvm::InstVisitor<InstructionTranslator, CFGNode *> {
private:
  CFG &cfg;
  const TypeMap &typeMap;
  const llvm::DataLayout &dataLayout;

  CFGNode *
  createCopyNode(const llvm::Instruction *inst,
                 const llvm::SmallPtrSetImpl<const llvm::Value *> &srcs);

  CFGNode *handleUnsupportedInst(const llvm::Instruction &);

public:
  InstructionTranslator(CFG &c, const TypeMap &t, const llvm::DataLayout &d)
      : cfg(c), typeMap(t), dataLayout(d) {}

  // Default case
  CFGNode *visitFreezeInst(llvm::FreezeInst &);
  CFGNode *visitInstruction(llvm::Instruction &) { return nullptr; }

  // Supported cases
  CFGNode *visitAllocaInst(llvm::AllocaInst &);
  CFGNode *visitLoadInst(llvm::LoadInst &);
  CFGNode *visitStoreInst(llvm::StoreInst &);
  CFGNode *visitReturnInst(llvm::ReturnInst &);
  CFGNode *visitCallInst(llvm::CallInst &);
  CFGNode *visitInvokeInst(llvm::InvokeInst &);
  CFGNode *visitPHINode(llvm::PHINode &);
  CFGNode *visitSelectInst(llvm::SelectInst &);
  CFGNode *visitGetElementPtrInst(llvm::GetElementPtrInst &);
  CFGNode *visitIntToPtrInst(llvm::IntToPtrInst &);
  CFGNode *visitBitCastInst(llvm::BitCastInst &);

  // Unsupported cases
  CFGNode *visitExtractValueInst(llvm::ExtractValueInst &);
  CFGNode *visitInsertValueInst(llvm::InsertValueInst &);
  CFGNode *visitVAArgInst(llvm::VAArgInst &);
  CFGNode *visitExtractElementInst(llvm::ExtractElementInst &);
  CFGNode *visitInsertElementInst(llvm::InsertElementInst &);
  CFGNode *visitShuffleVectorInst(llvm::ShuffleVectorInst &);
  CFGNode *visitLandingPadInst(llvm::LandingPadInst &);
};

} // namespace tpa
