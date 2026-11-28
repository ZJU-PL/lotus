/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopStructure.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"

namespace lotus {
namespace analysis {
namespace loop {

namespace {

constexpr const char *kLoopIDMetadataKey = "noelle.loop.id";

} // namespace

LoopStructure::LoopStructure(llvm::Loop *loop) {
  this->depth = loop->getLoopDepth();
  this->header = loop->getHeader();
  this->preHeader = loop->getLoopPreheader();

  for (auto *bb : loop->blocks()) {
    this->bbs.insert(bb);
    if (loop->isLoopLatch(bb)) {
      this->latchBBs.insert(bb);
    }

    for (auto &inst : *bb) {
      if (loop->isLoopInvariant(&inst)) {
        this->invariants.insert(&inst);
      }
    }
  }

  llvm::SmallVector<llvm::BasicBlock *, 10> exits;
  loop->getExitBlocks(exits);
  this->exitBlocks.assign(exits.begin(), exits.end());

  llvm::SmallVector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>, 10>
      edges;
  loop->getExitEdges(edges);
  this->exitEdges.assign(edges.begin(), edges.end());
}

uint64_t LoopStructure::getID(void) const {
  auto *headerTerm = this->header->getTerminator();
  auto *metaNode = headerTerm->getMetadata(kLoopIDMetadataKey);
  if (metaNode == nullptr || metaNode->getNumOperands() == 0) {
    return 0;
  }

  auto *mdStr = llvm::dyn_cast<llvm::MDString>(metaNode->getOperand(0));
  if (mdStr == nullptr) {
    return 0;
  }

  uint64_t parsedID = 0;
  if (mdStr->getString().getAsInteger(10, parsedID)) {
    llvm::errs() << "LoopStructure::getID: WARNING = metadata \""
                 << kLoopIDMetadataKey << "\" is not a valid integer: \""
                 << mdStr->getString() << "\"\n";
    return 0;
  }
  return parsedID;
}

void LoopStructure::setID(uint64_t id) {
  auto *headerTerm = this->header->getTerminator();
  auto &context = headerTerm->getContext();
  auto *str = llvm::MDString::get(context, std::to_string(id));
  auto *node = llvm::MDNode::get(context, str);
  headerTerm->setMetadata(kLoopIDMetadataKey, node);
}

bool LoopStructure::doesHaveID(void) const {
  auto *headerTerm = this->header->getTerminator();
  return headerTerm->getMetadata(kLoopIDMetadataKey) != nullptr;
}

llvm::Function *LoopStructure::getFunction(void) const {
  return this->header->getParent();
}

llvm::BasicBlock *LoopStructure::getHeader(void) const { return this->header; }

llvm::BasicBlock *LoopStructure::getPreHeader(void) const {
  return this->preHeader;
}

llvm::Instruction *LoopStructure::getEntryInstruction(void) const {
  return &*this->header->begin();
}

llvm::BasicBlock *LoopStructure::getSuccessorWithinLoopOfTheHeader(void) const {
  for (auto *succ : llvm::successors(this->header)) {
    if (this->isIncluded(succ)) {
      return succ;
    }
  }
  return nullptr;
}

uint32_t LoopStructure::getNestingLevel(void) const { return this->depth; }

const std::unordered_set<llvm::BasicBlock *> &
LoopStructure::getLatches(void) const {
  return this->latchBBs;
}

const std::unordered_set<llvm::BasicBlock *> &
LoopStructure::getBasicBlocks(void) const {
  return this->bbs;
}

std::unordered_set<llvm::Instruction *>
LoopStructure::getInstructions(void) const {
  std::unordered_set<llvm::Instruction *> insts;
  for (auto *bb : this->bbs) {
    for (auto &inst : *bb) {
      insts.insert(&inst);
    }
  }
  return insts;
}

uint64_t LoopStructure::getNumberOfInstructions(void) const {
  uint64_t total = 0;
  for (auto *bb : this->bbs) {
    total += bb->size();
  }
  return total;
}

const std::vector<llvm::BasicBlock *> &
LoopStructure::getLoopExitBasicBlocks(void) const {
  return this->exitBlocks;
}

bool LoopStructure::isALoopExit(llvm::Instruction *instruction) const {
  for (const auto &edge : this->exitEdges) {
    if (edge.first->getTerminator() == instruction) {
      return true;
    }
  }
  return false;
}

const std::vector<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> &
LoopStructure::getLoopExitEdges(void) const {
  return this->exitEdges;
}

uint64_t LoopStructure::numberOfExitBasicBlocks(void) const {
  return this->exitBlocks.size();
}

bool LoopStructure::isLoopInvariant(llvm::Value *value) const {
  if (auto *inst = llvm::dyn_cast<llvm::Instruction>(value)) {
    if (!this->isIncluded(inst->getParent())) {
      return true;
    }
    return this->invariants.find(inst) != this->invariants.end();
  }
  return llvm::isa<llvm::Constant>(value) || llvm::isa<llvm::Argument>(value);
}

bool LoopStructure::isIncluded(llvm::BasicBlock *bb) const {
  return this->bbs.find(bb) != this->bbs.end();
}

bool LoopStructure::isIncluded(llvm::Instruction *instruction) const {
  return this->isIncluded(instruction->getParent());
}

void LoopStructure::print(llvm::raw_ostream &stream) const {
  stream << "Loop summary: " << this->getID() << ", depth: " << this->depth
         << "\n";
  this->header->begin()->print(stream);
  stream << "\n";
}

} // namespace loop
} // namespace analysis
} // namespace lotus
