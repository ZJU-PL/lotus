#include "Alias/InclusionBased/FlowSensitive/ValueFlow/ValueFlowPTA.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
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
bool allocator(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "malloc" || name == "calloc" || name == "realloc" ||
         name == "aligned_alloc" || name == "valloc" || name == "memalign" ||
         name == "_Znwm" || name == "_Znam" || name == "_Znwj" ||
         name == "_Znaj";
}
bool deallocator(const Function *function) {
  if (!function || !function->isDeclaration())
    return false;
  const StringRef name = function->getName();
  return name == "free" || name == "_ZdlPv" || name == "_ZdaPv" ||
         name == "_ZdlPvm" || name == "_ZdaPvm";
}
bool scalarType(const Type *type) {
  return type->isSingleValueType() && !type->isVectorTy();
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
} // namespace

class ValueFlowPTA::Impl {
public:
  Impl(const Module &module, Config config)
      : module(module), config(std::move(config)) {}
  const Module &module;
  Config config;
  Statistics stats;
  bool analyzed = false;
  std::unique_ptr<vfg::Program> program;
  std::unique_ptr<vfg::Solver> solver;
  std::unordered_map<const Value *, NodeID> values;
  std::unordered_map<const Value *, ObjectID> objects;
  std::vector<const Value *> objectValues;
  std::vector<PointedToBySet> inverse;
  std::vector<BlockID> roots;
  std::unordered_map<const BasicBlock *, BlockID> entries;
  std::unordered_map<const AtomicCmpXchgInst *, NodeID> atomicResults;

  struct FunctionInfo {
    BlockID entry = 0;
    BlockID exit = 0;
    NodeID returned = 0;
  };
  std::map<const Function *, FunctionInfo> functions;
  struct StackObject {
    ObjectID object;
    const Function *function;
    bool uniqueSite;
  };
  std::vector<StackObject> stackObjects;
  struct CallSite {
    const CallBase *instruction = nullptr;
    BlockID dispatch = 0;
    BlockID continuation = 0;
    NodeID calledPointer = 0;
    std::set<const Function *> targets;
    bool external = false;
    BlockID externalStub = 0;
    std::set<const Function *> callbacks;
  };
  std::vector<CallSite> calls;

  ObjectID makeObject(const Value *value, bool singleton, bool memory) {
    const ObjectID object = program->addObject(
        singleton, memory, value->hasName() ? value->getName().str() : "");
    objects[value] = object;
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
    if (const auto *alias = dyn_cast<GlobalAlias>(value)) {
      program->addCopy(node(alias->getAliasee()), result);
    } else if (const auto *expression = dyn_cast<ConstantExpr>(value)) {
      if ((expression->isCast() ||
           expression->getOpcode() == Instruction::GetElementPtr) &&
          expression->getOperand(0)->getType()->isPointerTy())
        program->addCopy(node(expression->getOperand(0)), result);
      else
        program->addCopy(program->unknownValue(), result);
    } else if (isa<Constant>(value) ||
               (!isa<Instruction>(value) && !isa<Argument>(value))) {
      program->addCopy(program->unknownValue(), result);
    }
    return result;
  }

