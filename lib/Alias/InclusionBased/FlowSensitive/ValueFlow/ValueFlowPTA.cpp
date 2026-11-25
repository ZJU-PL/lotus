#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowPTA.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

namespace lotus::alias {
using namespace llvm;
using vfg::AccessID;
using vfg::BlockID;
using vfg::CallSiteID;
using vfg::FunctionID;
using vfg::NodeID;

namespace {

const Function *directCallee(const CallBase &call) {
  const Value *value = call.getCalledOperand()->stripPointerCasts();
  std::set<const Value *> seen;
  while (const auto *alias = dyn_cast<GlobalAlias>(value)) {
    if (!seen.insert(value).second)
      return nullptr;
    value = alias->getAliasee()->stripPointerCasts();
  }
  return dyn_cast<Function>(value);
}

bool isAnnotationFunction(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "__aser_alias__" || name == "__aser_no_alias__" ||
         name == "MAYALIAS" || name == "NOALIAS" || name == "MUSTALIAS" ||
         name == "PARTIALALIAS" || name == "EXPECTEDFAIL_MAYALIAS" ||
         name == "EXPECTEDFAIL_NOALIAS" || name == "svf_assert" ||
         name == "__VERIFIER_assert";
}

std::optional<AllocatorInfo> builtinAllocatorInfo(const Function *function) {
  if (!function || !function->isDeclaration())
    return std::nullopt;
  const StringRef name = function->getName();
  if (name == "malloc" || name == "aligned_alloc" || name == "valloc" ||
      name == "pvalloc" || name == "memalign" || name == "strdup" ||
      name == "strndup" || name == "fopen" || name == "fopen64" ||
      name == "fdopen" || name == "tmpfile" || name == "tmpfile64" ||
      name == "opendir" || name == "popen" || name == "mmap" ||
      name == "mmap64" || name == "_Znwm" || name == "_Znam" ||
      name == "_Znwj" || name == "_Znaj" || name == "_ZnwmRKSt9nothrow_t" ||
      name == "_ZnamRKSt9nothrow_t" || name == "_ZnwjRKSt9nothrow_t" ||
      name == "_ZnajRKSt9nothrow_t" || name == "_ZnwmSt11align_val_t" ||
      name == "_ZnamSt11align_val_t" || name == "_ZnwjSt11align_val_t" ||
      name == "_ZnajSt11align_val_t" ||
      name == "_ZnwmSt11align_val_tRKSt9nothrow_t" ||
      name == "_ZnamSt11align_val_tRKSt9nothrow_t" ||
      name == "_ZnwjSt11align_val_tRKSt9nothrow_t" ||
      name == "_ZnajSt11align_val_tRKSt9nothrow_t") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = true;
    info.initializesToZero = false;
    return info;
  }
  if (name == "calloc") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = true;
    info.initializesToZero = true;
    return info;
  }
  if (name == "realloc" || name == "reallocf") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = true;
    info.initializesToZero = false;
    return info;
  }
  if (name == "posix_memalign") {
    AllocatorInfo info;
    info.functionName = name.str();
    info.returnsPointer = false;
    info.ptrOutArgIndex = 0;
    info.initializesToZero = false;
    return info;
  }
  return std::nullopt;
}

bool isReallocatorName(StringRef name) {
  return name == "realloc" || name == "reallocf";
}

bool builtinDeallocator(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "free" || name == "cfree" || name == "_ZdlPv" ||
         name == "_ZdaPv" || name == "_ZdlPvm" || name == "_ZdaPvm" ||
         name == "_ZdlPvj" || name == "_ZdaPvj" ||
         name == "_ZdlPvRKSt9nothrow_t" || name == "_ZdaPvRKSt9nothrow_t";
}

bool builtinExitFunction(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "exit" || name == "_exit" || name == "_Exit" ||
         name == "abort" || name == "__assert_fail" || name == "__assert_rtn" ||
         name == "__cxa_pure_virtual";
}

bool builtinNoPointerEffect(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "printf" || name == "fprintf" || name == "vprintf" ||
         name == "vfprintf" || name == "puts" || name == "fputs" ||
         name == "putchar" || name == "putc" || name == "fputc" ||
         name == "perror" || name == "fflush" || name == "fclose" ||
         name == "close" || name == "strlen" || name == "strnlen" ||
         name == "strcmp" || name == "strncmp" || name == "memcmp" ||
         name == "strcasecmp" || name == "strncasecmp" || name == "atoi" ||
         name == "atol" || name == "atoll" || name == "atof" ||
         name == "rand" || name == "srand" || name == "random" ||
         name == "srandom" || name == "abs" || name == "labs" ||
         name == "llabs" || name == "fabs" || name == "fabsf" ||
         name == "sqrt" || name == "sqrtf" || name == "sin" || name == "cos" ||
         name == "tan" || name == "exp" || name == "log" || name == "log10" ||
         name == "pow" || name == "floor" || name == "ceil" ||
         name == "clock" || name == "time" || name == "difftime" ||
         name == "sleep" || name == "usleep" || name == "getpid" ||
         name == "getuid" || name == "isatty";
}

bool scalarType(const Type *type) {
  return type->isSingleValueType() && !type->isVectorTy();
}

bool typeContainsPointer(const Type *type, std::set<const Type *> &seen) {
  if (!type)
    return false;
  if (type->isPointerTy())
    return true;
  if (!seen.insert(type).second)
    return false;
  if (const auto *arr = dyn_cast<ArrayType>(type))
    return typeContainsPointer(arr->getElementType(), seen);
  if (const auto *vec = dyn_cast<VectorType>(type))
    return typeContainsPointer(vec->getElementType(), seen);
  if (const auto *st = dyn_cast<StructType>(type)) {
    for (const Type *elem : st->elements())
      if (typeContainsPointer(elem, seen))
        return true;
  }
  return false;
}

bool typeContainsPointer(const Type *type) {
  std::set<const Type *> seen;
  return typeContainsPointer(type, seen);
}

bool containsPtrToInt(const Value *value, unsigned depth = 0) {
  if (!value || depth > 6)
    return false;
  if (isa<PtrToIntInst>(value))
    return true;
  if (const auto *ce = dyn_cast<ConstantExpr>(value)) {
    if (ce->getOpcode() == Instruction::PtrToInt)
      return true;
    for (const Use &op : ce->operands())
      if (containsPtrToInt(op.get(), depth + 1))
        return true;
    return false;
  }
  if (const auto *inst = dyn_cast<Instruction>(value)) {
    if (isa<CastInst>(inst) || isa<FreezeInst>(inst))
      return containsPtrToInt(inst->getOperand(0), depth + 1);
    if (const auto *bin = dyn_cast<BinaryOperator>(inst)) {
      return containsPtrToInt(bin->getOperand(0), depth + 1) ||
             containsPtrToInt(bin->getOperand(1), depth + 1);
    }
    if (const auto *phi = dyn_cast<PHINode>(inst)) {
      for (const Use &inc : phi->incoming_values())
        if (containsPtrToInt(inc.get(), depth + 1))
          return true;
    }
    if (const auto *sel = dyn_cast<SelectInst>(inst)) {
      return containsPtrToInt(sel->getTrueValue(), depth + 1) ||
             containsPtrToInt(sel->getFalseValue(), depth + 1);
    }
  }
  return false;
}

// An allocation site inside a CFG cycle may represent multiple live objects.
bool blockInCycle(const BasicBlock &origin) {
  std::set<const BasicBlock *> seen;
  std::vector<const BasicBlock *> work;
  for (const BasicBlock *successor : successors(&origin))
    work.push_back(successor);
  while (!work.empty()) {
    const BasicBlock *block = work.back();
    work.pop_back();
    if (block == &origin)
      return true;
    if (!seen.insert(block).second)
      continue;
    for (const BasicBlock *successor : successors(block))
      work.push_back(successor);
  }
  return false;
}

