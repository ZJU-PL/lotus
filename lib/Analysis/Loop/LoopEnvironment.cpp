/*
 * Copyright 2026 Lotus contributors
 */
#include "Analysis/Loop/LoopEnvironment.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <cassert>

namespace lotus {
namespace analysis {
namespace loop {

LoopEnvironment::LoopEnvironment(
    LoopDependenceGraph *loopDG,
    std::vector<llvm::BasicBlock *> const &exitBlocks)
    : LoopEnvironment(loopDG, exitBlocks, std::set<llvm::Value *>{}) {}

LoopEnvironment::LoopEnvironment(
    LoopDependenceGraph *loopDG,
    std::vector<llvm::BasicBlock *> const &exitBlocks,
    const std::set<llvm::Value *> &excludeValues)
    : hasExitBlockEnv{exitBlocks.size() > 1}, exitBlockType{nullptr} {
  assert(loopDG != nullptr);

  for (auto *externalNode : loopDG->getExternalNodes()) {
    auto *externalValue = externalNode->getValue();
    if (externalValue == nullptr || excludeValues.count(externalValue) != 0) {
      continue;
    }

    bool isProducer = false;
    std::unordered_set<llvm::Instruction *> consumersOfLiveInValue;
    for (auto *edge : externalNode->getOutgoingEdges()) {
      if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
        continue;
      }
      auto *consumer =
          llvm::dyn_cast_or_null<llvm::Instruction>(edge->getDst()->getValue());
      if (consumer == nullptr) {
        continue;
      }
      isProducer = true;
      consumersOfLiveInValue.insert(consumer);
    }
    if (isProducer) {
      this->addLiveInValue(externalValue, consumersOfLiveInValue);
    }

    for (auto *edge : externalNode->getIncomingEdges()) {
      if (edge->getKind() != LoopDependenceEdgeKind::Variable) {
        continue;
      }
      auto *internalValue = edge->getSrc()->getValue();
      if (internalValue == nullptr) {
        continue;
      }
      if (!this->isProducer(internalValue)) {
        this->addLiveOutProducer(internalValue);
      }
      this->prodConsumers[internalValue].insert(externalValue);
    }
  }

  if (this->hasExitBlockEnv) {
    auto &context = exitBlocks.front()->getContext();
    this->exitBlockType = llvm::IntegerType::get(context, 32);
  }
}

llvm::iterator_range<std::vector<llvm::Value *>::iterator>
LoopEnvironment::getProducers(void) {
  return llvm::make_range(this->envProducers.begin(), this->envProducers.end());
}

llvm::iterator_range<std::set<int>::iterator>
LoopEnvironment::getEnvIDsOfLiveInVars(void) const {
  return llvm::make_range(this->liveInIDs.begin(), this->liveInIDs.end());
}

llvm::iterator_range<std::set<int>::iterator>
LoopEnvironment::getEnvIDsOfLiveOutVars(void) const {
  return llvm::make_range(this->liveOutIDs.begin(), this->liveOutIDs.end());
}

uint64_t LoopEnvironment::size(void) const {
  return this->envProducers.size() + (this->hasExitBlockEnv ? 1 : 0);
}

uint64_t LoopEnvironment::getNumberOfLiveIns(void) const {
  return this->liveInIDs.size();
}

uint64_t LoopEnvironment::getNumberOfLiveOuts(void) const {
  return this->liveOutIDs.size();
}

int64_t LoopEnvironment::getExitBlockID(void) const {
  return this->hasExitBlockEnv ? static_cast<int64_t>(this->envProducers.size())
                               : -1;
}

llvm::Type *LoopEnvironment::typeOfEnvironmentLocation(uint64_t id) const {
  if (id < this->envProducers.size()) {
    return this->envProducers[id]->getType();
  }
  return this->exitBlockType;
}

std::vector<llvm::Type *>
LoopEnvironment::getTypesOfEnvironmentLocations(void) const {
  std::vector<llvm::Type *> types;
  types.reserve(this->size());
  for (uint64_t i = 0; i < this->size(); ++i) {
    types.push_back(this->typeOfEnvironmentLocation(i));
  }
  return types;
}

bool LoopEnvironment::isLiveIn(llvm::Value *val) const {
  auto it = this->producerIDMap.find(val);
  if (it == this->producerIDMap.end()) {
    return false;
  }
  return this->liveInIDs.find(it->second) != this->liveInIDs.end();
}

bool LoopEnvironment::isProducer(llvm::Value *producer) const {
  return this->producerIDMap.find(producer) != this->producerIDMap.end();
}

llvm::Value *LoopEnvironment::getProducer(uint64_t id) const {
  assert(id < this->envProducers.size());
  return this->envProducers[id];
}

std::set<llvm::Value *> LoopEnvironment::consumersOf(llvm::Value *prod) const {
  auto it = this->prodConsumers.find(prod);
  if (it == this->prodConsumers.end()) {
    return {};
  }
  return it->second;
}

uint64_t LoopEnvironment::addProducer(llvm::Value *producer, bool liveIn) {
  auto existing = this->producerIDMap.find(producer);
  if (existing != this->producerIDMap.end()) {
    return static_cast<uint64_t>(existing->second);
  }

  auto nextID = static_cast<uint64_t>(this->envProducers.size());
  this->envProducers.push_back(producer);
  this->producerIDMap[producer] = static_cast<int>(nextID);
  if (liveIn) {
    this->liveInIDs.insert(static_cast<int>(nextID));
  } else {
    this->liveOutIDs.insert(static_cast<int>(nextID));
  }
  return nextID;
}

uint64_t LoopEnvironment::addLiveInProducer(llvm::Value *producer) {
  return this->addProducer(producer, true);
}

void LoopEnvironment::addLiveOutProducer(llvm::Value *producer) {
  (void)this->addProducer(producer, false);
}

uint64_t LoopEnvironment::addLiveInValue(
    llvm::Value *newLiveInValue,
    const std::unordered_set<llvm::Instruction *> &consumers) {
  auto newID = this->addLiveInProducer(newLiveInValue);
  for (auto *consumer : consumers) {
    this->prodConsumers[newLiveInValue].insert(consumer);
  }
  return newID;
}

} // namespace loop
} // namespace analysis
} // namespace lotus
