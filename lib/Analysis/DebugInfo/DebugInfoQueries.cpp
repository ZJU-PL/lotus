#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Utils/LLVM/PackedTypeLayout.h"

#include <functional>
#include <unordered_set>

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

std::string
DebugInfoAnalysis::getDeclaredFunctionName(const llvm::Function *F) {
  if (!F)
    return {};
  if (auto *subprogram = F->getSubprogram())
    if (!subprogram->getName().empty())
      return subprogram->getName().str();
  return llvm::demangle(F->getName().str());
}

uint64_t DebugInfoAnalysis::getBinaryAddress(const llvm::Value *V) {
  auto *instruction = llvm::dyn_cast_or_null<llvm::Instruction>(V);
  auto *metadata = instruction ? instruction->getMetadata("asm") : nullptr;
  if (!metadata || metadata->getNumOperands() == 0)
    return 0;
  auto *constant =
      llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(metadata->getOperand(0));
  auto *integer = constant
                      ? llvm::dyn_cast<llvm::ConstantInt>(constant->getValue())
                      : nullptr;
  return integer ? integer->getValue().getLimitedValue() : 0;
}

int DebugInfoAnalysis::getPhiOperandSourceLine(const llvm::PHINode *Phi,
                                               const llvm::Value *Operand) {
  if (!Phi)
    return 0;
  auto *locations = Phi->getMetadata("phi.dbg");
  for (unsigned index = 0; index < Phi->getNumIncomingValues(); ++index) {
    if (Phi->getIncomingValue(index) != Operand)
      continue;
    if (locations && index < locations->getNumOperands())
      if (auto *location = llvm::dyn_cast_or_null<llvm::DILocation>(
              locations->getOperand(index)))
        return static_cast<int>(location->getLine());
    auto findLocation = [&](const llvm::BasicBlock &block) -> int {
      for (const auto &instruction : block)
        if (auto *debug = llvm::dyn_cast<llvm::DbgValueInst>(&instruction))
          if (debug->getValue() == Operand && debug->getDebugLoc())
            return static_cast<int>(debug->getDebugLoc().getLine());
      return 0;
    };
    if (int line = findLocation(*Phi->getIncomingBlock(index)))
      return line;
    if (int line = findLocation(Phi->getFunction()->getEntryBlock()))
      return line;
  }
  return 0;
}

std::string DebugInfoAnalysis::getIRString(const llvm::Value *V) {
  if (!V)
    return {};
  std::string text;
  llvm::raw_string_ostream output(text);
  V->print(output);
  return output.str();
}

bool DebugInfoAnalysis::hasVariableDebugName(const llvm::Value *V) {
  return V && findVarInfoMDNode(V) != nullptr;
}

void DebugInfoAnalysis::collectTypeMetadata(const llvm::Function *F) {
  if (!F)
    return;
  pointerWidth = F->getParent()->getDataLayout().getPointerSizeInBits();
  std::unordered_set<const llvm::DIType *> visited;
  std::function<void(const llvm::DIType *)> collect =
      [&](const llvm::DIType *type) {
        if (!type || !visited.insert(type).second)
          return;
        if (auto *derived = llvm::dyn_cast<llvm::DIDerivedType>(type))
          collect(derived->getBaseType());
        if (auto *composite = llvm::dyn_cast<llvm::DICompositeType>(type)) {
          std::vector<std::string> names;
          for (auto *element : composite->getElements())
            if (auto *field = llvm::dyn_cast<llvm::DIType>(element)) {
              names.push_back(field->getName().str());
              collect(field);
            }
          if (!composite->getName().empty())
            compositeFieldNames[composite->getName().str()] = std::move(names);
        }
      };
  for (const auto &block : *F)
    for (const auto &instruction : block)
      if (auto *debug =
              llvm::dyn_cast<llvm::DbgVariableIntrinsic>(&instruction))
        if (auto *variable = debug->getVariable())
          collect(variable->getType());
}

void DebugInfoAnalysis::registerFieldName(llvm::Type *type, int64_t offset,
                                          const std::string &name) {
  fieldNameCache[{type, offset}] = name;
}

std::string DebugInfoAnalysis::getFieldName(llvm::Type *type, int64_t offset,
                                            int recursiveDepth) {
  auto found = fieldNameCache.find({type, offset});
  if (found != fieldNameCache.end())
    return found->second;
  auto *structure = llvm::dyn_cast<llvm::StructType>(type);
  if (!structure)
    return "!" + std::to_string(offset / 8);
  if (structure->isOpaque())
    return "!(opq)" + std::to_string(offset);
  if (structure->getNumElements() == 0)
    return "!(ob)" + std::to_string(offset);
  lotus::PackedTypeLayout layout(
      llvm::DataLayout("p:" + std::to_string(pointerWidth) + ":" +
                       std::to_string(pointerWidth)));
  unsigned index =
      offset >= static_cast<int64_t>(layout.getTypeSizeInBits(structure))
          ? structure->getNumElements() - 1
          : layout.getElementContainingOffset(structure, offset);
  std::string name;
  if (structure->hasName()) {
    llvm::StringRef typeName = structure->getName();
    if (!typeName.consume_front("struct."))
      typeName.consume_front("class.");
    auto names = compositeFieldNames.find(typeName.str());
    if (names != compositeFieldNames.end() && index < names->second.size())
      name = names->second[index];
  }
  if (name.empty())
    name = "!" + std::to_string(offset / 8);
  offset -= static_cast<int64_t>(layout.getElementOffsetInBits(structure,
                                                               index));
  if ((offset > 0 || recursiveDepth != 0) &&
      structure->getElementType(index)->isStructTy())
    name += "." + getFieldName(structure->getElementType(index), offset,
                               recursiveDepth == -1 ? -1 : recursiveDepth - 1);
  return name;
}