struct LifecycleEntry {
  int32_t priority = 65535;
  const Function *function = nullptr;
};

std::vector<LifecycleEntry> parseLifecycleList(const Module &module,
                                               const char *globalName,
                                               bool ascending) {
  std::vector<LifecycleEntry> entries;
  const GlobalVariable *list = module.getNamedGlobal(globalName);
  if (!list || !list->hasInitializer())
    return entries;
  const auto *arr = dyn_cast<ConstantArray>(list->getInitializer());
  if (!arr)
    return entries;
  for (const Use &op : arr->operands()) {
    const auto *st = dyn_cast<ConstantStruct>(op.get());
    if (!st || st->getNumOperands() < 2)
      continue;
    const auto *prio = dyn_cast<ConstantInt>(st->getOperand(0));
    const Value *fnVal = st->getOperand(1)->stripPointerCasts();
    const auto *fn = dyn_cast<Function>(fnVal);
    if (!fn || fn->isDeclaration())
      continue;
    entries.push_back(
        {prio ? static_cast<int32_t>(prio->getSExtValue()) : 65535, fn});
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [ascending](const LifecycleEntry &a,
                               const LifecycleEntry &b) {
                     return ascending ? (a.priority < b.priority)
                                      : (a.priority > b.priority);
                   });
  return entries;
}

} // namespace

class ValueFlowPTA::Impl {
public:
  Impl(const Module &module, Config config)
      : module(module), config(std::move(config)) {}

  const Module &module;
  Config config;
  Statistics stats;
  bool analyzed = false;
  const AliasSpecManager *specManager = nullptr;
  std::unique_ptr<vfg::Program> program;
  std::unique_ptr<vfg::Solver> solver;
  NodeID emptyValueNode = 0;

  std::unordered_map<const Value *, NodeID> values;
  std::unordered_map<const StoreInst *, NodeID> storeNodes;
  std::vector<const Value *> nodeValues;
  std::unordered_map<const Value *, ObjectID> objects;
  std::vector<const Value *> objectValues;
  std::vector<PointedToBySet> inverse;
  std::unordered_map<const BasicBlock *, BlockID> blockIds;
  std::unordered_map<const AtomicCmpXchgInst *, NodeID> atomicResults;
  std::unordered_map<const Function *, ObjectID> staticReturnObjects;
  std::unordered_map<const CallBase *, ObjectID> callAllocObjects;

  struct FunctionInfo {
    FunctionID id = 0;
    BlockID entry = 0;
    BlockID exit = 0;
    NodeID returned = 0;
    std::vector<NodeID> formalParams;
    std::vector<std::pair<unsigned, ObjectID>> byValParams;
  };
  std::map<const Function *, FunctionInfo> functions;
  std::unordered_map<FunctionID, const Function *> functionById;

  struct ExternalCallRecord {
    const CallBase *instruction = nullptr;
    BlockID block = 0;
    bool conservative = false;
  };
  std::vector<ExternalCallRecord> externalCalls;

  struct IndirectCallRecord {
    const CallBase *instruction = nullptr;
    BlockID block = 0;
    CallSiteID site = 0;
    NodeID calledPointer = 0;
    std::set<const Function *> modeledExternalTargets;
    std::set<const Function *> modeledByValTargets;
    bool modeledUnknownExternal = false;
  };
  std::vector<IndirectCallRecord> indirectCalls;

  std::vector<const Function *> entryPoints;

  std::optional<AllocatorInfo> getAllocatorInfo(const Function *fn) const {
    if (!fn || !fn->isDeclaration())
      return std::nullopt;
    if (specManager) {
      if (auto info = specManager->getAllocatorInfo(fn))
        return info;
    }
    return builtinAllocatorInfo(fn);
  }

  bool isDeallocator(const Function *fn) const {
    if (!fn || !fn->isDeclaration())
      return false;
    if (specManager && specManager->isDeallocator(fn))
      return true;
    return builtinDeallocator(fn);
  }

  bool isExitFunction(const Function *fn) const {
    if (!fn || !fn->isDeclaration())
      return false;
    if (specManager && specManager->isExitFunction(fn))
      return true;
    return builtinExitFunction(fn);
  }

  bool isNoEffectFunction(const Function *fn) const {
    if (!fn || !fn->isDeclaration())
      return false;
    if (isAnnotationFunction(fn))
      return true;
    if (specManager && specManager->isNoEffect(fn))
      return true;
    return builtinNoPointerEffect(fn);
  }

  bool mayReturn(const CallBase &call, const Function *callee = nullptr) const {
    if (callee && isExitFunction(callee) && !isa<InvokeInst>(call))
      return false;
    return !call.doesNotReturn() || isa<InvokeInst>(call);
  }

  void recordNodeValue(NodeID id, const Value *value) {
    if (id >= nodeValues.size())
      nodeValues.resize(id + 1, nullptr);
    if (!nodeValues[id])
      nodeValues[id] = value;
  }

  ObjectID makeObject(const Value *value, bool singleton, bool memory) {
    const ObjectID object = program->addObject(
        singleton, memory,
        value && value->hasName() ? value->getName().str() : "");
    if (value) {
      objects[value] = object;
      recordNodeValue(program->address(object), value);
    }
    objectValues.resize(program->objectCount() + 1, nullptr);
    objectValues[object] = value;
    return object;
  }

  NodeID node(const Value *value) {
    if (!value || !value->getType()->isPointerTy())
      return program->unknownValue();
    auto found = values.find(value);
    if (found != values.end())
      return found->second;
    if (isa<ConstantPointerNull>(value)) {
      values[value] = program->nullValue();
      return program->nullValue();
    }
    const NodeID result = program->addNode();
    values[value] = result; // Install before following recursive constants.
    recordNodeValue(result, value);
    if (const auto *alias = dyn_cast<GlobalAlias>(value)) {
      program->addCopy(node(alias->getAliasee()), result);
    } else if (const auto *expression = dyn_cast<ConstantExpr>(value)) {
      if ((expression->isCast() ||
           expression->getOpcode() == Instruction::GetElementPtr) &&
          expression->getOperand(0)->getType()->isPointerTy()) {
        program->addCopy(node(expression->getOperand(0)), result);
      } else if (expression->getOpcode() == Instruction::Select &&
                 expression->getNumOperands() >= 3) {
        program->addCopy(node(expression->getOperand(1)), result);
        program->addCopy(node(expression->getOperand(2)), result);
      } else {
        program->addCopy(program->unknownValue(), result);
      }
    } else if (isa<UndefValue>(value) || isa<PoisonValue>(value)) {
      // Undef/poison pointer constants carry no concrete address.
    } else if (isa<Constant>(value) ||
               (!isa<Instruction>(value) && !isa<Argument>(value))) {
      program->addCopy(program->unknownValue(), result);
    }
    return result;
  }

  ObjectID getOrCreateStaticReturnObject(const Function *callee) {
    auto it = staticReturnObjects.find(callee);
    if (it != staticReturnObjects.end())
      return it->second;
    const ObjectID obj = program->addObject(
        /*singleton=*/false, /*memory=*/true,
        "<static:" + callee->getName().str() + ">");
    objectValues.resize(program->objectCount() + 1, nullptr);
    objectValues[obj] = nullptr;
    staticReturnObjects[callee] = obj;
    return obj;
  }

