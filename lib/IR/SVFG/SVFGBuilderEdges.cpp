//===- SVFGBuilderEdges.cpp -- SVFG Edge Building Implementation
//---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// This file contains intra-procedural edge building methods for SVFGBuilder
//
//===----------------------------------------------------------------------===//

#include "IR/SVFG/SVFGBuilder.h"

#include <functional>
#include <map>

using namespace lotus::analysis;
using namespace llvm;

void SVFGBuilder::buildEdges() {
  buildAggregateEdges();
  buildDirectEdges();
  buildCopyEdges();
  buildGepEdges();
  buildPhiEdges();
  if (!config.pointerOnly) {
    buildCmpEdges();
    buildBranchEdges();
  }
  buildMemoryEdges(); // This calls buildLoadEdges() and buildStoreEdges()
}

void SVFGBuilder::buildAggregateEdges() {
  using Path = std::vector<unsigned>;
  std::map<const Value *, std::map<Path, SVFGNode *>> fields;
  std::function<SVFGNode *(const Value *, const Path &, const ICFGNode *)> field;
  field = [&](const Value *value, const Path &path,
              const ICFGNode *at) -> SVFGNode * {
    if (path.empty() && value->getType()->isPointerTy())
      if (auto *node = svfg->getValueNode(value))
        return node;
    auto &cached = fields[value][path];
    if (cached)
      return cached;
    // Publish the field definition before visiting operands: aggregate PHIs
    // can have backedges. A synthetic pointer copy also survives the
    // pointer-only view without pretending the aggregate itself is a pointer.
    const bool pointerLeaf = path.empty() && value->getType()->isPointerTy();
    auto *result = new CopySVFGNode(nextNode(), at,
                                    pointerLeaf ? value : nullptr);
    svfg->addNode(result);
    cached = result;
    if (pointerLeaf) {
      result->setValueId(getOrCreateValueId(value));
      valueToNode.emplace(value, result->getId());
      svfg->setValueNode(value, result->getId());
    }
    auto copy = [&](const Value *operand, const Path &indices) {
      svfg->addEdge(field(operand, indices, at), result, SVFGEdgeK::IntraCopy);
    };
    auto unknown = [&]() {
      auto *address = new AddrSVFGNode(nextNode(), at, nullptr,
                                       getOrCreateUnknownObjId());
      svfg->addNode(address);
      svfg->addEdge(address, result, SVFGEdgeK::IntraCopy);
    };
    if (const auto *insert = dyn_cast<InsertValueInst>(value)) {
      const auto indices = insert->getIndices();
      if (path.size() >= indices.size() &&
          std::equal(indices.begin(), indices.end(), path.begin()))
        copy(insert->getInsertedValueOperand(),
             Path(path.begin() + indices.size(), path.end()));
      else
        copy(insert->getAggregateOperand(), path);
    } else if (const auto *extract = dyn_cast<ExtractValueInst>(value)) {
      Path indices(extract->idx_begin(), extract->idx_end());
      indices.insert(indices.end(), path.begin(), path.end());
      copy(extract->getAggregateOperand(), indices);
    } else if (const auto *phi = dyn_cast<PHINode>(value)) {
      for (const Value *incoming : phi->incoming_values())
        copy(incoming, path);
    } else if (const auto *select = dyn_cast<SelectInst>(value)) {
      copy(select->getTrueValue(), path);
      copy(select->getFalseValue(), path);
    } else if (const auto *freeze = dyn_cast<FreezeInst>(value)) {
      copy(freeze->getOperand(0), path);
    } else if (const auto *constant = dyn_cast<Constant>(value)) {
      if (!path.empty()) {
        const Constant *element = constant;
        for (unsigned index : path) {
          element = element->getAggregateElement(index);
          if (!element)
            break;
        }
        if (element)
          copy(element, {});
        else
          unknown();
      } else if (!isa<ConstantPointerNull>(constant)) {
        const auto objects = getObjectIdsForValue(constant);
        if (objects.empty())
          unknown();
        for (uint32_t object : objects) {
          auto *address = new AddrSVFGNode(nextNode(), at, nullptr, object);
          svfg->addNode(address);
          svfg->addEdge(address, result, SVFGEdgeK::IntraCopy);
        }
      }
    } else if (const auto *call = dyn_cast<CallBase>(value)) {
      const Function *callee = call->getCalledFunction();
      if (!callee || callee->isDeclaration()) {
        unknown();
      } else {
        for (const BasicBlock &block : *callee)
          if (const auto *ret = dyn_cast<ReturnInst>(block.getTerminator()))
            if (const Value *returned = ret->getReturnValue())
              copy(returned, path);
      }
    } else if (const auto *argument = dyn_cast<Argument>(value)) {
      // Aggregate formals share field definitions across direct callers,
      // just as ordinary pointer formals share their points-to sets.
      const Function *callee = argument->getParent();
      bool hasCaller = false;
      for (const Function &caller : *callee->getParent())
        for (const BasicBlock &block : caller)
          for (const Instruction &inst : block)
            if (const auto *call = dyn_cast<CallBase>(&inst))
              if (call->getCalledFunction() == callee &&
                  argument->getArgNo() < call->arg_size()) {
                copy(call->getArgOperand(argument->getArgNo()), path);
                hasCaller = true;
              }
      if (!hasCaller || callee->hasAddressTaken())
        unknown();
    } else {
      // A missing aggregate definition is an unsupported pointer source,
      // not a proof that the extracted pointer has an empty points-to set.
      unknown();
    }
    return result;
  };
  // Resolving constant fields may add public pointer definitions.
  const auto definitions = valueToNode;
  for (const auto &entry : definitions) {
    const auto *extract = dyn_cast<ExtractValueInst>(entry.first);
    if (!extract || !extract->getType()->isPointerTy())
      continue;
    SVFGNode *destination = svfg->getNode(entry.second);
    Path path(extract->idx_begin(), extract->idx_end());
    svfg->addEdge(field(extract->getAggregateOperand(), path,
                        destination->getICFGNode()),
                  destination, SVFGEdgeK::IntraCopy);
  }
}

