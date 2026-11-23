#include "IR/PDG/Analysis/ValueFacts.h"
#include "IR/PDG/Analysis/FunctionFacts.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"

#include <cstdlib>
#include <functional>

using namespace llvm;

namespace pdg {

const Function *ValueFacts::callee(const CallBase &call) {
  const Value *operand = call.getCalledOperand()->stripPointerCastsAndAliases();
  if (const auto *direct = dyn_cast<Function>(operand))
    return direct;
  SmallPtrSet<const Value *, 16> active;
  std::unique_ptr<FunctionFacts> facts;
  std::function<const Function *(const Value *, unsigned)> resolve =
      [&](const Value *value, unsigned depth) -> const Function * {
    value = value->stripPointerCastsAndAliases();
    if (const auto *function = dyn_cast<Function>(value))
      return function;
    if (!depth || !active.insert(value).second)
      return nullptr;
    const Function *result = nullptr;
    if (const auto *load = dyn_cast<LoadInst>(value)) {
      if (!load->isAtomic() && !load->isVolatile()) {
        const Value *address = load->getPointerOperand()->stripPointerCasts();
        if (const auto *global = dyn_cast<GlobalVariable>(address)) {
          if (global->isConstant() && global->hasDefinitiveInitializer())
            result = resolve(global->getInitializer(), depth - 1);
        } else if (load->getFunction() == call.getFunction()) {
          if (!facts)
            facts = std::make_unique<FunctionFacts>(
                *const_cast<Function *>(call.getFunction()));
          const Value *origin = &facts->pointerOrigin(*load);
          if (origin != load)
            result = resolve(origin, depth - 1);
        }
      }
    } else {
      SmallVector<const Value *, 4> alternatives;
      if (const auto *select = dyn_cast<SelectInst>(value)) {
        if (const auto *condition =
                dyn_cast<ConstantInt>(select->getCondition()))
          alternatives.push_back(condition->isZero() ? select->getFalseValue()
                                                     : select->getTrueValue());
        else {
          alternatives.push_back(select->getTrueValue());
          alternatives.push_back(select->getFalseValue());
        }
      } else if (const auto *phi = dyn_cast<PHINode>(value))
        for (const Value *incoming : phi->incoming_values())
          alternatives.push_back(incoming);
      for (const Value *alternative : alternatives) {
        const Function *target = resolve(alternative, depth - 1);
        if (!target || (result && result != target)) {
          result = nullptr;
          break;
        }
        result = target;
      }
    }
    active.erase(value);
    return result;
  };
  return resolve(operand, 16);
}

const Value *ValueFacts::argument(const CallBase &call, unsigned index) {
  return index < call.arg_size() ? call.getArgOperand(index) : nullptr;
}

Optional<APInt> ValueFacts::integer(const Value &value) {
  if (const auto *constant = dyn_cast<ConstantInt>(&value))
    return constant->getValue();
  return None;
}

static Optional<uint64_t> arrayRegionBytes(const Value &pointer,
                                           const DataLayout &layout) {
  const Value *value = &pointer;
  while (const auto *cast = dyn_cast<BitCastOperator>(value))
    value = cast->getOperand(0);
  const auto *gep = dyn_cast<GEPOperator>(value);
  if (!gep || !gep->getNumIndices())
    return None;
  Type *type = gep->getSourceElementType();
  const auto *iterator = gep->idx_begin();
  const auto *first = dyn_cast<ConstantInt>(iterator++->get());
  if (!first)
    return None;
  if (!first->isZero()) {
    APInt offset(layout.getIndexTypeSizeInBits(pointer.getType()), 0);
    if (gep->accumulateConstantOffset(layout, offset) && !offset.isNegative() &&
        offset.getActiveBits() <= 64)
      if (auto parent = ValueFacts::objectBytes(*gep->getPointerOperand()))
        if (offset.getZExtValue() <= *parent)
          return *parent - offset.getZExtValue();
    return None;
  }
  Optional<uint64_t> region;
  if (type->isArrayTy())
    region = layout.getTypeAllocSize(type).getFixedValue();
  for (; iterator != gep->idx_end(); ++iterator) {
    const auto *index = dyn_cast<ConstantInt>(iterator->get());
    if (!index || index->isNegative() || index->getValue().getActiveBits() > 64)
      return None;
    uint64_t n = index->getZExtValue();
    if (auto *structure = dyn_cast<StructType>(type)) {
      if (n >= structure->getNumElements())
        return None;
      type = structure->getElementType(n);
      if (type->isArrayTy())
        region = layout.getTypeAllocSize(type).getFixedValue();
    } else if (auto *array = dyn_cast<ArrayType>(type)) {
      if (n > array->getNumElements())
        return None;
      uint64_t stride =
          layout.getTypeAllocSize(array->getElementType()).getFixedValue();
      region = (array->getNumElements() - n) * stride;
      type = array->getElementType();
      if (type->isArrayTy())
        region = layout.getTypeAllocSize(type).getFixedValue();
    } else
      return None;
  }
  if (region)
    return region;
  // A byte-offset GEP may have erased the array type. Recover its parent's
  // subobject extent instead of crossing into the next field of the object.
  APInt offset(layout.getIndexTypeSizeInBits(pointer.getType()), 0);
  if (gep->accumulateConstantOffset(layout, offset) && !offset.isNegative() &&
      offset.getActiveBits() <= 64)
    if (auto parent = ValueFacts::objectBytes(*gep->getPointerOperand()))
      if (offset.getZExtValue() <= *parent)
        return *parent - offset.getZExtValue();
  return None;
}

Optional<uint64_t> ValueFacts::objectBytes(const Value &pointer) {
  if (!pointer.getType()->isPointerTy())
    return None;
  const Module *module = nullptr;
  if (const auto *inst = dyn_cast<Instruction>(&pointer))
    module = inst->getModule();
  else if (const auto *argument = dyn_cast<Argument>(&pointer))
    module = argument->getParent()->getParent();
  else if (const auto *global =
               dyn_cast<GlobalValue>(getUnderlyingObject(&pointer)))
    module = global->getParent();
  if (!module)
    return None;
  uint64_t bytes = 0;
  ObjectSizeOpts opts;
  opts.NullIsUnknownSize = true;
  const DataLayout &layout = module->getDataLayout();
  auto region = arrayRegionBytes(pointer, layout);
  if (getObjectSize(&pointer, bytes, layout, nullptr, opts))
    return region ? std::min(bytes, *region) : bytes;
  return region;
}

std::vector<const Value *>
ValueFacts::localStackOrigins(const Value &pointer, const Function &function) {
  std::vector<const Value *> result;
  if (!pointer.getType()->isPointerTy())
    return result;
  SmallVector<const Value *, 4> objects;
  getUnderlyingObjects(&pointer, objects);
  if (objects.empty())
    return result;
  for (const Value *object : objects) {
    const auto *alloca = dyn_cast<AllocaInst>(object);
    const auto *call = dyn_cast<CallBase>(object);
    bool local = alloca && alloca->getFunction() == &function;
    // The same stack-returning APIs modeled by CodeQL's source predicate.
    // This classifies the call result, without following the callee body.
    if (call && call->getFunction() == &function)
      if (const Function *target = callee(*call))
        for (const char *name :
             {"alloca", "strdupa", "strndupa", "_alloca", "_malloca"})
          local |= hasLibraryName(*target, name);
    // Requiring every origin to be local implements a must-origin check.
    // Loads, unmodeled calls, null, and mixed nonlocal PHIs are not evidence.
    if (!local)
      return {};
    result.push_back(object);
  }
  return result;
}

const Value *ValueFacts::lengthSource(const Value &value) {
  const Value *current = &value;
  APInt offset(128, 0);
  for (unsigned depth = 0; depth < 64; ++depth) {
    if (const auto *call = dyn_cast<CallBase>(current)) {
      const Function *target = callee(*call);
      if (!offset.isNegative() && target && call->arg_size() == 1 &&
          (hasLibraryName(*target, "strlen", true, true) ||
           hasLibraryName(*target, "wcslen", true, true)))
        return argument(*call, 0);
      return nullptr;
    }
    const auto *op = dyn_cast<BinaryOperator>(current);
    if (!op || (op->getOpcode() != Instruction::Add &&
                op->getOpcode() != Instruction::Sub))
      return nullptr;
    unsigned constant_index = 1;
    auto constant = integer(*op->getOperand(constant_index));
    if (!constant && op->getOpcode() == Instruction::Add) {
      constant_index = 0;
      constant = integer(*op->getOperand(constant_index));
    }
    if (!constant || constant->getBitWidth() > 64)
      return nullptr;
    APInt delta = constant->sext(128);
    offset += op->getOpcode() == Instruction::Sub ? -delta : delta;
    current = op->getOperand(1 - constant_index);
  }
  return nullptr;
}

static std::string canonicalSymbolName(const Function &function) {
  StringRef symbol = function.getName();
  if (symbol.consume_front("\01")) {
    const Module *module = function.getParent();
    char prefix = module ? module->getDataLayout().getGlobalPrefix() : '\0';
    if (!prefix && module &&
        module->getDataLayoutStr().find("m:") == std::string::npos &&
        Triple(module->getTargetTriple()).isOSDarwin())
      prefix = '_';
    if (prefix && !symbol.empty() && symbol.front() == prefix)
      symbol = symbol.drop_front();
  }
  return symbol.str();
}

bool ValueFacts::hasLibraryName(const Function &function,
                                const std::string &name, bool allow_std,
                                bool allow_bsl) {
  const std::string mangled = canonicalSymbolName(function);
  if (mangled == name)
    return true;
  ItaniumPartialDemangler demangler;
  if (demangler.partialDemangle(mangled.c_str()) || !demangler.isFunction())
    return false;
  char *base = demangler.getFunctionBaseName(nullptr, nullptr);
  char *context = demangler.getFunctionDeclContextName(nullptr, nullptr);
  bool match = base && context && name == base &&
               (StringRef(context).empty() ||
                (allow_std && StringRef(context) == "std") ||
                (allow_bsl && StringRef(context) == "bsl"));
  std::free(base);
  std::free(context);
  return match;
}

std::string ValueFacts::functionBaseName(const Function &function) {
  ItaniumPartialDemangler demangler;
  const std::string mangled = canonicalSymbolName(function);
  if (demangler.partialDemangle(mangled.c_str()) || !demangler.isFunction())
    return mangled;
  char *base = demangler.getFunctionBaseName(nullptr, nullptr);
  std::string name = base ? base : function.getName().str();
  std::free(base);
  return name;
}

Optional<std::string> ValueFacts::constantString(const Value &value) {
  if (!value.getType()->isPointerTy())
    return None;
  const auto *origin = dyn_cast<GlobalVariable>(getUnderlyingObject(&value));
  if (!origin || !origin->isConstant())
    return None;
  StringRef text;
  if (getConstantStringInfo(&value, text))
    return text.str();
  // Wide literals use integer arrays, with ASCII format syntax unchanged.
  const auto *global = dyn_cast<GlobalVariable>(getUnderlyingObject(&value));
  if (!global || !global->isConstant() || !global->hasInitializer())
    return None;
  const auto *data = dyn_cast<ConstantDataSequential>(global->getInitializer());
  if (!data || !data->getElementType()->isIntegerTy())
    return None;
  unsigned offset = 0;
  if (const auto *gep = dyn_cast<GEPOperator>(&value)) {
    if (gep->getNumIndices() != 2)
      return None;
    const auto *index = gep->idx_begin();
    const auto *first = dyn_cast<ConstantInt>(index++->get());
    const auto *second = dyn_cast<ConstantInt>(index->get());
    if (!first || !first->isZero() || !second ||
        second->getValue().getActiveBits() > 32)
      return None;
    offset = second->getZExtValue();
  } else if (&value != global && value.stripPointerCasts() != global)
    return None;
  std::string result;
  for (unsigned i = offset; i < data->getNumElements(); ++i) {
    uint64_t c = data->getElementAsInteger(i);
    if (!c)
      return result;
    if (c > 127)
      return None;
    result += static_cast<char>(c);
  }
  return None;
}

Optional<uint64_t> ValueFacts::macroInteger(const Module &module,
                                            StringRef name) {
  const NamedMDNode *units = module.getNamedMetadata("llvm.dbg.cu");
  if (!units)
    return None;
  SmallVector<const Metadata *, 16> pending;
  for (const MDNode *node : units->operands())
    if (const auto *unit = dyn_cast<DICompileUnit>(node))
      for (const auto *macro : unit->getMacros())
        pending.push_back(macro);
  Optional<uint64_t> result;
  while (!pending.empty()) {
    const Metadata *node = pending.pop_back_val();
    if (const auto *file = dyn_cast<DIMacroFile>(node)) {
      for (const auto *macro : file->getElements())
        pending.push_back(macro);
    } else if (const auto *macro = dyn_cast<DIMacro>(node)) {
      if (macro->getName() != name)
        continue;
      StringRef value = macro->getValue().trim();
      while (value.size() > 1 && value.front() == '(' && value.back() == ')')
        value = value.drop_front().drop_back().trim();
      while (!value.empty() && StringRef("uUlL").contains(value.back()))
        value = value.drop_back();
      uint64_t integer = 0;
      if (value.empty() || value.getAsInteger(0, integer) ||
          (result && *result != integer))
        return None;
      result = integer;
    }
  }
  return result;
}

bool ValueFacts::macroDefined(const Module &module, StringRef name) {
  const NamedMDNode *units = module.getNamedMetadata("llvm.dbg.cu");
  if (!units)
    return false;
  SmallVector<const Metadata *, 16> pending;
  for (const MDNode *node : units->operands())
    if (const auto *unit = dyn_cast<DICompileUnit>(node))
      for (const auto *macro : unit->getMacros())
        pending.push_back(macro);
  while (!pending.empty()) {
    const Metadata *node = pending.pop_back_val();
    if (const auto *file = dyn_cast<DIMacroFile>(node)) {
      for (const auto *macro : file->getElements())
        pending.push_back(macro);
    } else if (const auto *macro = dyn_cast<DIMacro>(node))
      if (macro->getName() == name)
        return true;
  }
  return false;
}

} // namespace pdg