  ObjectID getOrCreateCallAllocObject(const CallBase &call,
                                      const AllocatorInfo &info) {
    auto it = callAllocObjects.find(&call);
    if (it != callAllocObjects.end())
      return it->second;
    const ObjectID obj = makeObject(&call, /*singleton=*/false, /*memory=*/true);
    program->setObjectGlobal(obj, false);
    if (info.initializesToZero)
      program->setInitializer(obj, program->nullValue());
    else
      program->setEmptyInitializer(obj);
    callAllocObjects[&call] = obj;
    return obj;
  }

  void collectAggregateValuePointers(const Value *val,
                                     std::vector<NodeID> &out,
                                     bool &sawOpaque,
                                     unsigned depth = 0) {
    if (!val || depth > 6) {
      sawOpaque = true;
      return;
    }
    if (val->getType()->isPointerTy()) {
      out.push_back(node(val));
      return;
    }
    if (isa<UndefValue>(val) || isa<PoisonValue>(val))
      return;
    if (isa<ConstantAggregateZero>(val)) {
      if (typeContainsPointer(val->getType()))
        out.push_back(program->nullValue());
      return;
    }
    if (const auto *c = dyn_cast<Constant>(val)) {
      if (c->getType()->isAggregateType() || c->getType()->isVectorTy()) {
        for (const Use &op : c->operands())
          collectAggregateValuePointers(op.get(), out, sawOpaque, depth + 1);
      }
      return;
    }
    if (const auto *ins = dyn_cast<InsertValueInst>(val)) {
      collectAggregateValuePointers(ins->getAggregateOperand(), out, sawOpaque,
                                    depth + 1);
      collectAggregateValuePointers(ins->getInsertedValueOperand(), out,
                                    sawOpaque, depth + 1);
      return;
    }
    if (const auto *ins = dyn_cast<InsertElementInst>(val)) {
      collectAggregateValuePointers(ins->getOperand(0), out, sawOpaque,
                                    depth + 1);
      collectAggregateValuePointers(ins->getOperand(1), out, sawOpaque,
                                    depth + 1);
      return;
    }
    if (containsPtrToInt(val)) {
      sawOpaque = true;
      return;
    }
    if (typeContainsPointer(val->getType()))
      sawOpaque = true;
  }

  // Collect pointer-valued fields from a global constant initializer.
  // Unlike the legacy adapter, non-pointer scalar fields (e.g., i32 0 in
  // `{ i32, i8* }`) do NOT inject `unknown` in pointer-only mode.
  void collectInitializer(const Constant *constant, NodeID destination,
                          bool &addedAny) {
    if (!constant)
      return;
    if (constant->getType()->isPointerTy()) {
      if (isa<UndefValue>(constant) || isa<PoisonValue>(constant))
        return;
      program->addCopy(node(constant), destination);
      addedAny = true;
    } else if (isa<ConstantAggregateZero>(constant)) {
      if (typeContainsPointer(constant->getType())) {
        program->addCopy(program->nullValue(), destination);
        addedAny = true;
      }
    } else if (isa<UndefValue>(constant) || isa<PoisonValue>(constant)) {
      if (!config.pointerOnlyMemory) {
        program->addCopy(program->unknownValue(), destination);
        addedAny = true;
      }
    } else if (constant->getType()->isAggregateType() ||
               constant->getType()->isVectorTy()) {
      for (const Use &operand : constant->operands())
        collectInitializer(dyn_cast<Constant>(operand.get()), destination,
                           addedAny);
      if (!config.pointerOnlyMemory && constant->getNumOperands() == 0) {
        program->addCopy(program->unknownValue(), destination);
        addedAny = true;
      }
    } else if (const auto *ce = dyn_cast<ConstantExpr>(constant)) {
      if (containsPtrToInt(ce)) {
        program->addCopy(program->unknownValue(), destination);
        addedAny = true;
      } else if (!config.pointerOnlyMemory) {
        program->addCopy(program->unknownValue(), destination);
        addedAny = true;
      }
    } else if (!config.pointerOnlyMemory) {
      program->addCopy(program->unknownValue(), destination);
      addedAny = true;
    }
  }