void SVFGBuilder::buildDirectEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      if (!isa<BinaryOpSVFGNode>(dstNode) && !isa<UnaryOpSVFGNode>(dstNode))
        continue;

      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        auto srcIt = valueToNode.find(opVal);
        if (srcIt == valueToNode.end())
          continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (!srcNode || srcNode == dstNode)
          continue;

        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraDirect);
      }
    }
  }
}

void SVFGBuilder::buildCopyEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        auto srcIt = valueToNode.find(opVal);
        if (srcIt == valueToNode.end())
          continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (!srcNode)
          continue;

        if (isa<LoadInst>(&inst) || isa<StoreInst>(&inst) ||
            isa<GetElementPtrInst>(&inst) || isa<PHINode>(&inst) ||
            isa<SelectInst>(&inst) || isa<CallBase>(&inst) ||
            isa<CmpInst>(&inst) || isa<BranchInst>(&inst) ||
            isa<BinaryOperator>(&inst)) {
          continue;
        }

        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraCopy);
      }
    }
  }
}

void SVFGBuilder::buildLoadEdges() {
  for (const auto &pair : loadToLoadNode) {
    const LoadInst *load = pair.first;
    auto dstIt = loadToLoadNode.find(load);
    SVFGNode *dstNode = svfg->getNode(dstIt->second);
    if (!dstNode)
      continue;

    const Value *ptr = load->getPointerOperand();
    auto ptrIt = valueToNode.find(ptr);
    if (ptrIt != valueToNode.end()) {
      if (SVFGNode *ptrNode = svfg->getNode(ptrIt->second)) {
        svfg->addEdge(ptrNode, dstNode, SVFGEdgeK::IntraDirect);
      }
    }
  }
}

void SVFGBuilder::buildStoreEdges() {
  for (const auto &pair : storeToStoreNode) {
    const StoreInst *store = pair.first;
    auto srcIt = storeToStoreNode.find(store);
    SVFGNode *srcNode = svfg->getNode(srcIt->second);
    if (!srcNode)
      continue;

    const Value *ptr = store->getPointerOperand();
    auto ptrIt = valueToNode.find(ptr);
    if (ptrIt != valueToNode.end()) {
      if (SVFGNode *ptrNode = svfg->getNode(ptrIt->second)) {
        svfg->addEdge(ptrNode, srcNode, SVFGEdgeK::IntraDirect);
      }
    }
    const Value *val = store->getValueOperand();
    auto valIt = valueToNode.find(val);
    if (valIt != valueToNode.end()) {
      if (SVFGNode *valNode = svfg->getNode(valIt->second)) {
        svfg->addEdge(valNode, srcNode, SVFGEdgeK::IntraDirect);
      }
    }
  }
}

void SVFGBuilder::buildGepEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      if (!isa<GetElementPtrInst>(&inst))
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      if (inst.getNumOperands() == 0)
        continue;
      const Value *ptr = inst.getOperand(0);
      auto srcIt = valueToNode.find(ptr);
      if (srcIt == valueToNode.end())
        continue;

      SVFGNode *srcNode = svfg->getNode(srcIt->second);
      if (srcNode) {
        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraGep);
      }
    }
  }
}

void SVFGBuilder::buildPhiEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

      for (const Instruction &inst : *bb) {
        if (!isa<PHINode>(&inst) && !isa<SelectInst>(&inst))
          continue;

        auto dstIt = valueToNode.find(&inst);
        if (dstIt == valueToNode.end())
          continue;

        SVFGNode *dstNode = svfg->getNode(dstIt->second);
        if (!dstNode || !dstNode->isPhiNode())
          continue;

        for (const Use &op : inst.operands()) {
          const Value *incomingVal = op.get();
          auto srcIt = valueToNode.find(incomingVal);
          if (srcIt == valueToNode.end())
            continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (srcNode) {
          svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraPhi);
        }
      }
    }
  }
}

void SVFGBuilder::buildCmpEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;
    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      if (!isa<CmpInst>(&inst))
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;
      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      for (const Use &op : inst.operands()) {
        auto srcIt = valueToNode.find(op.get());
        if (srcIt == valueToNode.end())
          continue;
        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (srcNode)
          svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraCmp);
      }
    }
  }
}

void SVFGBuilder::buildBranchEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;
    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const auto *br = dyn_cast<BranchInst>(&inst);
      if (!br || !br->isConditional())
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;
      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      const Value *cond = br->getCondition();
      auto srcIt = valueToNode.find(cond);
      if (srcIt == valueToNode.end())
        continue;
      SVFGNode *srcNode = svfg->getNode(srcIt->second);
      if (srcNode)
        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraBranch);
    }
  }
}

void SVFGBuilder::buildMemoryEdges() {
  buildLoadEdges();
  buildStoreEdges();
}