  void collectObjectsAndNodes() {
    for (const GlobalVariable &global : module.globals()) {
      const auto object =
          makeObject(&global, scalarType(global.getValueType()), true);
      values[&global] = program->address(object);
    }
    for (const Function &function : module) {
      const auto object = makeObject(&function, false, false);
      values[&function] = program->address(object);
    }
    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      for (const Argument &argument : function.args()) {
        if (!argument.getType()->isPointerTy())
          continue;
        if (argument.hasByValAttr()) {
          // byval is a private copy, not an alias of caller-owned storage.
          const auto object = makeObject(&argument, false, true);
          values[&argument] = program->address(object);
        } else {
          node(&argument);
        }
      }
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
            values[alloca] = program->address(object);
            stackObjects.push_back({object, &function, unique});
          } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
            const Function *callee = directCallee(*call);
            if (call->getType()->isPointerTy() && allocator(callee)) {
              const auto object = makeObject(call, false, true);
              const NodeID result = node(call);
              program->addCopy(program->address(object), result);
              program->addCopy(program->nullValue(), result);
              // realloc may resize in place and return its original argument.
              if (callee->getName() == "realloc" && call->arg_size() != 0)
                program->addCopy(node(call->getArgOperand(0)), result);
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

  // Flatten aggregate initializers into one summary cell, matching the paper's
  // monolithic treatment of arrays and structures (not field sensitivity).
  void initializer(const Constant *constant, NodeID destination) {
    if (!constant)
      return;
    if (constant->getType()->isPointerTy()) {
      program->addCopy(node(constant), destination);
    } else if (isa<ConstantAggregateZero>(constant)) {
      program->addCopy(program->nullValue(), destination);
    } else if (isa<UndefValue>(constant) || isa<PoisonValue>(constant)) {
      program->addCopy(program->unknownValue(), destination);
    } else if (constant->getType()->isAggregateType() ||
               constant->getType()->isVectorTy()) {
      if (constant->getNumOperands() == 0)
        program->addCopy(program->unknownValue(), destination);
      for (const Use &operand : constant->operands())
        initializer(dyn_cast<Constant>(operand.get()), destination);
    } else {
      // Reinterpreting non-pointer bytes as a pointer is outside the four
      // primitive instructions in the paper; preserve it as unknown.
      program->addCopy(program->unknownValue(), destination);
    }
  }

  void createControlBlocks() {
    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      FunctionInfo info;
      info.entry = program->addBlock();
      info.exit = program->addBlock();
      if (function.getReturnType()->isPointerTy())
        info.returned = program->addNode();
      functions[&function] = info;
      for (const BasicBlock &block : function)
        entries[&block] = program->addBlock();
      program->addControlEdge(info.entry,
                              entries.at(&function.getEntryBlock()));
    }
    std::vector<const Function *> entryPoints = config.entryPoints;
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
          function->isDeclaration())
        throw std::invalid_argument(
            "ValueFlowPTA: entry point is not a module definition");
      const auto entry = functions.at(function).entry;
      roots.push_back(entry);
      program->addRoot(entry);
      for (const Argument &argument : function->args())
        if (argument.getType()->isPointerTy() && !argument.hasByValAttr())
          program->addCopy(program->unknownValue(), node(&argument));
    }
  }

  bool intrinsic(const CallBase &call, BlockID block) {
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
      if (call.getType()->isPointerTy())
        program->addCopy(program->unknownValue(), node(&call));
      return true;
    default:
      break;
    }
    if (const auto *memory = dyn_cast<MemIntrinsic>(instruction)) {
      const auto *length = dyn_cast<ConstantInt>(memory->getLength());
      if (!length || !length->isZero()) {
        // A partial byte write can construct an arbitrary pointer. A simple
        // source->destination copy would be unsound for a partial memcpy.
        program->addStore(block, node(memory->getDest()),
                          program->unknownValue(), false);
      }
      return true;
    }
    if (call.getType()->isPointerTy())
      program->addCopy(program->unknownValue(), node(&call));
    if (!call.doesNotAccessMemory() && !call.onlyReadsMemory())
      program->addStore(block, program->unknownValue(), program->unknownValue(),
                        false);
    return true;
  }

  void buildInstructions() {
    for (const GlobalVariable &global : module.globals()) {
      if (!global.hasInitializer())
        continue;
      const NodeID initial = program->addNode();
      initializer(global.getInitializer(), initial);
      program->setInitializer(objects.at(&global), initial);
    }
    for (const Function &function : module) {
      if (function.isDeclaration())
        continue;
      for (const BasicBlock &block : function) {
        BlockID current = entries.at(&block);
        for (const Instruction &instruction : block) {
          if (const auto *alloca = dyn_cast<AllocaInst>(&instruction)) {
            // A new invocation must not inherit a previous invocation's stack
            // contents. Summary sites remain weak; singleton slots reset.
            program->addStore(current, node(alloca), program->unknownValue());
            continue;
          }
          if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
            if (load->getType()->isPointerTy())
              program->addLoad(current, node(load->getPointerOperand()),
                               node(load));
          } else if (const auto *store = dyn_cast<StoreInst>(&instruction)) {
            const bool fullPointer =
                store->getValueOperand()->getType()->isPointerTy();
            program->addStore(current, node(store->getPointerOperand()),
                              node(store->getValueOperand()), fullPointer);
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
            program->addCopy(node(gep->getPointerOperand()), node(gep));
          } else if (const auto *cast = dyn_cast<CastInst>(&instruction)) {
            if (cast->getType()->isPointerTy())
              program->addCopy(node(cast->getOperand(0)), node(cast));
          } else if (const auto *freeze = dyn_cast<FreezeInst>(&instruction)) {
            if (freeze->getType()->isPointerTy())
              program->addCopy(node(freeze->getOperand(0)), node(freeze));
          } else if (const auto *rmw = dyn_cast<AtomicRMWInst>(&instruction)) {
            if (rmw->getType()->isPointerTy())
              program->addLoad(current, node(rmw->getPointerOperand()),
                               node(rmw));
            program->addStore(current, node(rmw->getPointerOperand()),
                              node(rmw->getValOperand()), false);
          } else if (const auto *cmp =
                         dyn_cast<AtomicCmpXchgInst>(&instruction)) {
            program->addLoad(current, node(cmp->getPointerOperand()),
                             atomicResults.at(cmp));
            program->addStore(current, node(cmp->getPointerOperand()),
                              node(cmp->getNewValOperand()), false);
          } else if (const auto *extract =
                         dyn_cast<ExtractValueInst>(&instruction)) {
            if (extract->getType()->isPointerTy()) {
              const auto *cmp =
                  dyn_cast<AtomicCmpXchgInst>(extract->getAggregateOperand());
              if (cmp && extract->getNumIndices() == 1 &&
                  *extract->idx_begin() == 0)
                program->addCopy(atomicResults.at(cmp), node(extract));
              else
                program->addCopy(program->unknownValue(), node(extract));
            }
          } else if (const auto *call = dyn_cast<CallBase>(&instruction)) {
            if (intrinsic(*call, current))
              continue;
            const auto continuation = program->addBlock();
            CallSite site;
            site.instruction = call;
            site.dispatch = current;
            site.continuation = continuation;
            site.calledPointer = node(call->getCalledOperand());
            calls.push_back(std::move(site));
            current = continuation;
          } else if (const auto *ret = dyn_cast<ReturnInst>(&instruction)) {
            const Value *value = ret->getReturnValue();
            if (functions.at(&function).returned && value)
              program->addCopy(node(value), functions.at(&function).returned);
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
            program->addControlEdge(current, functions.at(&function).exit);
        } else {
          for (const BasicBlock *successor : successors(&block))
            program->addControlEdge(current, entries.at(successor));
        }
      }
    }
  }

  bool mayReturn(const CallBase &call) const {
    // Exceptions may leave an invoke even when it cannot return normally.
    return !call.doesNotReturn() || isa<InvokeInst>(call);
  }

  void connectFunction(CallSite &site, const Function *callee) {
    if (!site.targets.insert(callee).second)
      return;
    const auto &info = functions.at(callee);
    program->addControlEdge(site.dispatch, info.entry);
    if (mayReturn(*site.instruction))
      program->addControlEdge(info.exit, site.continuation);
    unsigned index = 0;
    for (const Argument &argument : callee->args()) {
      if (argument.getType()->isPointerTy() && !argument.hasByValAttr()) {
        const NodeID actual = index < site.instruction->arg_size()
                                  ? node(site.instruction->getArgOperand(index))
                                  : program->unknownValue();
        program->addCopy(actual, node(&argument));
      }
      ++index;
    }
    if (site.instruction->getType()->isPointerTy())
      program->addCopy(info.returned ? info.returned : program->unknownValue(),
                       node(site.instruction));
    if (!directCallee(*site.instruction))
      ++stats.resolvedIndirectTargets;
  }

  void connectExternal(CallSite &site) {
    if (site.external)
      return;
    site.external = true;
    const CallBase &call = *site.instruction;
    const Function *callee = directCallee(call);
    const bool allocation = allocator(callee);
    const bool deallocation = deallocator(callee);
    const BlockID stub = program->addBlock();
    site.externalStub = stub;
    program->addControlEdge(site.dispatch, stub);
    if (mayReturn(call))
      program->addControlEdge(stub, site.continuation);
    if (call.getType()->isPointerTy() && !allocation)
      program->addCopy(program->unknownValue(), node(&call));
    if (allocation || deallocation)
      return;
    ++stats.conservativeExternalCalls;
    if (!call.doesNotAccessMemory() && !call.onlyReadsMemory())
      program->addStore(stub, program->unknownValue(), program->unknownValue(),
                        false);
    if (call.hasFnAttr(Attribute::NoCallback))
      return;
    // Opaque external code may invoke escaping callbacks. Model arbitrary
    // callback arguments and repeated invocations, with the external effects
    // both before and after each callback. Symbol-name discovery/dlopen is not
    // modeled; these candidates are functions whose addresses occur in the IR.
    for (const Function &function : module) {
      if (function.isDeclaration() || !function.hasAddressTaken())
        continue;
      site.callbacks.insert(&function);
      const auto &info = functions.at(&function);
      program->addControlEdge(stub, info.entry);
      program->addControlEdge(info.exit, stub);
      for (const Argument &argument : function.args())
        if (argument.getType()->isPointerTy() && !argument.hasByValAttr())
          program->addCopy(program->unknownValue(), node(&argument));
    }
  }

  // Re-evaluate recursion whenever the resolved call graph grows. Never use
  // strong updates on a stack site summarizing several recursive activations.
  void updateSingletons() {
    std::map<const Function *, std::set<const Function *>> successors;
    for (const auto &call : calls) {
      auto &targets = successors[call.instruction->getFunction()];
      targets.insert(call.targets.begin(), call.targets.end());
      targets.insert(call.callbacks.begin(), call.callbacks.end());
    }
    std::set<const Function *> recursive;
    for (const auto &entry : functions) {
      const Function *origin = entry.first;
      std::set<const Function *> seen;
      std::vector<const Function *> work(successors[origin].begin(),
                                         successors[origin].end());
      while (!work.empty()) {
        const auto *current = work.back();
        work.pop_back();
        if (current == origin) {
          recursive.insert(origin);
          break;
        }
        if (!seen.insert(current).second)
          continue;
        const auto &next = successors[current];
        work.insert(work.end(), next.begin(), next.end());
      }
    }
    for (const auto &object : stackObjects)
      program->setSingleton(object.object,
                            object.uniqueSite &&
                                !recursive.count(object.function));
  }

  std::set<BlockID> reachableBlocks() const {
    std::set<BlockID> seen;
    std::vector<BlockID> work = roots;
    while (!work.empty()) {
      const auto block = work.back();
      work.pop_back();
      if (!seen.insert(block).second)
        continue;
      const auto &next = program->block(block).successors;
      work.insert(work.end(), next.begin(), next.end());
    }
    return seen;
  }

  void run() {
    analyzed = false;
    solver.reset();
    program = std::make_unique<vfg::Program>();
    stats = {};
    values.clear();
    objects.clear();
    objectValues.assign(3, nullptr);
    inverse.clear();
    roots.clear();
    entries.clear();
    atomicResults.clear();
    functions.clear();
    stackObjects.clear();
    calls.clear();
    // Lifecycle lists are not ordinary constant initializers. Silently ignoring
    // their implicit calls would produce incomplete results for C++ modules.
    for (const char *name : {"llvm.global_ctors", "llvm.global_dtors"}) {
      if (const GlobalVariable *list = module.getNamedGlobal(name)) {
        const auto *type = dyn_cast<ArrayType>(list->getValueType());
        if (!type || type->getNumElements() != 0)
          throw std::invalid_argument(
              "ValueFlowPTA: lower global constructors/destructors to explicit "
              "entry-point calls and remove their lifecycle lists first");
      }
    }
    collectObjectsAndNodes();
    createControlBlocks();
    buildInstructions();
    vfg::Solver::Config solverConfig;
    solverConfig.enableStrongUpdates = config.enableStrongUpdates;
    while (true) {
      // Direct calls require no points-to solution. Close their reachable
      // call graph before solving, including direct calls exposed by a newly
      // resolved indirect target. Do not let unreachable callers contribute
      // return paths or external effects.
      bool connectedDirect;
      do {
        connectedDirect = false;
        const auto reachable = reachableBlocks();
        for (auto &site : calls) {
          const Function *callee = directCallee(*site.instruction);
          if (!callee || !reachable.count(site.dispatch))
            continue;
          if (callee->isDeclaration()) {
            if (!site.external) {
              connectExternal(site);
              connectedDirect = true;
            }
          } else if (!site.targets.count(callee)) {
            connectFunction(site, callee);
            connectedDirect = true;
          }
        }
      } while (connectedDirect);
      updateSingletons();
      solver = std::make_unique<vfg::Solver>(*program, solverConfig);
      solver->analyze();
      ++stats.callGraphIterations;
      stats.solver = solver->statistics();
      const auto reachable = reachableBlocks();
      // Collect decisions first: the Program is immutable while Solver lives.
      struct Connection {
        std::size_t site;
        const Function *target;
      };
      std::vector<Connection> connections;
      for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto &site = calls[i];
        if (!reachable.count(site.dispatch))
          continue;
        const auto *direct = directCallee(*site.instruction);
        auto propose = [&](const Function *target) {
          if (!target || target->isDeclaration()) {
            if (!site.external)
              connections.push_back({i, nullptr});
          } else if (!site.targets.count(target)) {
            connections.push_back({i, target});
          }
        };
        if (direct) {
          propose(direct);
          continue;
        }
        const auto &targets = solver->pointsTo(site.calledPointer);
        for (ObjectID object : targets) {
          if (object == program->unknownObject()) {
            propose(nullptr);
            for (const Function &candidate : module)
              if (!candidate.isDeclaration())
                propose(&candidate);
          } else if (const auto *function =
                         dyn_cast_or_null<Function>(objectValues.at(object))) {
            propose(function);
          }
        }
      }
      if (connections.empty())
        break;
      solver.reset();
      for (const auto &connection : connections) {
        if (connection.target)
          connectFunction(calls[connection.site], connection.target);
        else
          connectExternal(calls[connection.site]);
      }
      // Calls can add CFG paths, invalidating prior reaching definitions.
      // Rebuild the VFG from direct edges instead of retaining stale flows.
    }
    inverse.resize(program->objectCount() + 1);
    for (const auto &entry : values)
      for (ObjectID object : solver->pointsTo(entry.second))
        inverse[object].insert(entry.first);
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
} // namespace lotus::alias