  void collectObjectsAndFunctions() {
    emptyValueNode = program->addNode();

    for (const GlobalVariable &global : module.globals()) {
      const auto object =
          makeObject(&global, scalarType(global.getValueType()), true);
      values[&global] = program->address(object);
    }

    for (const Function &function : module) {
      const auto object = makeObject(&function, false, false);
      values[&function] = program->address(object);
      if (function.isDeclaration())
        continue;

      FunctionInfo info;
      info.id = program->addFunction(function.getName().str());
      info.entry = program->addBlock(info.id);
      info.exit = program->addBlock(info.id);
      program->setFunctionEntry(info.id, info.entry);
      program->setFunctionExit(info.id, info.exit);
      program->setFunctionObject(object, info.id);
      program->setAddressTakenFunction(info.id, function.hasAddressTaken());
      if (function.getReturnType()->isPointerTy()) {
        info.returned = program->addNode();
        program->setFunctionReturn(info.id, info.returned);
      }

      unsigned argIdx = 0;
      for (const Argument &argument : function.args()) {
        if (argument.getType()->isPointerTy()) {
          if (argument.hasByValAttr()) {
            const auto argObj = makeObject(&argument, false, true);
            program->setEmptyInitializer(argObj);
            program->setObjectOwner(argObj, info.id);
            values[&argument] = program->address(argObj);
            info.formalParams.push_back(0);
            info.byValParams.emplace_back(argIdx, argObj);
          } else {
            const NodeID paramNode = node(&argument);
            info.formalParams.push_back(paramNode);
          }
        } else {
          info.formalParams.push_back(0);
        }
        ++argIdx;
      }
      program->setFunctionParams(info.id, info.formalParams);

      for (const BasicBlock &block : function)
        blockIds[&block] = program->addBlock(info.id);
      program->addControlEdge(info.entry,
                              blockIds.at(&function.getEntryBlock()));

      functionById[info.id] = &function;
      functions[&function] = std::move(info);
    }

    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      const FunctionID fnId = functions.at(&function).id;
      for (const BasicBlock &block : function) {
        bool cycleChecked = false;
        bool cyclic = false;
        for (const Instruction &instruction : block) {
          if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
            if (!cycleChecked) {
              cyclic = blockInCycle(block);
              cycleChecked = true;
            }
            const auto *count = dyn_cast<ConstantInt>(alloca->getArraySize());
            const bool unique = scalarType(alloca->getAllocatedType()) &&
                                count && count->isOne() && !cyclic;
            const auto object = makeObject(alloca, unique, true);
            program->setEmptyInitializer(object);
            program->setObjectOwner(object, fnId);
            values[alloca] = program->address(object);
          } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
            const Function *callee = directCallee(*call);
            if (auto allocInfo = getAllocatorInfo(callee)) {
              const ObjectID object = getOrCreateCallAllocObject(*call, *allocInfo);
              if (allocInfo->returnsPointer && call->getType()->isPointerTy()) {
                const NodeID result = node(call);
                program->addCopy(program->address(object), result);
                program->addCopy(program->nullValue(), result);
              }
            }
          }
          if (instruction.getType()->isPointerTy())
            node(&instruction);
          if (const auto *cmp = dyn_cast<AtomicCmpXchgInst>(&instruction))
            atomicResults[cmp] = program->addNode();
        }
      }
    }

    for (const GlobalAlias &alias : module.aliases())
      node(&alias);
  }

  void setupEntryPointsAndLifecycle() {
    std::vector<LifecycleEntry> ctors;
    std::vector<LifecycleEntry> dtors;
    for (const char *name : {"llvm.global_ctors", "llvm.global_dtors"}) {
      if (const GlobalVariable *list = module.getNamedGlobal(name)) {
        const auto *type = dyn_cast<ArrayType>(list->getValueType());
        if (!type || type->getNumElements() != 0) {
          if (!config.lowerGlobalLifecycle) {
            throw std::invalid_argument(
                "ValueFlowPTA: lower global constructors/destructors to "
                "explicit entry-point calls and remove their lifecycle lists "
                "first");
          }
        }
      }
    }
    if (config.lowerGlobalLifecycle) {
      ctors = parseLifecycleList(module, "llvm.global_ctors",
                                 /*ascending=*/true);
      dtors = parseLifecycleList(module, "llvm.global_dtors",
                                 /*ascending=*/false);
    }
    stats.globalCtors = ctors.size();
    stats.globalDtors = dtors.size();

    entryPoints = config.entryPoints;
    if (entryPoints.empty()) {
      const Function *main = module.getFunction("main");
      if (main && !main->isDeclaration()) {
        entryPoints.push_back(main);
      } else {
        for (const Function &function : module)
          if (!function.isDeclaration() && !function.hasLocalLinkage())
            entryPoints.push_back(&function);
        if (entryPoints.empty())
          for (const Function &function : module)
            if (!function.isDeclaration())
              entryPoints.push_back(&function);
      }
    }
    for (const Function *function : entryPoints) {
      if (!function || function->getParent() != &module ||
          function->isDeclaration()) {
        throw std::invalid_argument(
            "ValueFlowPTA: entry point is not a module definition");
      }
    }

    // Seed entry-point arguments (external inputs) with unknown.
    for (const Function *function : entryPoints) {
      const auto &info = functions.at(function);
      for (const Argument &argument : function->args()) {
        if (!argument.getType()->isPointerTy())
          continue;
        if (!argument.hasByValAttr())
          program->addCopy(program->unknownValue(), node(&argument));
      }
      for (const auto &[argIdx, byValObj] : info.byValParams) {
        (void)argIdx;
        program->addStore(info.entry, program->address(byValObj),
                          program->unknownValue(), /*allowStrong=*/false);
      }
    }

    if (ctors.empty() && dtors.empty() && entryPoints.size() <= 1) {
      for (const Function *function : entryPoints)
        program->addEntryFunction(functions.at(function).id);
      return;
    }

    // Synthesize a startup/teardown driver. For single-entry modules (e.g.,
    // main), run straight-line: ctors -> main -> dtors. For multi-entry
    // library modules, wrap entry points in a non-deterministic loop so global
    // state written by any entry point flows soundly to every other entry point
    // in any call order while preserving intra-function flow sensitivity.
    const FunctionID startup = program->addFunction("<__vfg_startup>");
    const BlockID ctorBlock = program->addBlock(startup);
    program->setFunctionEntry(startup, ctorBlock);
    for (const auto &entry : ctors) {
      auto it = functions.find(entry.function);
      if (it == functions.end())
        continue;
      const CallSiteID cs = program->addCallSite(ctorBlock);
      program->addCallTarget(cs, it->second.id);
    }

    if (entryPoints.size() <= 1) {
      for (const Function *entry : entryPoints) {
        const CallSiteID cs = program->addCallSite(ctorBlock);
        program->addCallTarget(cs, functions.at(entry).id);
      }
      for (const auto &entry : dtors) {
        auto it = functions.find(entry.function);
        if (it == functions.end())
          continue;
        const CallSiteID cs = program->addCallSite(ctorBlock);
        program->addCallTarget(cs, it->second.id);
      }
      program->setFunctionExit(startup, ctorBlock);
    } else {
      const BlockID loopHeader = program->addBlock(startup);
      const BlockID dtorBlock = program->addBlock(startup);
      program->addControlEdge(ctorBlock, loopHeader);
      for (const Function *entry : entryPoints) {
        const BlockID callBlock = program->addBlock(startup);
        const CallSiteID cs = program->addCallSite(callBlock);
        program->addCallTarget(cs, functions.at(entry).id);
        program->addControlEdge(loopHeader, callBlock);
        program->addControlEdge(callBlock, loopHeader);
      }
      program->addControlEdge(loopHeader, dtorBlock);
      for (const auto &entry : dtors) {
        auto it = functions.find(entry.function);
        if (it == functions.end())
          continue;
        const CallSiteID cs = program->addCallSite(dtorBlock);
        program->addCallTarget(cs, it->second.id);
      }
      program->setFunctionExit(startup, dtorBlock);
    }
    program->addEntryFunction(startup);
  }

  void emitByValCopies(BlockID block, const CallBase &call,
                       const Function *callee, CallSiteID anchorSite = 0) {
    const auto &info = functions.at(callee);
    for (const auto &[argIdx, byValObj] : info.byValParams) {
      if (argIdx >= call.arg_size() ||
          !call.getArgOperand(argIdx)->getType()->isPointerTy()) {
        if (anchorSite) {
          program->addStoreBeforeCall(anchorSite, program->address(byValObj),
                                      program->unknownValue(),
                                      /*allowStrong=*/false);
        } else {
          program->addStore(block, program->address(byValObj),
                            program->unknownValue(), /*allowStrong=*/false);
        }
        continue;
      }
      const Value *actual = call.getArgOperand(argIdx);
      const NodeID tmp = program->addNode();
      if (anchorSite) {
        program->addLoadBeforeCall(anchorSite, node(actual), tmp);
        program->addStoreBeforeCall(anchorSite, program->address(byValObj), tmp,
                                    /*allowStrong=*/false);
      } else {
        program->addLoad(block, node(actual), tmp);
        program->addStore(block, program->address(byValObj), tmp,
                          /*allowStrong=*/false);
      }
    }
  }

  bool handleIntrinsic(const CallBase &call, BlockID block) {
    const auto *instruction = dyn_cast<IntrinsicInst>(&call);
    if (!instruction)
      return false;
    if (isa<DbgInfoIntrinsic>(instruction))
      return true;
    switch (instruction->getIntrinsicID()) {
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
    case Intrinsic::assume:
    case Intrinsic::invariant_start:
    case Intrinsic::invariant_end:
    case Intrinsic::vaend:
    case Intrinsic::trap:
    case Intrinsic::stackrestore:
    case Intrinsic::stacksave:
      if (call.getType()->isPointerTy())
        program->addCopy(program->unknownValue(), node(&call));
      return true;
    default:
      break;
    }

    const DataLayout &dl = module.getDataLayout();
    const uint64_t ptrSize = dl.getPointerSize() ? dl.getPointerSize() : 8;

    if (const auto *memSet = dyn_cast<MemSetInst>(instruction)) {
      const auto *length = dyn_cast<ConstantInt>(memSet->getLength());
      if (length && length->isZero())
        return true;
      const auto *byteVal = dyn_cast<ConstantInt>(memSet->getValue());
      const bool fullZero =
          byteVal && byteVal->isZero() &&
          (!length || length->getZExtValue() >= ptrSize);
      program->addStore(block, node(memSet->getDest()),
                        fullZero ? program->nullValue()
                                 : program->unknownValue(),
                        /*allowStrong=*/false);
      return true;
    }
    if (const auto *memTransfer = dyn_cast<MemTransferInst>(instruction)) {
      const auto *length = dyn_cast<ConstantInt>(memTransfer->getLength());
      if (length && length->isZero())
        return true;
      if (length && length->getZExtValue() < ptrSize) {
        // Partial-byte overwrite can synthesize an arbitrary pointer value.
        program->addStore(block, node(memTransfer->getDest()),
                          program->unknownValue(), /*allowStrong=*/false);
      } else {
        // Model pointer transfer *src -> *dest without injecting unknown.
        const NodeID tmp = program->addNode();
        program->addLoad(block, node(memTransfer->getSource()), tmp);
        program->addStore(block, node(memTransfer->getDest()), tmp,
                          /*allowStrong=*/false);
      }
      return true;
    }

    if (call.getType()->isPointerTy())
      program->addCopy(program->unknownValue(), node(&call));
    if (!call.doesNotAccessMemory() && !call.onlyReadsMemory())
      program->addStore(block, program->unknownValue(), program->unknownValue(),
                        /*allowStrong=*/false);
    return true;
  }

  // Model an external call (either direct or resolved indirectly).
  // Returns true if handled via a known spec (non-conservative), false if
  // treated conservatively as an opaque external call.
  bool modelExternalCall(BlockID block, const CallBase &call,
                         const Function *callee, CallSiteID existingSite = 0) {
    // Any indirect callsite reaching an external target (even readonly/no-op)
    // must be marked external so defined sibling targets cannot emit an unsound
    // strong callOutStore when the external target executes instead.
    if (existingSite)
      program->markCallExternal(existingSite, true);

    auto emitLoad = [&](NodeID ptr, NodeID res) {
      if (existingSite)
        program->addLoadAfterCall(existingSite, ptr, res);
      else
        program->addLoad(block, ptr, res);
    };
    auto emitStore = [&](NodeID ptr, NodeID val, bool allowStrong) {
      if (existingSite)
        program->addStoreAfterCall(existingSite, ptr, val, allowStrong);
      else
        program->addStore(block, ptr, val, allowStrong);
    };

    const DataLayout &dl = module.getDataLayout();
    const uint64_t ptrSize = dl.getPointerSize() ? dl.getPointerSize() : 8;

    if (callee) {
      if (isAnnotationFunction(callee) || isDeallocator(callee) ||
          isExitFunction(callee)) {
        return true;
      }

      // Explicitly validate byte-value and partial-length semantics for C
      // memory library functions (matching handleIntrinsic).
      const StringRef name = callee->getName();
      if (name == "memset" && call.arg_size() >= 3 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        const NodeID dst = node(call.getArgOperand(0));
        if (call.getType()->isPointerTy())
          program->addCopy(dst, node(&call));
        const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2));
        if (!length || !length->isZero()) {
          const auto *byteVal = dyn_cast<ConstantInt>(call.getArgOperand(1));
          const bool fullZero =
              byteVal && byteVal->isZero() &&
              (!length || length->getZExtValue() >= ptrSize);
          emitStore(dst,
                    fullZero ? program->nullValue() : program->unknownValue(),
                    /*allowStrong=*/false);
        }
        return true;
      }
      if ((name == "memcpy" || name == "memmove" || name == "bcopy") &&
          call.arg_size() >= 3) {
        const unsigned dstIdx = (name == "bcopy") ? 1u : 0u;
        const unsigned srcIdx = (name == "bcopy") ? 0u : 1u;
        if (call.getArgOperand(dstIdx)->getType()->isPointerTy() &&
            call.getArgOperand(srcIdx)->getType()->isPointerTy()) {
          const NodeID dst = node(call.getArgOperand(dstIdx));
          const NodeID src = node(call.getArgOperand(srcIdx));
          if (call.getType()->isPointerTy())
            program->addCopy(dst, node(&call));
          const auto *length = dyn_cast<ConstantInt>(call.getArgOperand(2));
          if (!length || !length->isZero()) {
            if (length && length->getZExtValue() < ptrSize) {
              emitStore(dst, program->unknownValue(), /*allowStrong=*/false);
            } else {
              const NodeID tmp = program->addNode();
              emitLoad(src, tmp);
              emitStore(dst, tmp, /*allowStrong=*/false);
            }
          }
          return true;
        }
      }

      // Explicitly model callback-registering C standard/POSIX functions
      // before AliasSpecManager (ptr.spec marks atexit/sigaction as IGNORE and
      // signal as COPY-only, which would otherwise omit handler invocation).
      auto emitHandlerCall = [&](NodeID handlerPtr,
                                 std::vector<NodeID> handlerArgs) {
        const CallSiteID cbCs =
            existingSite
                ? program->addCallSiteAfterCall(
                      existingSite, handlerPtr, 0, std::move(handlerArgs),
                      /*mayReturn=*/true, /*isInvoke=*/false)
                : program->addCallSite(block, handlerPtr, 0,
                                       std::move(handlerArgs),
                                       /*mayReturn=*/true, /*isInvoke=*/false);
        program->markCallExternal(cbCs, true);
      };
      if (name == "atexit" && call.arg_size() >= 1 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        emitHandlerCall(node(call.getArgOperand(0)), {});
        return true;
      }
      if (name == "on_exit" && call.arg_size() >= 2 &&
          call.getArgOperand(0)->getType()->isPointerTy()) {
        const Value *arg1 = call.getArgOperand(1);
        emitHandlerCall(node(call.getArgOperand(0)),
                        {0, arg1->getType()->isPointerTy() ? node(arg1) : 0});
        return true;
      }
      if ((name == "signal" || name == "__sysv_signal") &&
          call.arg_size() >= 2 &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        const NodeID handler = node(call.getArgOperand(1));
        if (call.getType()->isPointerTy()) {
          program->addCopy(handler, node(&call));
          program->addCopy(program->unknownValue(), node(&call));
        }
        emitHandlerCall(handler, {});
        return true;
      }
      if (name == "sigaction" && call.arg_size() >= 2 &&
          call.getArgOperand(1)->getType()->isPointerTy()) {
        const NodeID handler = program->addNode();
        emitLoad(node(call.getArgOperand(1)), handler);
        emitHandlerCall(handler, {});
        if (call.arg_size() >= 3 &&
            call.getArgOperand(2)->getType()->isPointerTy()) {
          emitStore(node(call.getArgOperand(2)), program->unknownValue(),
                    /*allowStrong=*/false);
        }
        return true;
      }

      if (auto allocInfo = getAllocatorInfo(callee)) {
        const ObjectID obj = getOrCreateCallAllocObject(call, *allocInfo);
        if (allocInfo->returnsPointer && call.getType()->isPointerTy()) {
          const NodeID result = node(&call);
          program->addCopy(program->address(obj), result);
          program->addCopy(program->nullValue(), result);
        } else {
          // Covers both explicit OUT-arg allocators (posix_memalign) and
          // specs marked ALLOC that return int/ssize_t and write *arg0
          // (asprintf, getline, getdelim).
          const int outIdx =
              allocInfo->ptrOutArgIndex >= 0 ? allocInfo->ptrOutArgIndex : 0;
          if (static_cast<unsigned>(outIdx) < call.arg_size()) {
            const Value *outArg = call.getArgOperand(outIdx);
            if (outArg->getType()->isPointerTy()) {
              emitStore(node(outArg), program->address(obj),
                        /*allowStrong=*/false);
            }
          }
        }
        if (isReallocatorName(callee->getName()) && call.arg_size() > 0 &&
            call.getArgOperand(0)->getType()->isPointerTy()) {
          const NodeID oldPtr = node(call.getArgOperand(0));
          if (call.getType()->isPointerTy())
            program->addCopy(oldPtr, node(&call));
          // realloc copies contents of *oldPtr into the new object.
          const NodeID copied = program->addNode();
          emitLoad(oldPtr, copied);
          emitStore(program->address(obj), copied, /*allowStrong=*/false);
        }
        return true;
      }

      // Check AliasSpecManager copy / return-alias specs.
      bool modeledBySpec = false;
      if (specManager) {
        const auto copies = specManager->getCopyEffects(callee);
        for (const auto &copy : copies) {
          modeledBySpec = true;
          if (copy.returnsAlias && call.getType()->isPointerTy()) {
            const int idx =
                copy.retArgIndex >= 0 ? copy.retArgIndex : copy.dstArgIndex;
            if (idx >= 0 && static_cast<unsigned>(idx) < call.arg_size() &&
                call.getArgOperand(idx)->getType()->isPointerTy()) {
              program->addCopy(node(call.getArgOperand(idx)), node(&call));
            }
          }
          if (copy.dstArgIndex >= 0 &&
              static_cast<unsigned>(copy.dstArgIndex) < call.arg_size() &&
              call.getArgOperand(copy.dstArgIndex)->getType()->isPointerTy()) {
            const NodeID dst = node(call.getArgOperand(copy.dstArgIndex));
            if (copy.srcArgIndex >= 0 &&
                static_cast<unsigned>(copy.srcArgIndex) < call.arg_size()) {
              const Value *srcVal = call.getArgOperand(copy.srcArgIndex);
              if (srcVal->getType()->isPointerTy()) {
                const NodeID src = node(srcVal);
                if (copy.srcIsRegion) {
                  const NodeID tmp = program->addNode();
                  emitLoad(src, tmp);
                  emitStore(dst, tmp, /*allowStrong=*/false);
                } else {
                  // Covers both `ArgN R ArgM V` and `ArgN D ArgM V`
                  // (e.g., strtol/strtod writing Arg0 into *Arg1).
                  emitStore(dst, src, /*allowStrong=*/false);
                }
              }
            } else if (copy.srcArgIndex < 0) {
              // srcArgIndex < 0 represents NULL or STATIC in AliasSpecManager.
              emitStore(dst, program->unknownValue(), /*allowStrong=*/false);
            }
          }
        }
        const auto retAliases = specManager->getReturnAliasInfo(callee);
        if (!retAliases.empty()) {
          modeledBySpec = true;
          if (call.getType()->isPointerTy()) {
            const NodeID res = node(&call);
            for (const auto &ret : retAliases) {
              if (ret.isNull)
                program->addCopy(program->nullValue(), res);
              if (ret.isStatic) {
                const ObjectID staticObj =
                    getOrCreateStaticReturnObject(callee);
                program->addCopy(program->address(staticObj), res);
              }
              if (ret.argIndex >= 0 &&
                  static_cast<unsigned>(ret.argIndex) < call.arg_size() &&
                  call.getArgOperand(ret.argIndex)->getType()->isPointerTy()) {
                const NodeID arg = node(call.getArgOperand(ret.argIndex));
                if (ret.isRegion) {
                  emitLoad(arg, res);
                } else {
                  program->addCopy(arg, res);
                }
              }
            }
          }
        }
        if (modeledBySpec)
          return true;
      }

      if (isNoEffectFunction(callee)) {
        if (call.getType()->isPointerTy())
          program->addCopy(program->unknownValue(), node(&call));
        return true;
      }
    }

    // Opaque external call: conservative modeling.
    const bool readOnly =
        call.doesNotAccessMemory() || call.onlyReadsMemory() ||
        (callee &&
         (callee->doesNotAccessMemory() || callee->onlyReadsMemory()));
    const bool noCallback =
        call.hasFnAttr(Attribute::NoCallback) ||
        (callee && callee->hasFnAttribute(Attribute::NoCallback));
    if (call.getType()->isPointerTy())
      program->addCopy(program->unknownValue(), node(&call));
    if (!readOnly)
      emitStore(program->unknownValue(), program->unknownValue(),
                /*allowStrong=*/false);
    if (!noCallback) {
      // Use a dedicated callback site with empty actualArgs so address-taken
      // callbacks receive `unknown` arguments (not caller actualArgs of an
      // indirect call).
      const CallSiteID cbSite =
          existingSite
              ? program->addCallSiteAfterCall(existingSite, 0, 0, {},
                                              mayReturn(call, callee),
                                              isa<InvokeInst>(call))
              : program->addCallSite(block, 0, 0, {}, mayReturn(call, callee),
                                     isa<InvokeInst>(call));
      program->markCallExternal(cbSite, true);
      for (const Function &fn : module) {
        if (fn.isDeclaration() || !fn.hasAddressTaken())
          continue;
        const auto &cbInfo = functions.at(&fn);
        if (readOnly) {
          for (const auto &[argIdx, byValObj] : cbInfo.byValParams) {
            (void)argIdx;
            program->addStoreBeforeCall(cbSite, program->address(byValObj),
                                        program->unknownValue(),
                                        /*allowStrong=*/false);
          }
        }
        program->addExternalCallback(cbSite, cbInfo.id);
      }
    }
    return false;
  }

  void buildInstructions() {
    for (const GlobalVariable &global : module.globals()) {
      if (!global.hasInitializer())
        continue;
      const ObjectID obj = objects.at(&global);
      const NodeID initial = program->addNode();
      bool addedAny = false;
      collectInitializer(global.getInitializer(), initial, addedAny);
      if (addedAny)
        program->setInitializer(obj, initial);
      else
        program->setEmptyInitializer(obj);
    }

    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      const auto &fnInfo = functions.at(&function);

      for (const BasicBlock &block : function) {
        const BlockID current = blockIds.at(&block);
        for (const Instruction &instruction : block) {
          if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
            const ObjectID obj = objects.at(alloca);
            if (program->object(obj).singleton) {
              // Strong reset for a singleton stack slot on activation without
              // injecting `unknown`.
              program->addStore(current, node(alloca), emptyValueNode,
                                /*allowStrong=*/true);
            } else if (!config.pointerOnlyMemory) {
              program->addStore(current, node(alloca), program->unknownValue(),
                                /*allowStrong=*/false);
            }
            continue;
          }

          if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
            if (load->getType()->isPointerTy()) {
              program->addLoad(current, node(load->getPointerOperand()),
                               node(load));
            }
          } else if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
            const Value *val = store->getValueOperand();
            const NodeID ptrNode = node(store->getPointerOperand());
            if (val->getType()->isPointerTy()) {
              const AccessID id = program->addStore(current, ptrNode, node(val),
                                                    /*allowStrong=*/true);
              const NodeID storedNode = program->access(id).value;
              storeNodes[store] = storedNode;
              recordNodeValue(storedNode, store);
            } else if (!config.pointerOnlyMemory) {
              const AccessID id = program->addStore(
                  current, ptrNode, program->unknownValue(), false);
              const NodeID storedNode = program->access(id).value;
              storeNodes[store] = storedNode;
              recordNodeValue(storedNode, store);
            } else if (typeContainsPointer(val->getType())) {
              std::vector<NodeID> ptrs;
              bool sawOpaque = false;
              collectAggregateValuePointers(val, ptrs, sawOpaque);
              if (sawOpaque)
                ptrs.push_back(program->unknownValue());
              if (!ptrs.empty()) {
                const NodeID merged = program->addNode();
                for (NodeID src : ptrs)
                  program->addCopy(src, merged);
                const AccessID id = program->addStore(current, ptrNode, merged,
                                                      /*allowStrong=*/false);
                const NodeID storedNode = program->access(id).value;
                storeNodes[store] = storedNode;
                recordNodeValue(storedNode, store);
              }
            } else if (containsPtrToInt(val)) {
              const AccessID id = program->addStore(
                  current, ptrNode, program->unknownValue(), false);
              const NodeID storedNode = program->access(id).value;
              storeNodes[store] = storedNode;
              recordNodeValue(storedNode, store);
            }
          } else if (const auto *phi = dyn_cast<PHINode>(&instruction)) {
            if (phi->getType()->isPointerTy())
              for (const Use &incoming : phi->incoming_values())
                program->addCopy(node(incoming.get()), node(phi));
          } else if (const auto *select = dyn_cast<SelectInst>(&instruction)) {
            if (select->getType()->isPointerTy()) {
              program->addCopy(node(select->getTrueValue()), node(select));
              program->addCopy(node(select->getFalseValue()), node(select));
            }
          } else if (const auto *gep =
                         dyn_cast<GetElementPtrInst>(&instruction)) {
            if (gep->getType()->isPointerTy())
              program->addCopy(node(gep->getPointerOperand()), node(gep));
          } else if (const auto *cast = dyn_cast<CastInst>(&instruction)) {
            if (cast->getType()->isPointerTy())
              program->addCopy(node(cast->getOperand(0)), node(cast));
          } else if (const auto *freeze = dyn_cast<FreezeInst>(&instruction)) {
            if (freeze->getType()->isPointerTy())
              program->addCopy(node(freeze->getOperand(0)), node(freeze));
          } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(&instruction)) {
            if (rmw->getType()->isPointerTy()) {
              program->addLoad(current, node(rmw->getPointerOperand()),
                               node(rmw));
              program->addStore(current, node(rmw->getPointerOperand()),
                                node(rmw->getValOperand()), false);
            } else if (!config.pointerOnlyMemory ||
                       containsPtrToInt(rmw->getValOperand())) {
              program->addStore(current, node(rmw->getPointerOperand()),
                                program->unknownValue(), false);
            }
          } else if (const auto *cmp =
                         dyn_cast<AtomicCmpXchgInst>(&instruction)) {
            if (cmp->getNewValOperand()->getType()->isPointerTy()) {
              program->addLoad(current, node(cmp->getPointerOperand()),
                               atomicResults.at(cmp));
              program->addStore(current, node(cmp->getPointerOperand()),
                                node(cmp->getNewValOperand()), false);
            } else if (!config.pointerOnlyMemory ||
                       containsPtrToInt(cmp->getNewValOperand())) {
              program->addLoad(current, node(cmp->getPointerOperand()),
                               atomicResults.at(cmp));
              program->addStore(current, node(cmp->getPointerOperand()),
                                program->unknownValue(), false);
            }
          } else if (const auto *extract =
                         dyn_cast<ExtractValueInst>(&instruction)) {
            if (extract->getType()->isPointerTy()) {
              const auto *cmp =
                  dyn_cast<AtomicCmpXchgInst>(extract->getAggregateOperand());
              if (cmp && extract->getNumIndices() == 1 &&
                  *extract->idx_begin() == 0) {
                program->addCopy(atomicResults.at(cmp), node(extract));
              } else {
                std::vector<NodeID> ptrs;
                bool sawOpaque = false;
                collectAggregateValuePointers(extract->getAggregateOperand(),
                                              ptrs, sawOpaque);
                if (sawOpaque || ptrs.empty())
                  program->addCopy(program->unknownValue(), node(extract));
                else
                  for (NodeID p : ptrs)
                    program->addCopy(p, node(extract));
              }
            }
          } else if (const auto *extractElem =
                         dyn_cast<ExtractElementInst>(&instruction)) {
            if (extractElem->getType()->isPointerTy()) {
              std::vector<NodeID> ptrs;
              bool sawOpaque = false;
              collectAggregateValuePointers(extractElem->getVectorOperand(),
                                            ptrs, sawOpaque);
              if (sawOpaque || ptrs.empty())
                program->addCopy(program->unknownValue(), node(extractElem));
              else
                for (NodeID p : ptrs)
                  program->addCopy(p, node(extractElem));
            }
          } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
            if (handleIntrinsic(*call, current))
              continue;
            if (call->isInlineAsm()) {
              if (call->getType()->isPointerTy())
                program->addCopy(program->unknownValue(), node(call));
              if (!call->doesNotAccessMemory() && !call->onlyReadsMemory())
                program->addStore(current, program->unknownValue(),
                                  program->unknownValue(),
                                  /*allowStrong=*/false);
              continue;
            }
            const Function *callee = directCallee(*call);
            if (callee && !callee->isDeclaration()) {
              emitByValCopies(current, *call, callee);
              std::vector<NodeID> actualArgs;
              actualArgs.reserve(call->arg_size());
              for (unsigned i = 0; i < call->arg_size(); ++i) {
                const Value *arg = call->getArgOperand(i);
                actualArgs.push_back(arg->getType()->isPointerTy() ? node(arg)
                                                                   : 0);
              }
              const NodeID resNode =
                  call->getType()->isPointerTy() ? node(call) : 0;
              const CallSiteID cs = program->addCallSite(
                  current, 0, resNode, std::move(actualArgs),
                  mayReturn(*call, callee), isa<InvokeInst>(*call));
              program->addCallTarget(cs, functions.at(callee).id);
            } else if (callee && callee->isDeclaration()) {
              const bool specModeled = modelExternalCall(current, *call, callee);
              externalCalls.push_back({call, current, !specModeled});
            } else {
              // Indirect call: resolved dynamically inside vfg::Solver.
              std::vector<NodeID> actualArgs;
              actualArgs.reserve(call->arg_size());
              for (unsigned i = 0; i < call->arg_size(); ++i) {
                const Value *arg = call->getArgOperand(i);
                actualArgs.push_back(arg->getType()->isPointerTy() ? node(arg)
                                                                   : 0);
              }
              const NodeID resNode =
                  call->getType()->isPointerTy() ? node(call) : 0;
              const NodeID calleePtr = node(call->getCalledOperand());
              const CallSiteID cs = program->addCallSite(
                  current, calleePtr, resNode, std::move(actualArgs),
                  mayReturn(*call), isa<InvokeInst>(*call));
              indirectCalls.push_back(
                  {call, current, cs, calleePtr, {}, {}, false});
            }
          } else if (const auto *ret = dyn_cast<ReturnInst>(&instruction)) {
            const Value *value = ret->getReturnValue();
            if (fnInfo.returned && value)
              program->addCopy(node(value), fnInfo.returned);
          } else {
            if (instruction.getType()->isPointerTy())
              program->addCopy(program->unknownValue(), node(&instruction));
            if (instruction.mayWriteToMemory())
              program->addStore(current, program->unknownValue(),
                                program->unknownValue(), false);
          }
        }

        const Instruction *terminator = block.getTerminator();
        if (succ_empty(&block)) {
          if (!isa<UnreachableInst>(terminator))
            program->addControlEdge(current, fnInfo.exit);
        } else {
          for (const BasicBlock *successor : successors(&block))
            program->addControlEdge(current, blockIds.at(successor));
        }
      }
    }
  }

  void run() {
    analyzed = false;
    solver.reset();
    program = std::make_unique<vfg::Program>();
    stats = {};
    emptyValueNode = 0;
    values.clear();
    storeNodes.clear();
    nodeValues.clear();
    objects.clear();
    objectValues.assign(3, nullptr);
    inverse.clear();
    blockIds.clear();
    atomicResults.clear();
    staticReturnObjects.clear();
    callAllocObjects.clear();
    functions.clear();
    functionById.clear();
    externalCalls.clear();
    indirectCalls.clear();
    entryPoints.clear();

    if (config.useExternalSpecs) {
      static thread_local const AliasSpecManager sharedSpecs;
      specManager = &sharedSpecs;
    } else {
      specManager = nullptr;
    }

    collectObjectsAndFunctions();
    setupEntryPointsAndLifecycle();
    buildInstructions();

    vfg::Solver::Config solverConfig;
    solverConfig.enableStrongUpdates = config.enableStrongUpdates;

    // Defined indirect targets resolve dynamically inside a single
    // Solver::analyze() pass. Only loop again if a reachable indirect call
    // discovers an external declaration target, an unknown external target,
    // or a defined target with byval parameters requiring caller-side copies.
    while (true) {
      solver = std::make_unique<vfg::Solver>(*program, solverConfig);
      solver->analyze();
      ++stats.callGraphIterations;

      struct PendingExternal {
        std::size_t idx;
        const Function *callee; // nullptr means unknown external
      };
      struct PendingByVal {
        std::size_t idx;
        const Function *callee;
      };
      std::vector<PendingExternal> newExternals;
      std::vector<PendingByVal> newByVals;

      for (std::size_t i = 0; i < indirectCalls.size(); ++i) {
        auto &rec = indirectCalls[i];
        if (!solver->isBlockReachable(rec.block))
          continue;

        for (FunctionID fnId : solver->resolvedCallTargets(rec.site)) {
          auto fnIt = functionById.find(fnId);
          if (fnIt == functionById.end())
            continue;
          const Function *callee = fnIt->second;
          if (!functions.at(callee).byValParams.empty() &&
              !rec.modeledByValTargets.count(callee)) {
            newByVals.push_back({i, callee});
          }
        }

        for (ObjectID obj : solver->pointsTo(rec.calledPointer)) {
          if (obj == program->unknownObject()) {
            if (!rec.modeledUnknownExternal)
              newExternals.push_back({i, nullptr});
          } else if (const auto *callee =
                         dyn_cast_or_null<Function>(objectValues.at(obj))) {
            if (callee->isDeclaration() &&
                !rec.modeledExternalTargets.count(callee)) {
              newExternals.push_back({i, callee});
            }
          }
        }
      }

      if (newExternals.empty() && newByVals.empty())
        break;

      solver.reset();
      for (const auto &bv : newByVals) {
        auto &rec = indirectCalls[bv.idx];
        rec.modeledByValTargets.insert(bv.callee);
        emitByValCopies(rec.block, *rec.instruction, bv.callee, rec.site);
      }
      for (const auto &ext : newExternals) {
        auto &rec = indirectCalls[ext.idx];
        if (ext.callee)
          rec.modeledExternalTargets.insert(ext.callee);
        else
          rec.modeledUnknownExternal = true;
        const CallBase *inst = rec.instruction;
        const BlockID blk = rec.block;
        const CallSiteID site = rec.site;
        const bool specModeled =
            modelExternalCall(blk, *inst, ext.callee, site);
        externalCalls.push_back({inst, blk, !specModeled});
      }
    }

    // Collect final statistics from the converged solver.
    stats.solver = solver->statistics();
    stats.resolvedIndirectTargets = 0;
    for (const auto &rec : indirectCalls) {
      if (!solver->isBlockReachable(rec.block))
        continue;
      stats.resolvedIndirectTargets +=
          solver->resolvedCallTargets(rec.site).size() +
          rec.modeledExternalTargets.size();
    }
    stats.conservativeExternalCalls = 0;
    stats.specModeledCalls = 0;
    for (const auto &rec : externalCalls) {
      if (!solver->isBlockReachable(rec.block))
        continue;
      if (rec.conservative)
        ++stats.conservativeExternalCalls;
      else
        ++stats.specModeledCalls;
    }

    // Populate pointed-to-by inverse index over tracked LLVM Values.
    inverse.resize(program->objectCount() + 1);
    for (const auto &entry : values)
      for (ObjectID object : solver->pointsTo(entry.second))
        inverse[object].insert(entry.first);

    stats.functions = program->functionCount();
    stats.callSites = program->callSiteCount();
    stats.objects = program->objectCount();
    stats.valueNodes = program->nodeCount();
    stats.controlBlocks = program->blockCount();
    analyzed = true;
  }

  void requireAnalyzed() const {
    if (!analyzed)
      throw std::logic_error("ValueFlowPTA: analyze() must precede queries");
  }
};

ValueFlowPTA::ValueFlowPTA(const Module &module)
    : ValueFlowPTA(module, Config{}) {}
ValueFlowPTA::ValueFlowPTA(const Module &module, Config config)
    : impl_(std::make_unique<Impl>(module, std::move(config))) {}
ValueFlowPTA::~ValueFlowPTA() = default;

void ValueFlowPTA::analyze() { impl_->run(); }

const ValueFlowPTA::PointsToSet &
ValueFlowPTA::getPointsTo(const Value *value) const {
  impl_->requireAnalyzed();
  static const PointsToSet empty;
  const auto it = impl_->values.find(value);
  return it == impl_->values.end() ? empty
                                   : impl_->solver->pointsTo(it->second);
}

const ValueFlowPTA::PointedToBySet &
ValueFlowPTA::getPointedToBy(ObjectID object) const {
  impl_->requireAnalyzed();
  if (!object || object >= impl_->inverse.size())
    throw std::out_of_range("ValueFlowPTA: invalid object");
  return impl_->inverse[object];
}

ValueFlowPTA::PointedToByStoreSet
ValueFlowPTA::getPointedToByStores(ObjectID object) const {
  impl_->requireAnalyzed();
  PointedToByStoreSet result;
  for (NodeID id : impl_->solver->pointedToBy(object)) {
    if (id < impl_->nodeValues.size()) {
      if (const auto *store = dyn_cast_or_null<StoreInst>(impl_->nodeValues[id]))
        result.insert(store);
    }
  }
  return result;
}

bool ValueFlowPTA::mayAlias(const Value *left, const Value *right) const {
  impl_->requireAnalyzed();
  auto a = impl_->values.find(left), b = impl_->values.find(right);
  if (a == impl_->values.end() || b == impl_->values.end())
    return true;
  return impl_->solver->mayAlias(a->second, b->second);
}

ValueFlowPTA::ObjectID
ValueFlowPTA::getObjectId(const Value *allocation) const {
  impl_->requireAnalyzed();
  auto it = impl_->objects.find(allocation);
  return it == impl_->objects.end() ? 0 : it->second;
}

const Value *ValueFlowPTA::getObjectValue(ObjectID object) const {
  impl_->requireAnalyzed();
  return object < impl_->objectValues.size() ? impl_->objectValues[object]
                                             : nullptr;
}

ValueFlowPTA::ObjectID ValueFlowPTA::getNullObjectId() const {
  impl_->requireAnalyzed();
  return impl_->program->nullObject();
}

ValueFlowPTA::ObjectID ValueFlowPTA::getUnknownObjectId() const {
  impl_->requireAnalyzed();
  return impl_->program->unknownObject();
}

const ValueFlowPTA::Statistics &ValueFlowPTA::getStatistics() const {
  impl_->requireAnalyzed();
  return impl_->stats;
}

const vfg::Program &ValueFlowPTA::getProgram() const {
  impl_->requireAnalyzed();
  return *impl_->program;
}

const vfg::Solver &ValueFlowPTA::getSolver() const {
  impl_->requireAnalyzed();
  return *impl_->solver;
}

ValueFlowPTA::NodeID ValueFlowPTA::getValueNode(const Value *value) const {
  impl_->requireAnalyzed();
  auto it = impl_->values.find(value);
  return it == impl_->values.end() ? 0 : it->second;
}

ValueFlowPTA::NodeID ValueFlowPTA::getStoreNode(const StoreInst *store) const {
  impl_->requireAnalyzed();
  auto it = impl_->storeNodes.find(store);
  return it == impl_->storeNodes.end() ? 0 : it->second;
}

const Value *ValueFlowPTA::getNodeValue(NodeID node) const {
  impl_->requireAnalyzed();
  return node < impl_->nodeValues.size() ? impl_->nodeValues[node] : nullptr;
}

vfg::NodeSet ValueFlowPTA::getVFGSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  return impl_->solver->vfgSuccessors(node);
}

vfg::NodeSet ValueFlowPTA::getVFGIndirectSuccessors(NodeID node) const {
  impl_->requireAnalyzed();
  return impl_->solver->vfgIndirectSuccessors(node);
}

} // namespace lotus::alias
