#include "Concurrency/CUDA/CUDAAnalysis.h"
#include "Concurrency/CUDA/CUDAAnalysisHelpers.h"
#include "Concurrency/CUDA/CUDAFunctionSummary.h"
#include "Concurrency/CUDA/CUDAKernelProtocolAnalysis.h"
#include "Concurrency/CUDA/CUDAModel.h"
#include "Concurrency/CUDA/CUDAParticipantAnalysis.h"
#include "Concurrency/CUDA/CUDAStreamAutomaton.h"

#include <algorithm>
#include <limits>

#include <llvm/Analysis/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace concurrency::cuda {

namespace detail {

static void addOrderedDependency(llvm::SmallVectorImpl<size_t> &deps,
                                 size_t dep) {
  if (!llvm::is_contained(deps, dep)) {
    deps.push_back(dep);
  }
}

static void addOrderedDependencies(llvm::SmallVectorImpl<size_t> &deps,
                                   llvm::ArrayRef<size_t> more) {
  for (size_t dep : more) {
    addOrderedDependency(deps, dep);
  }
}

static bool isLegacyConfigureCall(const CallBase *call) {
  const Function *callee = call ? call->getCalledFunction() : nullptr;
  return callee && callee->getName().contains("cudaConfigureCall");
}

bool isNVVMKernel(const Function *function) {
  return function && (function->hasFnAttribute("nvvm.kernel") ||
                      function->getCallingConv() == CallingConv::PTX_Kernel);
}

bool isCUDAKernelCandidate(const Function *function) {
  return function && !function->isDeclaration();
}

static const Function *resolveFunctionFromValue(const Value *value) {
  const Value *current = value;
  SmallPtrSet<const Value *, 8> visited;
  while (current && visited.insert(current).second) {
    current = current->stripPointerCasts();
    if (const auto *fn = dyn_cast<Function>(current)) {
      return fn;
    }
    if (const auto *ce = dyn_cast<ConstantExpr>(current)) {
      if (ce->isCast() || ce->getOpcode() == Instruction::PtrToInt ||
          ce->getOpcode() == Instruction::IntToPtr) {
        current = ce->getOperand(0);
        continue;
      }
    }
    if (const auto *inst = dyn_cast<Instruction>(current)) {
      if (inst->getOpcode() == Instruction::BitCast ||
          inst->getOpcode() == Instruction::AddrSpaceCast ||
          inst->getOpcode() == Instruction::PtrToInt ||
          inst->getOpcode() == Instruction::IntToPtr) {
        current = inst->getOperand(0);
        continue;
      }
    }
    break;
  }
  return nullptr;
}

struct DecodedLaunch {
  const Function *kernel = nullptr;
  LaunchDimensions dimensions;
  const Value *argument_array = nullptr;
  const Value *dynamic_shared_memory = nullptr;
  const Value *stream = nullptr;
  bool recognized_layout = false;
};

static StringRef normalizeLaunchEntryName(StringRef name) {
  name = CUDAModel::normalizeLaunchName(name);
  if (name.endswith("_ptsz") || name.endswith("_ptds")) {
    name = name.drop_back(5);
  }
  return name;
}

static const Value *getAggregateElement(const Value *value, unsigned index) {
  if (!value) {
    return nullptr;
  }
  if (const auto *constant = dyn_cast<Constant>(value)) {
    return constant->getAggregateElement(index);
  }
  if (const auto *insert = dyn_cast<InsertValueInst>(value)) {
    ArrayRef<unsigned> indices = insert->getIndices();
    if (indices.size() == 1 && indices.front() == index) {
      return insert->getInsertedValueOperand();
    }
    return getAggregateElement(insert->getAggregateOperand(), index);
  }
  if (const auto *load = dyn_cast<LoadInst>(value)) {
    const Value *pointer = load->getPointerOperand()->stripPointerCasts();
    if (const auto *global = dyn_cast<GlobalVariable>(pointer)) {
      if (global->hasInitializer()) {
        return getAggregateElement(global->getInitializer(), index);
      }
    }
  }
  return nullptr;
}

static const Value *getAggregateElement(const Value *value,
                                        ArrayRef<unsigned> path) {
  const Value *current = value;
  for (unsigned index : path) {
    current = getAggregateElement(current, index);
    if (!current) {
      return nullptr;
    }
  }
  return current;
}

static SmallVector<unsigned, 4> getConstantGEPPath(const Value *pointer,
                                                   const Value *&base) {
  SmallVector<unsigned, 4> path;
  const Value *current = pointer;
  while (const auto *cast = dyn_cast_or_null<Operator>(current)) {
    if (!Instruction::isCast(cast->getOpcode())) {
      break;
    }
    current = cast->getOperand(0);
  }
  if (const auto *gep = dyn_cast_or_null<GEPOperator>(current)) {
    base = gep->getPointerOperand()->stripPointerCasts();
    for (const Value *index : gep->indices()) {
      const auto *constant = dyn_cast<ConstantInt>(index);
      if (!constant || constant->getValue().getActiveBits() > 32) {
        path.clear();
        base = nullptr;
        return path;
      }
      path.push_back(static_cast<unsigned>(constant->getZExtValue()));
    }
    while (path.size() > 1 && path.front() == 0) {
      path.erase(path.begin());
    }
    return path;
  }
  base = current;
  return path;
}

static const Value *recoverStoredValue(const Value *pointer,
                                       ArrayRef<unsigned> requested_path,
                                       const Instruction *before) {
  if (!pointer || !before || !before->getFunction()) {
    return nullptr;
  }
  const Value *requested_base = nullptr;
  SmallVector<unsigned, 4> prefix = getConstantGEPPath(pointer, requested_base);
  if (!requested_base) {
    return nullptr;
  }
  prefix.append(requested_path.begin(), requested_path.end());
  while (prefix.size() > 1 && prefix.front() == 0) {
    prefix.erase(prefix.begin());
  }
  DominatorTree dom_tree(*const_cast<Function *>(before->getFunction()));
  const StoreInst *best = nullptr;
  for (const Instruction &inst : instructions(*before->getFunction())) {
    const auto *store = dyn_cast<StoreInst>(&inst);
    if (!store || !dom_tree.dominates(store, before)) {
      continue;
    }
    if (store->getParent() == before->getParent() &&
        !store->comesBefore(before)) {
      continue;
    }
    const Value *store_base = nullptr;
    SmallVector<unsigned, 4> store_path =
        getConstantGEPPath(store->getPointerOperand(), store_base);
    if (store_base != requested_base || store_path != prefix) {
      continue;
    }
    if (!best || dom_tree.dominates(best, store)) {
      best = store;
    }
  }
  return best ? best->getValueOperand() : nullptr;
}

static const Value *getAggregateElementAt(const Value *value,
                                          ArrayRef<unsigned> path,
                                          const Instruction *before) {
  if (const Value *element = getAggregateElement(value, path)) {
    return element;
  }
  return recoverStoredValue(value, path, before);
}

static const Value *recoverLaunchArgument(const Value *argument_array,
                                          unsigned index,
                                          const Instruction *launch) {
  if (!argument_array || !launch) {
    return nullptr;
  }
  const Value *array = argument_array;
  while (const auto *cast = dyn_cast<Operator>(array)) {
    if (!Instruction::isCast(cast->getOpcode())) {
      break;
    }
    array = cast->getOperand(0);
  }
  const Value *aggregate = array;
  if (const auto *gep = dyn_cast<GEPOperator>(array)) {
    if (const auto *global = dyn_cast<GlobalVariable>(
            gep->getPointerOperand()->stripPointerCasts())) {
      if (global->hasInitializer()) {
        aggregate = global->getInitializer();
      }
    }
  }
  if (const auto *global = dyn_cast<GlobalVariable>(array)) {
    if (!global->hasInitializer()) {
      return nullptr;
    }
    aggregate = global->getInitializer();
  }
  const Value *actual = nullptr;
  if (isa<GlobalVariable>(aggregate) ||
      aggregate->getType()->isAggregateType()) {
    actual = getAggregateElementAt(aggregate, {index}, launch);
  } else if (index == 0) {
    actual = recoverStoredValue(argument_array, {}, launch);
  } else {
    actual = recoverStoredValue(argument_array, {index}, launch);
  }
  if (!actual) {
    return nullptr;
  }
  actual = actual->stripPointerCasts();
  if (const auto *slot = dyn_cast<GlobalVariable>(actual)) {
    if (slot->hasInitializer() &&
        slot->getInitializer()->getType()->isPointerTy()) {
      actual = slot->getInitializer()->stripPointerCasts();
    }
  }
  if (isa<AllocaInst>(actual) || isa<GEPOperator>(actual)) {
    if (const Value *stored = recoverStoredValue(actual, {}, launch)) {
      actual = stored->stripPointerCasts();
    }
  }
  return actual;
}

static void setUnitDimensions(std::array<SymbolicDimension, 3> &dimensions) {
  for (SymbolicDimension &dimension : dimensions) {
    dimension.kind = SymbolicValueKind::Constant;
    dimension.constant = 1;
    dimension.value = nullptr;
  }
}

static bool decodeDim3(const Value *value,
                       std::array<SymbolicDimension, 3> &dimensions,
                       const Instruction *before = nullptr) {
  if (!value) {
    return false;
  }
  if (!value->getType()->isAggregateType()) {
    return false;
  }
  bool decoded_any = false;
  for (unsigned index = 0; index < 3; ++index) {
    const Value *component = before
                                 ? getAggregateElementAt(value, {index}, before)
                                 : getAggregateElement(value, index);
    if (!component) {
      continue;
    }
    dimensions[index] = CUDASymbolicModel::classifyDimension(component);
    decoded_any = true;
  }
  return decoded_any;
}

static DecodedLaunch decodeLaunch(const CallBase *call) {
  DecodedLaunch decoded;
  if (!call || call->arg_empty()) {
    return decoded;
  }
  const Function *entry = call->getCalledFunction();
  if (!entry) {
    return decoded;
  }
  const StringRef name = normalizeLaunchEntryName(entry->getName());

  auto setKernel = [&](unsigned index) {
    if (index < call->arg_size()) {
      decoded.kernel = resolveFunctionFromValue(call->getArgOperand(index));
    }
  };
  auto setScalarDimensions = [&](unsigned grid_base, unsigned block_base) {
    for (unsigned index = 0; index < 3; ++index) {
      if (grid_base + index < call->arg_size()) {
        decoded.dimensions.grid[index] = CUDASymbolicModel::classifyDimension(
            call->getArgOperand(grid_base + index));
      }
      if (block_base + index < call->arg_size()) {
        decoded.dimensions.block[index] = CUDASymbolicModel::classifyDimension(
            call->getArgOperand(block_base + index));
      }
    }
  };

  if (name == "cudaLaunchKernel" || name == "cudaLaunchCooperativeKernel") {
    setKernel(0);
    if (call->arg_size() >= 6 &&
        call->getArgOperand(1)->getType()->isAggregateType() &&
        call->getArgOperand(2)->getType()->isAggregateType()) {
      decodeDim3(call->getArgOperand(1), decoded.dimensions.grid, call);
      decodeDim3(call->getArgOperand(2), decoded.dimensions.block, call);
      decoded.argument_array = call->getArgOperand(3);
      decoded.dynamic_shared_memory = call->getArgOperand(4);
      decoded.stream = call->getArgOperand(5);
      decoded.recognized_layout = true;
      return decoded;
    }

    // Preserve the historical Lotus scalar fixture layout while keeping it
    // separate from the production CUDA ABI.
    if (call->arg_size() >= 9) {
      setUnitDimensions(decoded.dimensions.grid);
      setUnitDimensions(decoded.dimensions.block);
      decoded.dimensions.grid[0] =
          CUDASymbolicModel::classifyDimension(call->getArgOperand(1));
      decoded.dimensions.block[0] =
          CUDASymbolicModel::classifyDimension(call->getArgOperand(2));
      decoded.dimensions.grid[1] =
          CUDASymbolicModel::classifyDimension(call->getArgOperand(3));
      decoded.dimensions.block[1] =
          CUDASymbolicModel::classifyDimension(call->getArgOperand(4));
      decoded.dimensions.grid[2] =
          CUDASymbolicModel::classifyDimension(call->getArgOperand(5));
      decoded.argument_array = call->getArgOperand(6);
      decoded.dynamic_shared_memory = call->getArgOperand(7);
      decoded.stream = call->getArgOperand(8);
      decoded.recognized_layout = true;
    }
    return decoded;
  }

  if (name == "cudaLaunchKernelExC" || name == "cuLaunchKernelEx") {
    setKernel(1);
    decoded.argument_array =
        call->arg_size() > 2 ? call->getArgOperand(2) : nullptr;
    const Value *config = call->getArgOperand(0)->stripPointerCasts();
    bool decoded_config = false;
    if (const auto *global = dyn_cast<GlobalVariable>(config)) {
      if (global->hasInitializer()) {
        const Value *initializer = global->getInitializer();
        for (unsigned index = 0; index < 3; ++index) {
          if (const Value *grid =
                  getAggregateElement(initializer, {0u, index})) {
            decoded.dimensions.grid[index] =
                CUDASymbolicModel::classifyDimension(grid);
          }
          if (const Value *block =
                  getAggregateElement(initializer, {1u, index})) {
            decoded.dimensions.block[index] =
                CUDASymbolicModel::classifyDimension(block);
          }
        }
        decoded.dynamic_shared_memory = getAggregateElement(initializer, 2u);
        decoded.stream = getAggregateElement(initializer, 3u);
        decoded.recognized_layout = true;
        decoded_config = true;
      }
    }
    if (!decoded_config) {
      for (unsigned index = 0; index < 3; ++index) {
        if (const Value *grid =
                getAggregateElementAt(config, {0u, index}, call)) {
          decoded.dimensions.grid[index] =
              CUDASymbolicModel::classifyDimension(grid);
          decoded_config = true;
        }
        if (const Value *block =
                getAggregateElementAt(config, {1u, index}, call)) {
          decoded.dimensions.block[index] =
              CUDASymbolicModel::classifyDimension(block);
          decoded_config = true;
        }
      }
      decoded.dynamic_shared_memory = getAggregateElementAt(config, {2u}, call);
      decoded.stream = getAggregateElementAt(config, {3u}, call);
      decoded.recognized_layout = decoded_config;
    }
    return decoded;
  }

  if (name == "cuLaunchKernel" || name == "cuLaunchCooperativeKernel") {
    setKernel(0);
    if (call->arg_size() >= 10) {
      setScalarDimensions(1, 4);
      decoded.dynamic_shared_memory = call->getArgOperand(7);
      decoded.stream = call->getArgOperand(8);
      decoded.argument_array = call->getArgOperand(9);
      decoded.recognized_layout = true;
    }
    return decoded;
  }

  return decoded;
}

const Function *getKernelFromRuntimeLaunch(const CallBase *call) {
  return decodeLaunch(call).kernel;
}

const Value *getStreamOperand(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return nullptr;
  }
  const Function *callee = call->getCalledFunction();
  if (!callee) {
    return nullptr;
  }
  StringRef name = callee->getName();
  if ((name.contains("Memcpy") || name.contains("Memset")) &&
      name.contains("Async")) {
    return call->arg_size() >= 4 ? call->getArgOperand(call->arg_size() - 1)
                                 : nullptr;
  }
  if (name.contains("cudaMemPrefetchAsync")) {
    return call->arg_size() >= 4 ? call->getArgOperand(3) : nullptr;
  }
  if (name.contains("StreamCreate")) {
    return nullptr;
  }
  if (name.contains("StreamWaitEvent")) {
    return call->getArgOperand(0);
  }
  if (name.contains("Stream")) {
    return call->getArgOperand(0);
  }
  if (name.contains("EventRecord")) {
    return call->arg_size() >= 2 ? call->getArgOperand(1) : nullptr;
  }
  if (name.contains("GraphLaunch")) {
    return call->arg_size() >= 2 ? call->getArgOperand(1) : nullptr;
  }
  if (CUDAModel::isKernelLaunch(normalizeLaunchEntryName(name))) {
    return decodeLaunch(call).stream;
  }
  return nullptr;
}

const Value *getEventOperand(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return nullptr;
  }
  const Function *callee = call->getCalledFunction();
  if (!callee) {
    return nullptr;
  }
  StringRef name = callee->getName();
  if (name.contains("StreamWaitEvent")) {
    return call->arg_size() >= 2 ? call->getArgOperand(1) : nullptr;
  }
  if (name.contains("Event")) {
    return call->getArgOperand(0);
  }
  return nullptr;
}

const Value *getPotentialStream(const CallBase *call) {
  if (!call || call->arg_empty()) {
    return nullptr;
  }
  if (const Value *stream = getStreamOperand(call)) {
    return stream;
  }
  return nullptr;
}

static std::optional<HostStreamKind>
classifyCreatedStreamHandle(const Value *stream) {
  if (const auto *create = dyn_cast_or_null<CallBase>(stream)) {
    const Function *callee = create->getCalledFunction();
    if (callee && callee->getName().contains("StreamCreate")) {
      if (create->arg_size() > 1) {
        if (auto flags = CUDASymbolicModel::evaluateConstantInt(
                create->getArgOperand(1))) {
          return (*flags & 1) != 0 ? HostStreamKind::NonBlockingExplicit
                                   : HostStreamKind::BlockingExplicit;
        }
        return HostStreamKind::Unknown;
      }
      return HostStreamKind::BlockingExplicit;
    }
  }
  const auto *load = dyn_cast_or_null<LoadInst>(stream);
  if (!load) {
    return std::nullopt;
  }
  const Value *slot = load->getPointerOperand()->stripPointerCasts();
  for (const User *user : slot->users()) {
    const auto *call = dyn_cast<CallBase>(user);
    if (!call || call->arg_empty() ||
        call->getArgOperand(0)->stripPointerCasts() != slot) {
      continue;
    }
    const Function *callee = call->getCalledFunction();
    if (!callee) {
      continue;
    }
    const StringRef name = callee->getName();
    if (!name.contains("StreamCreate")) {
      continue;
    }
    if (call->arg_size() > 1) {
      if (auto flags =
              CUDASymbolicModel::evaluateConstantInt(call->getArgOperand(1))) {
        return (*flags & 1) != 0 ? HostStreamKind::NonBlockingExplicit
                                 : HostStreamKind::BlockingExplicit;
      }
      return HostStreamKind::Unknown;
    }
    return HostStreamKind::Unknown;
  }
  return std::nullopt;
}

HostStreamKind classifyHostStream(const CallBase *call, const Value *stream) {
  if (!call) {
    return HostStreamKind::Unknown;
  }
  const Function *callee = call->getCalledFunction();
  const StringRef api_name = callee ? callee->getName() : StringRef{};
  const bool per_thread_entry =
      api_name.endswith("_ptsz") || api_name.endswith("_ptds");
  if (!stream) {
    if (isCUDAKernelCandidate(callee) ||
        CUDAModel::isLegacyKernelConfiguration(api_name)) {
      return per_thread_entry ? HostStreamKind::PerThreadDefault
                              : HostStreamKind::LegacyDefault;
    }
    return HostStreamKind::Unknown;
  }
  if (auto created_kind = classifyCreatedStreamHandle(stream)) {
    return *created_kind;
  }
  if (const auto *constant = dyn_cast<Constant>(stream)) {
    if (constant->isNullValue()) {
      return per_thread_entry ? HostStreamKind::PerThreadDefault
                              : HostStreamKind::LegacyDefault;
    }
    if (const auto *expr = dyn_cast<ConstantExpr>(constant)) {
      if (expr->getOpcode() == Instruction::IntToPtr) {
        if (const auto *integer = dyn_cast<ConstantInt>(expr->getOperand(0))) {
          if (integer->equalsInt(1)) {
            return HostStreamKind::LegacyDefault;
          }
          if (integer->equalsInt(2)) {
            return HostStreamKind::PerThreadDefault;
          }
        }
      }
    }
    return HostStreamKind::BlockingExplicit;
  }
  const Value *base = stream->stripPointerCasts();
  if (const auto *int_to_ptr = dyn_cast<IntToPtrInst>(base)) {
    if (const auto *ci = dyn_cast<ConstantInt>(int_to_ptr->getOperand(0))) {
      if (ci->isZero()) {
        return per_thread_entry ? HostStreamKind::PerThreadDefault
                                : HostStreamKind::LegacyDefault;
      }
      if (ci->equalsInt(1)) {
        return HostStreamKind::LegacyDefault;
      }
      if (ci->equalsInt(2)) {
        return HostStreamKind::PerThreadDefault;
      }
      return HostStreamKind::Unknown;
    }
  }
  return HostStreamKind::Unknown;
}

LaunchOrderingSource getOrderingSource(ThreadAPI::TD_TYPE type) {
  switch (type) {
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
    return LaunchOrderingSource::DeviceSynchronize;
  case ThreadAPI::TD_CUDA_STREAM:
    return LaunchOrderingSource::StreamSynchronize;
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
    return LaunchOrderingSource::MemoryBarrier;
  default:
    return LaunchOrderingSource::Unknown;
  }
}

SynchronizationScope getSyncScope(ThreadAPI::TD_TYPE type) {
  switch (type) {
  case ThreadAPI::TD_CUDA_WARP_BARRIER:
    return SynchronizationScope::Warp;
  case ThreadAPI::TD_CUDA_BARRIER:
    return SynchronizationScope::Block;
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
    return SynchronizationScope::Device;
  case ThreadAPI::TD_CUDA_STREAM:
  case ThreadAPI::TD_CUDA_EVENT:
    return SynchronizationScope::Device;
  default:
    return SynchronizationScope::None;
  }
}

SynchronizationPrimitive getSynchronizationPrimitive(ThreadAPI::TD_TYPE type,
                                                     const Instruction *inst) {
  switch (type) {
  case ThreadAPI::TD_CUDA_WARP_BARRIER:
    return SynchronizationPrimitive::WarpBarrier;
  case ThreadAPI::TD_CUDA_BARRIER:
    return SynchronizationPrimitive::BlockBarrier;
  case ThreadAPI::TD_CUDA_DEVICE_SYNC:
    return SynchronizationPrimitive::DeviceSynchronize;
  case ThreadAPI::TD_CUDA_STREAM:
  case ThreadAPI::TD_CUDA_EVENT:
    return SynchronizationPrimitive::StreamProgramOrder;
  case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
    if (const auto *call = dyn_cast_or_null<CallBase>(inst)) {
      if (const Function *callee = call->getCalledFunction()) {
        StringRef name = callee->getName();
        if (name.contains("threadfence_block") || name.contains("membar.cta")) {
          return SynchronizationPrimitive::BlockFence;
        }
        if (name.contains("threadfence_system") ||
            name.contains("membar.sys")) {
          return SynchronizationPrimitive::SystemFence;
        }
      }
    }
    return SynchronizationPrimitive::DeviceFence;
  default:
    return SynchronizationPrimitive::None;
  }
}

bool launchesOrdered(const std::vector<KernelLaunchInfo> &launches,
                     size_t earlier_idx, size_t later_idx) {
  if (earlier_idx >= later_idx || later_idx >= launches.size()) {
    return false;
  }

  const KernelLaunchInfo &earlier = launches[earlier_idx];
  const KernelLaunchInfo &later = launches[later_idx];
  if (!earlier.host_function || earlier.host_function != later.host_function) {
    return false;
  }
  if (llvm::is_contained(later.ordered_dependencies, earlier_idx)) {
    return true;
  }
  return false;
}

static bool isExplicitStreamKind(HostStreamKind kind) {
  return kind == HostStreamKind::BlockingExplicit ||
         kind == HostStreamKind::NonBlockingExplicit;
}

static bool sameStreamQueue(HostStreamKind lhs_kind, const Value *lhs_stream,
                            HostStreamKind rhs_kind, const Value *rhs_stream) {
  if (lhs_stream && rhs_stream && lhs_stream == rhs_stream) {
    return true;
  }
  if (lhs_kind != rhs_kind) {
    return false;
  }
  if (lhs_kind == HostStreamKind::LegacyDefault ||
      lhs_kind == HostStreamKind::PerThreadDefault) {
    return true;
  }
  return isExplicitStreamKind(lhs_kind) && lhs_stream &&
         lhs_stream == rhs_stream;
}

static bool hasLegacyImplicitOrdering(HostStreamKind lhs, HostStreamKind rhs) {
  const bool lhs_legacy = lhs == HostStreamKind::LegacyDefault;
  const bool rhs_legacy = rhs == HostStreamKind::LegacyDefault;
  const bool lhs_blocking = lhs == HostStreamKind::BlockingExplicit;
  const bool rhs_blocking = rhs == HostStreamKind::BlockingExplicit;
  const bool lhs_ptds = lhs == HostStreamKind::PerThreadDefault;
  const bool rhs_ptds = rhs == HostStreamKind::PerThreadDefault;
  return (lhs_legacy && (rhs_blocking || rhs_ptds)) ||
         (rhs_legacy && (lhs_blocking || lhs_ptds));
}

static bool mustExecuteBefore(const Instruction *from, const Instruction *to,
                              const DominatorTree &dom_tree) {
  if (!from || !to || from->getFunction() != to->getFunction()) {
    return false;
  }
  if (from->getParent() == to->getParent()) {
    return from->comesBefore(to);
  }
  if (!dom_tree.dominates(from, to)) {
    return false;
  }
  // A back-edge from the later site to the earlier site means different loop
  // iterations can reverse the static-instance order.
  return !llvm::isPotentiallyReachable(to->getParent(), from->getParent());
}

static const Value *canonicalizeHandle(const Value *handle,
                                       const Instruction *use,
                                       StringRef create_name) {
  const auto *load = dyn_cast_or_null<LoadInst>(handle);
  if (!load || !use || load->getFunction() != use->getFunction()) {
    return handle;
  }
  const Value *slot = load->getPointerOperand()->stripPointerCasts();
  DominatorTree dom_tree(*const_cast<Function *>(use->getFunction()));
  const CallBase *best = nullptr;
  for (const User *user : slot->users()) {
    const auto *create = dyn_cast<CallBase>(user);
    const Function *callee = create ? create->getCalledFunction() : nullptr;
    if (!create || !callee || create->arg_empty() ||
        !callee->getName().contains(create_name) ||
        create->getArgOperand(0)->stripPointerCasts() != slot ||
        !mustExecuteBefore(create, load, dom_tree)) {
      continue;
    }
    if (!best || mustExecuteBefore(best, create, dom_tree)) {
      best = create;
    }
  }
  return best ? static_cast<const Value *>(best) : slot;
}

static const Value *canonicalizeStreamHandle(const Value *stream,
                                             const Instruction *use) {
  return canonicalizeHandle(stream, use, "StreamCreate");
}

static const Value *canonicalizeEventHandle(const Value *event,
                                            const Instruction *use) {
  return canonicalizeHandle(event, use, "EventCreate");
}

static bool boundaryApplies(const LaunchOrderingState::StreamState *state,
                            const Instruction *launch,
                            const DominatorTree &dom_tree) {
  return state && state->ordered_since_last_launch && state->boundary_inst &&
         mustExecuteBefore(state->boundary_inst, launch, dom_tree);
}

detail::LaunchOrderingState::StreamState *
getMutableStreamState(detail::LaunchOrderingState &ordering_state,
                      HostStreamKind stream_kind, const Value *stream) {
  if (stream && (isExplicitStreamKind(stream_kind) ||
                 stream_kind == HostStreamKind::Unknown)) {
    return &ordering_state.stream_states[stream];
  }
  if (stream_kind == HostStreamKind::LegacyDefault) {
    return &ordering_state.default_stream;
  }
  if (stream_kind == HostStreamKind::PerThreadDefault) {
    return &ordering_state.per_thread_default_stream;
  }
  return nullptr;
}

static detail::LaunchOrderingState::StreamState &
getHostState(detail::LaunchOrderingState &ordering_state,
             HostStreamKind stream_kind, const Value *stream) {
  if (auto *stream_state =
          getMutableStreamState(ordering_state, stream_kind, stream)) {
    return *stream_state;
  }
  return ordering_state.host_state;
}

const detail::LaunchOrderingState::StreamState *
getStreamState(const detail::LaunchOrderingState &ordering_state,
               HostStreamKind stream_kind, const Value *stream) {
  if (stream && (isExplicitStreamKind(stream_kind) ||
                 stream_kind == HostStreamKind::Unknown)) {
    auto it = ordering_state.stream_states.find(stream);
    return it == ordering_state.stream_states.end() ? nullptr : &it->second;
  }
  if (stream_kind == HostStreamKind::LegacyDefault) {
    return &ordering_state.default_stream;
  }
  if (stream_kind == HostStreamKind::PerThreadDefault) {
    return &ordering_state.per_thread_default_stream;
  }
  return nullptr;
}

void markStreamOrdered(detail::LaunchOrderingState &ordering_state,
                       HostStreamKind stream_kind, const Value *stream,
                       SynchronizationScope scope, LaunchOrderingSource source,
                       SynchronizationPrimitive primitive,
                       const Instruction *boundary_inst) {
  auto &stream_state = getHostState(ordering_state, stream_kind, stream);
  stream_state.ordered_since_last_launch = true;
  stream_state.usable_for_unknown_launch =
      stream_kind != HostStreamKind::Unknown;
  stream_state.scope = scope;
  stream_state.source = source;
  stream_state.primitive = primitive;
  stream_state.stream = stream;
  stream_state.boundary_inst = boundary_inst;
  stream_state.stream_kind = stream_kind;
}

void clearStreamOrdered(detail::LaunchOrderingState &ordering_state,
                        HostStreamKind stream_kind, const Value *stream) {
  auto &stream_state = getHostState(ordering_state, stream_kind, stream);
  stream_state.ordered_since_last_launch = false;
  stream_state.usable_for_unknown_launch = false;
  stream_state.scope = SynchronizationScope::None;
  stream_state.source = LaunchOrderingSource::None;
  stream_state.primitive = SynchronizationPrimitive::None;
  stream_state.stream = nullptr;
  stream_state.boundary_inst = nullptr;
  stream_state.stream_kind = HostStreamKind::Unknown;
  stream_state.ordered_dependencies.clear();
}

} // namespace detail

bool LaunchDimensions::hasSymbolicGrid() const {
  return llvm::any_of(grid, [](const SymbolicDimension &dim) {
    return dim.kind != SymbolicValueKind::Constant;
  });
}

bool LaunchDimensions::hasSymbolicBlock() const {
  return llvm::any_of(block, [](const SymbolicDimension &dim) {
    return dim.kind != SymbolicValueKind::Constant;
  });
}

CUDAAnalysis::CUDAAnalysis(Module &module,
                           lotus::AliasAnalysisWrapper *alias_analysis,
                           DeviceConfig config)
    : m_module(module), m_thread_api(ThreadAPI::getThreadAPI()),
      m_alias_analysis(alias_analysis), m_device_config(config),
      m_cuda_enabled(m_thread_api->getConfig().enable_cuda()) {}

CUDAAnalysis::CUDAAnalysis(Module &module, DeviceConfig config)
    : CUDAAnalysis(module, nullptr, config) {}

namespace {

uint64_t computeModuleStructuralFingerprint(const llvm::Module &module) {
  auto mix = [](uint64_t h, uint64_t v) -> uint64_t {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  };
  auto mixStr = [&](uint64_t h, llvm::StringRef s) -> uint64_t {
    h = mix(h, s.size());
    for (unsigned char c : s) {
      h = h * 1315423911ULL + c;
    }
    return h;
  };

  uint64_t h = 0xcbf29ce484222325ULL;
  h = mixStr(h, module.getModuleIdentifier());
  h = mixStr(h, module.getTargetTriple());
  h = mix(h, module.global_size());
  for (const llvm::GlobalVariable &gv : module.globals()) {
    h = mixStr(h, gv.getName());
    h = mix(h, gv.getAddressSpace());
    h = mix(h, gv.hasInitializer() ? 1u : 0u);
  }
  h = mix(h, module.size());
  for (const llvm::Function &fn : module) {
    h = mixStr(h, fn.getName());
    h = mix(h, fn.isDeclaration() ? 0u : 1u);
    h = mix(h, fn.arg_size());
    h = mix(h, fn.size());
    for (const llvm::BasicBlock &bb : fn) {
      h = mix(h, bb.size());
      for (const llvm::Instruction &inst : bb) {
        h = mix(h, inst.getOpcode());
        h = mix(h, inst.getNumOperands());
        if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst)) {
          if (const llvm::Function *callee = cb->getCalledFunction()) {
            h = mixStr(h, callee->getName());
          }
        }
      }
    }
  }
  return h;
}

bool moduleMayContainCUDA(const llvm::Module &module,
                          const ThreadAPI *thread_api) {
  if (module.getNamedMetadata("nvvm.annotations") != nullptr) {
    return true;
  }
  if (llvm::StringRef(module.getTargetTriple()).contains("nvptx")) {
    return true;
  }
  for (const llvm::GlobalVariable &gv : module.globals()) {
    const unsigned as = gv.getAddressSpace();
    if (as == 1 || as == 3 || as == 4 || as == 101) {
      return true;
    }
    const llvm::StringRef name = gv.getName();
    if (name.contains("cuda") || name.contains("CUDA") ||
        name.contains("nvvm")) {
      return true;
    }
  }
  for (const llvm::Function &fn : module) {
    if (detail::isNVVMKernel(&fn)) {
      return true;
    }
    const llvm::StringRef name = fn.getName();
    if (CUDAModel::isLegacyKernelConfiguration(name) ||
        name.contains("GraphLaunch") || name.contains("cuda") ||
        name.contains("CUDA") || name.contains("nvvm") ||
        name.contains("__device_stub__")) {
      return true;
    }
    if (thread_api) {
      const ThreadAPI::TD_TYPE type = thread_api->getType(&fn);
      if (type != ThreadAPI::TD_DUMMY &&
          thread_api->getRuntimeLibrary(&fn) ==
              ThreadAPI::RuntimeLibrary::CUDA) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

void CUDAAnalysis::runAnalysis() {
  m_has_completed_analysis = false;
  m_launches.clear();
  m_kernel_summaries.clear();
  m_launch_context_index.clear();
  m_inter_kernel_races.clear();
  m_memory_transfers.clear();
  m_unified_memory.clear();
  m_abstract_state.clear();
  m_operation_count = 0;
  m_device_sync_count = 0;
  m_barrier_count = 0;
  m_warp_barrier_count = 0;
  m_memory_barrier_count = 0;
  m_module_fingerprint = 0;
  m_has_module_fingerprint = false;
  m_cuda_enabled = m_thread_api->getConfig().enable_cuda();
  if (!m_cuda_enabled) {
    m_module_fingerprint = computeModuleStructuralFingerprint(m_module);
    m_has_module_fingerprint = true;
    m_has_completed_analysis = true;
    return;
  }
  if (!moduleMayContainCUDA(m_module, m_thread_api)) {
    m_module_fingerprint = computeModuleStructuralFingerprint(m_module);
    m_has_module_fingerprint = true;
    m_has_completed_analysis = true;
    return;
  }
  if (!m_alias_analysis) {
    initializeDefaultAliasAnalysis();
  }
  size_t launch_sequence = 0;
  CUDASteamAutomatonBuilder automaton_builder(m_abstract_state);

  for (Function &function : m_module) {
    if (function.isDeclaration()) {
      continue;
    }

    detail::LaunchOrderingState ordering_state;
    DominatorTree dom_tree(function);
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      const Function *callee = m_thread_api->getCallee(call);
      ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
      const Function *direct_callee = call->getCalledFunction();
      const StringRef direct_name =
          direct_callee ? direct_callee->getName() : StringRef{};
      const bool is_graph_launch = direct_name.contains("GraphLaunch");
      const bool is_multi_device_launch =
          type == ThreadAPI::TD_CUDA_MULTI_DEVICE_LAUNCH;
      if (type != ThreadAPI::TD_DUMMY &&
          m_thread_api->getRuntimeLibrary(call) ==
              ThreadAPI::RuntimeLibrary::CUDA) {
        ++m_operation_count;
        switch (type) {
        case ThreadAPI::TD_CUDA_DEVICE_SYNC:
          ++m_device_sync_count;
          break;
        case ThreadAPI::TD_CUDA_BARRIER:
          ++m_barrier_count;
          break;
        case ThreadAPI::TD_CUDA_WARP_BARRIER:
          ++m_warp_barrier_count;
          break;
        case ThreadAPI::TD_CUDA_MEMORY_BARRIER:
          ++m_memory_barrier_count;
          break;
        default:
          break;
        }
      }

      if (type == ThreadAPI::TD_CUDA_DEVICE_SYNC) {
        ordering_state.device_synchronized = true;
        auto &host_state = ordering_state.host_state;
        host_state.ordered_dependencies.clear();
        for (size_t dep = 0; dep < m_launches.size(); ++dep) {
          const KernelLaunchInfo &prior = m_launches[dep];
          if (prior.host_function == &function && prior.launch &&
              detail::mustExecuteBefore(prior.launch, &inst, dom_tree)) {
            detail::addOrderedDependency(host_state.ordered_dependencies, dep);
          }
        }
        detail::markStreamOrdered(ordering_state, HostStreamKind::Unknown,
                                  nullptr, SynchronizationScope::Device,
                                  LaunchOrderingSource::DeviceSynchronize,
                                  SynchronizationPrimitive::DeviceSynchronize,
                                  &inst);
        ordering_state.host_state.usable_for_unknown_launch = true;
        automaton_builder.addDeviceSync(&inst);
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_STREAM && !is_graph_launch) {
        const Value *stream = detail::canonicalizeStreamHandle(
            detail::getStreamOperand(call), &inst);
        const HostStreamKind stream_kind =
            detail::classifyHostStream(call, stream);
        const Value *event = detail::canonicalizeEventHandle(
            detail::getEventOperand(call), &inst);
        const Function *called_fn = call->getCalledFunction();
        StringRef name = called_fn ? called_fn->getName() : StringRef{};
        if (name.contains("Create")) {
          const Value *output_slot =
              call->arg_empty() ? nullptr : call->getArgOperand(0);
          automaton_builder.addStreamCreate(&inst, output_slot);
        } else if (name.contains("Destroy")) {
          automaton_builder.addStreamDestroy(&inst, stream);
        } else if (name.contains("Synchronize")) {
          if (stream_kind == HostStreamKind::Unknown) {
            recordModelGap(&inst,
                           "CUDA stream synchronization references an "
                           "unknown stream kind; ordering is limited "
                           "to the canonical handle identity",
                           0.35);
          }
          auto &host_state =
              detail::getHostState(ordering_state, stream_kind, stream);
          host_state.ordered_dependencies.clear();
          for (size_t dep = 0; dep < m_launches.size(); ++dep) {
            const KernelLaunchInfo &prior = m_launches[dep];
            if (prior.host_function == &function && prior.launch &&
                detail::sameStreamQueue(prior.stream_kind, prior.stream,
                                        stream_kind, stream) &&
                detail::mustExecuteBefore(prior.launch, &inst, dom_tree)) {
              detail::addOrderedDependency(host_state.ordered_dependencies,
                                           dep);
            }
          }
          detail::markStreamOrdered(
              ordering_state, stream_kind, stream, SynchronizationScope::Device,
              LaunchOrderingSource::StreamSynchronize,
              SynchronizationPrimitive::StreamProgramOrder, &inst);
          ordering_state.host_state = host_state;
          ordering_state.host_state.usable_for_unknown_launch = true;
          if (stream) {
            automaton_builder.addStreamSync(&inst, stream);
          }
        } else if (name.contains("WaitEvent")) {
          automaton_builder.addEventWait(&inst, event, stream);
          auto event_it = ordering_state.event_states.find(event);
          if (event_it != ordering_state.event_states.end() &&
              event_it->second.has_record && event_it->second.record_inst &&
              detail::mustExecuteBefore(event_it->second.record_inst, &inst,
                                        dom_tree) &&
              stream) {
            auto &host_state =
                detail::getHostState(ordering_state, stream_kind, stream);
            host_state.ordered_dependencies.clear();
            detail::addOrderedDependencies(
                host_state.ordered_dependencies,
                event_it->second.recorded_dependencies);
            detail::markStreamOrdered(
                ordering_state, stream_kind, stream,
                SynchronizationScope::Device,
                LaunchOrderingSource::StreamSynchronize,
                SynchronizationPrimitive::StreamProgramOrder, &inst);
          }
        }
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_EVENT) {
        const Value *event = detail::canonicalizeEventHandle(
            detail::getEventOperand(call), &inst);
        const Value *stream = detail::canonicalizeStreamHandle(
            detail::getStreamOperand(call), &inst);
        const HostStreamKind stream_kind =
            detail::classifyHostStream(call, stream);
        const Function *called_fn = call->getCalledFunction();
        StringRef name = called_fn ? called_fn->getName() : StringRef{};
        if (name.contains("Create")) {
          const Value *output_slot =
              call->arg_empty() ? nullptr : call->getArgOperand(0);
          automaton_builder.addEventCreate(&inst, output_slot);
        } else if (name.contains("Destroy")) {
          automaton_builder.addEventDestroy(&inst, event);
        } else if (name.contains("Record")) {
          if (event) {
            auto &event_state = ordering_state.event_states[event];
            event_state.has_record = true;
            event_state.recorded_stream = stream;
            event_state.recorded_stream_kind = stream_kind;
            event_state.record_inst = &inst;
            event_state.recorded_dependencies.clear();
            for (size_t dep = 0; dep < m_launches.size(); ++dep) {
              const KernelLaunchInfo &prior = m_launches[dep];
              if (prior.host_function == &function && prior.launch &&
                  detail::sameStreamQueue(prior.stream_kind, prior.stream,
                                          stream_kind, stream) &&
                  detail::mustExecuteBefore(prior.launch, &inst, dom_tree)) {
                detail::addOrderedDependency(event_state.recorded_dependencies,
                                             dep);
              }
            }
          }
          automaton_builder.addEvent(&inst, event, stream);
        } else if (name.contains("Wait")) {
          automaton_builder.addEventWait(&inst, event, stream);
          auto event_it = ordering_state.event_states.find(event);
          if (event_it != ordering_state.event_states.end() &&
              event_it->second.has_record && event_it->second.record_inst &&
              detail::mustExecuteBefore(event_it->second.record_inst, &inst,
                                        dom_tree) &&
              stream) {
            auto &host_state =
                detail::getHostState(ordering_state, stream_kind, stream);
            host_state.ordered_dependencies.clear();
            detail::addOrderedDependencies(
                host_state.ordered_dependencies,
                event_it->second.recorded_dependencies);
            detail::markStreamOrdered(
                ordering_state, stream_kind, stream,
                SynchronizationScope::Device,
                LaunchOrderingSource::StreamSynchronize,
                SynchronizationPrimitive::StreamProgramOrder, &inst);
          }
        } else if (name.contains("Synchronize")) {
          if (event) {
            auto event_it = ordering_state.event_states.find(event);
            if (event_it != ordering_state.event_states.end() &&
                event_it->second.has_record && event_it->second.record_inst &&
                detail::mustExecuteBefore(event_it->second.record_inst, &inst,
                                          dom_tree)) {
              ordering_state.host_state.ordered_dependencies.clear();
              detail::addOrderedDependencies(
                  ordering_state.host_state.ordered_dependencies,
                  event_it->second.recorded_dependencies);
              detail::markStreamOrdered(
                  ordering_state, HostStreamKind::Unknown, nullptr,
                  SynchronizationScope::Device,
                  LaunchOrderingSource::StreamSynchronize,
                  SynchronizationPrimitive::StreamProgramOrder, &inst);
              ordering_state.host_state.usable_for_unknown_launch = true;
            }
          }
          automaton_builder.addEventSync(&inst, event);
        }
        continue;
      }

      if (type == ThreadAPI::TD_CUDA_MEMCPY ||
          type == ThreadAPI::TD_CUDA_MEMSET ||
          type == ThreadAPI::TD_CUDA_UNIFIED_MEMORY) {
        const Function *called_fn = call->getCalledFunction();
        if (called_fn && called_fn->getName().contains("Async")) {
          const Value *stream = detail::canonicalizeStreamHandle(
              detail::getStreamOperand(call), &inst);
          if (stream) {
            automaton_builder.addStreamOperation(&inst, stream);
          } else {
            automaton_builder.addStreamOperation(&inst, nullptr);
          }
          const HostStreamKind stream_kind =
              detail::classifyHostStream(call, stream);
          if (stream_kind != HostStreamKind::Unknown) {
            detail::markStreamOrdered(
                ordering_state, stream_kind, stream,
                SynchronizationScope::Device,
                LaunchOrderingSource::ProgramOrder,
                SynchronizationPrimitive::StreamProgramOrder, &inst);
          }
        }
      }

      if (type != ThreadAPI::TD_CUDA_KERNEL_LAUNCH && !is_graph_launch &&
          !is_multi_device_launch) {
        continue;
      }

      if (detail::isLegacyConfigureCall(call)) {
        continue;
      }

      const detail::DecodedLaunch decoded = detail::decodeLaunch(call);
      const Function *kernel = decoded.kernel;
      if (!kernel) {
        kernel = m_thread_api->getCUDALaunchedKernel(&inst);
      }
      const bool opaque_launch = !detail::isCUDAKernelCandidate(kernel);
      if (opaque_launch) {
        recordModelGap(&inst,
                       "CUDA launch site could not be matched to a "
                       "concrete kernel function; retaining an opaque "
                       "launch with unknown device-memory effects",
                       0.35);
      }

      if (!decoded.recognized_layout && call->getCalledFunction() &&
          CUDAModel::isKernelLaunch(detail::normalizeLaunchEntryName(
              call->getCalledFunction()->getName()))) {
        recordModelGap(&inst,
                       "CUDA launch API layout is unsupported; launch "
                       "dimensions, stream, arguments, and dynamic "
                       "shared memory remain unknown",
                       0.25);
      }

      const Value *stream = detail::canonicalizeStreamHandle(
          decoded.stream ? decoded.stream : detail::getStreamOperand(call),
          &inst);
      const HostStreamKind stream_kind =
          detail::classifyHostStream(call, stream);
      const bool stream_known = stream != nullptr ||
                                stream_kind == HostStreamKind::LegacyDefault ||
                                stream_kind == HostStreamKind::PerThreadDefault;
      const auto *stream_state =
          detail::getStreamState(ordering_state, stream_kind, stream);
      const detail::LaunchOrderingState::StreamState *selected_state = nullptr;
      if (stream_known &&
          detail::boundaryApplies(stream_state, &inst, dom_tree)) {
        selected_state = stream_state;
      } else if (ordering_state.host_state.usable_for_unknown_launch &&
                 detail::boundaryApplies(&ordering_state.host_state, &inst,
                                         dom_tree)) {
        selected_state = &ordering_state.host_state;
      }
      KernelLaunchInfo launch;
      launch.launch = &inst;
      launch.host_function = &function;
      launch.dimensions = decoded.recognized_layout
                              ? decoded.dimensions
                              : getLaunchDimensions(&inst);
      launch.argument_array = decoded.argument_array;
      launch.dynamic_shared_memory = decoded.dynamic_shared_memory;
      launch.dynamic_shared_memory_size =
          CUDASymbolicModel::classifyDimension(decoded.dynamic_shared_memory);
      launch.sequence = launch_sequence++;
      launch.kernel = kernel;
      launch.stream = stream;
      launch.stream_known = stream_known;
      launch.stream_kind = stream_kind;
      launch.predecessor = SynchronizationPrimitive::None;
      launch.host_happens_before = false;
      launch.is_opaque = opaque_launch;
      launch.has_unknown_memory_effect = opaque_launch;
      if (launch.kernel) {
        for (const Argument &argument : launch.kernel->args()) {
          launch.argument_values.push_back(detail::recoverLaunchArgument(
              launch.argument_array, argument.getArgNo(), launch.launch));
        }
      }

      if (selected_state &&
          (!selected_state->ordered_dependencies.empty() ||
           selected_state->source == LaunchOrderingSource::ProgramOrder)) {
        if (selected_state == &ordering_state.host_state && !stream_known) {
          launch.stream = selected_state->stream;
          launch.stream_kind = selected_state->stream_kind;
          launch.stream_known =
              selected_state->stream_kind != HostStreamKind::Unknown;
        }
        launch.ordered_after_previous = true;
        launch.ordered_dependencies = selected_state->ordered_dependencies;
        launch.ordering_scope = selected_state->scope;
        launch.ordering_source = selected_state->source;
        launch.predecessor = selected_state->primitive;
        launch.host_happens_before = true;
      }

      for (size_t dep = 0; dep < m_launches.size(); ++dep) {
        const KernelLaunchInfo &prior = m_launches[dep];
        if (prior.host_function != &function || !prior.launch ||
            !detail::mustExecuteBefore(prior.launch, &inst, dom_tree)) {
          continue;
        }
        if (detail::sameStreamQueue(prior.stream_kind, prior.stream,
                                    launch.stream_kind, launch.stream) ||
            detail::hasLegacyImplicitOrdering(prior.stream_kind,
                                              launch.stream_kind)) {
          detail::addOrderedDependency(launch.ordered_dependencies, dep);
        }
      }
      if (!launch.ordered_dependencies.empty() &&
          !launch.ordered_after_previous) {
        launch.ordered_after_previous = true;
        launch.ordering_scope = SynchronizationScope::Device;
        launch.ordering_source = LaunchOrderingSource::ProgramOrder;
        launch.predecessor = SynchronizationPrimitive::StreamProgramOrder;
        launch.host_happens_before = true;
      }

      m_launches.push_back(launch);
      automaton_builder.addStreamOperation(&inst, stream);
      LaunchContextKey key;
      key.kernel = launch.kernel;
      key.launch_site = launch.launch;
      key.dimensions = launch.dimensions;
      key.argument_array = launch.argument_array;
      key.dynamic_shared_memory = launch.dynamic_shared_memory;
      if (launch.kernel && !m_launch_context_index.count(key)) {
        analyzeKernel(launch.kernel, &m_launches.back());
        m_launch_context_index[key] = m_kernel_summaries.size() - 1;
      }
      if (launch.dimensions.hasSymbolicGrid() ||
          launch.dimensions.hasSymbolicBlock()) {
        recordModelGap(&inst,
                       "CUDA launch dimensions remain symbolic, "
                       "reducing precision for race and performance "
                       "diagnostics",
                       0.5);
      }
      detail::clearStreamOrdered(ordering_state, stream_kind, stream);
      ordering_state.device_synchronized = false;
    }
  }

  automaton_builder.finalize();

  for (Function &function : m_module) {
    if (function.isDeclaration() || !detail::isNVVMKernel(&function)) {
      continue;
    }
    LaunchContextKey key;
    key.kernel = &function;
    key.dimensions = LaunchDimensions();
    if (m_launch_context_index.count(key)) {
      continue;
    }
    recordModelGap(&function,
                   "Kernel analyzed without an explicit host-side "
                   "launch context; launch dimensions defaulted "
                   "conservatively",
                   0.55);
    analyzeKernel(&function, nullptr);
  }

  analyzeMemoryTransfers();
  analyzeUnifiedMemory();
  analyzeInterKernelRaces();

  CUDAFunctionSummaryAnalysis func_summary_analysis(m_module);
  func_summary_analysis.runAnalysis();

  for (const auto &pair : func_summary_analysis.getSummaries()) {
    m_abstract_state.function_summaries[pair.first] = pair.second;
  }

  size_t kernel_class_counter = 0;
  std::set<const llvm::Function *> analyzed_kernels;
  for (size_t i = 0; i < m_kernel_summaries.size(); ++i) {
    const llvm::Function *kernel = m_kernel_summaries[i].kernel;
    if (!kernel)
      continue;
    analyzed_kernels.insert(kernel);
    CUDAParticipantAnalysis participant_analysis(*kernel);

    for (const auto &bb : *kernel) {
      for (const auto &inst : bb) {
        if (llvm::isa<llvm::CallBase>(&inst)) {
          auto set = participant_analysis.getActiveSetAt(&inst);
          if (!set.scopes.empty()) {
            set.kernel = kernel;
            set.instruction = &inst;
            m_abstract_state.participant_sets.push_back(set);
          }
        }
      }
    }

    CUDAKernelProtocolAnalysis protocol_analysis(*kernel, m_abstract_state);
    protocol_analysis.runAnalysis();
  }

  for (Function &function : m_module) {
    for (const Instruction &inst : instructions(function)) {
      const auto *call = dyn_cast<CallBase>(&inst);
      if (!call) {
        continue;
      }
      auto type = m_thread_api->getType(call);
      if (type == ThreadAPI::TD_CUDA_DEVICE_SYNC) {
        CUDASynchronizationFact fact;
        fact.synchronization_class_id =
            m_abstract_state.synchronization_facts.size();
        fact.inst = call;
        fact.primitive =
            static_cast<int>(SynchronizationPrimitive::DeviceSynchronize);
        fact.scope = static_cast<int>(SynchronizationScope::Device);
        fact.ordering_effect = true;
        fact.participating_threads = 0;
        m_abstract_state.synchronization_facts.push_back(fact);
        m_abstract_state
            .synchronization_fact_by_class[fact.synchronization_class_id] =
            fact;
      }
    }
  }

  for (const auto &launch : m_launches) {
    CUDAKernelFact fact;
    fact.kernel_class_id = kernel_class_counter++;
    fact.kernel = launch.kernel;
    fact.launch_site = launch.launch;
    fact.stream = launch.stream;
    fact.stream_known = launch.stream_known;
    fact.is_ordered_after_previous = launch.ordered_after_previous;
    m_abstract_state.kernel_facts.push_back(fact);
    m_abstract_state.kernel_fact_by_class[fact.kernel_class_id] = fact;
  }
  m_module_fingerprint = computeModuleStructuralFingerprint(m_module);
  m_has_module_fingerprint = true;
  m_has_completed_analysis = true;
}

bool CUDAAnalysis::hasCurrentModuleSnapshot() const {
  if (!m_has_completed_analysis || !m_has_module_fingerprint) {
    return false;
  }
  return computeModuleStructuralFingerprint(m_module) == m_module_fingerprint;
}

void CUDAAnalysis::analyzeInterKernelRaces() {
  if (m_launches.size() < 2 && m_memory_transfers.empty()) {
    return;
  }

  struct TransferRegion {
    const MemoryTransferInfo *transfer = nullptr;
    AccessInfo access;
  };
  SmallVector<TransferRegion, 16> transfer_regions;
  SmallVector<const MemoryTransferInfo *, 8> opaque_transfers;
  auto add_transfer_region = [&](const MemoryTransferInfo &transfer,
                                 const Value *pointer, MemorySpace space,
                                 bool is_write) {
    if (!transfer.is_async || !pointer ||
        (space != MemorySpace::Device && space != MemorySpace::Global &&
         space != MemorySpace::Constant)) {
      return;
    }
    TransferRegion region;
    region.transfer = &transfer;
    region.access.inst = transfer.inst;
    region.access.pointer = pointer;
    const BaseObjectInfo base_info =
        CUDAMemoryModel::getBaseObjectInfo(pointer);
    region.access.base = base_info.primary();
    region.access.base_objects = base_info.objects;
    region.access.has_ambiguous_base = base_info.ambiguous;
    region.access.space = space;
    region.access.is_write = is_write;
    region.access.access_size = static_cast<uint32_t>(std::min<uint64_t>(
        transfer.size, std::numeric_limits<uint32_t>::max()));
    region.access.alias_precision =
        base_info.ambiguous ? AliasPrecision::Ambiguous : AliasPrecision::Exact;
    if (region.access.base) {
      transfer_regions.push_back(std::move(region));
    }
  };
  for (const MemoryTransferInfo &transfer : m_memory_transfers) {
    if (transfer.is_async &&
        (transfer.region_unknown || (!transfer.src && !transfer.dst))) {
      opaque_transfers.push_back(&transfer);
    }
    add_transfer_region(transfer, transfer.src, transfer.src_space, false);
    add_transfer_region(transfer, transfer.dst, transfer.dst_space, true);
  }

  auto operations_ordered = [&](const Instruction *first,
                                HostStreamKind first_kind,
                                const Value *first_stream,
                                const Instruction *second,
                                HostStreamKind second_kind,
                                const Value *second_stream) {
    if (!first || !second || first->getFunction() != second->getFunction()) {
      return false;
    }
    DominatorTree dom_tree(*const_cast<Function *>(first->getFunction()));
    if (detail::mustExecuteBefore(second, first, dom_tree)) {
      std::swap(first, second);
      std::swap(first_kind, second_kind);
      std::swap(first_stream, second_stream);
    }
    if (!detail::mustExecuteBefore(first, second, dom_tree)) {
      return false;
    }
    first_stream = detail::canonicalizeStreamHandle(first_stream, first);
    second_stream = detail::canonicalizeStreamHandle(second_stream, second);
    if (auto kind = detail::classifyCreatedStreamHandle(first_stream)) {
      first_kind = *kind;
    }
    if (auto kind = detail::classifyCreatedStreamHandle(second_stream)) {
      second_kind = *kind;
    }
    if (detail::sameStreamQueue(first_kind, first_stream, second_kind,
                                second_stream) ||
        detail::hasLegacyImplicitOrdering(first_kind, second_kind)) {
      return true;
    }

    DenseMap<const Value *, const CallBase *> records;
    for (const Instruction &candidate : instructions(*first->getFunction())) {
      const auto *call = dyn_cast<CallBase>(&candidate);
      if (!call || !detail::mustExecuteBefore(first, call, dom_tree) ||
          !detail::mustExecuteBefore(call, second, dom_tree)) {
        continue;
      }
      const ThreadAPI::TD_TYPE type = m_thread_api->getType(call);
      const Function *callee = call->getCalledFunction();
      const StringRef name = callee ? callee->getName() : StringRef{};
      if (type == ThreadAPI::TD_CUDA_DEVICE_SYNC) {
        return true;
      }
      if (name.contains("StreamSynchronize")) {
        const Value *synced = detail::canonicalizeStreamHandle(
            detail::getStreamOperand(call), call);
        if (detail::sameStreamQueue(first_kind, first_stream,
                                    detail::classifyHostStream(call, synced),
                                    synced)) {
          return true;
        }
      }
      if (name.contains("EventRecord")) {
        const Value *event = detail::canonicalizeEventHandle(
            detail::getEventOperand(call), call);
        const Value *record_stream = detail::canonicalizeStreamHandle(
            detail::getStreamOperand(call), call);
        if (detail::sameStreamQueue(
                first_kind, first_stream,
                detail::classifyHostStream(call, record_stream),
                record_stream)) {
          records[event] = call;
        }
      } else if (name.contains("StreamWaitEvent")) {
        const Value *event = detail::canonicalizeEventHandle(
            detail::getEventOperand(call), call);
        const Value *wait_stream = detail::canonicalizeStreamHandle(
            detail::getStreamOperand(call), call);
        if (records.count(event) &&
            detail::sameStreamQueue(
                second_kind, second_stream,
                detail::classifyHostStream(call, wait_stream), wait_stream)) {
          return true;
        }
      } else if (name.contains("EventSynchronize")) {
        const Value *event = detail::canonicalizeEventHandle(
            detail::getEventOperand(call), call);
        if (records.count(event)) {
          return true;
        }
      }
    }
    return false;
  };

  auto get_launch_argument = [&](const KernelLaunchInfo &launch,
                                 unsigned index) -> const Value * {
    if (index < launch.argument_values.size() &&
        launch.argument_values[index]) {
      return launch.argument_values[index];
    }
    return detail::recoverLaunchArgument(launch.argument_array, index,
                                         launch.launch);
  };

  auto instantiate_access = [&](const AccessInfo &access,
                                const KernelLaunchInfo &launch) {
    AccessInfo instantiated = access;
    const auto *formal = dyn_cast_or_null<Argument>(access.base);
    if (!formal || formal->getParent() != launch.kernel) {
      return instantiated;
    }
    const Value *actual = get_launch_argument(launch, formal->getArgNo());
    if (!actual) {
      instantiated.has_ambiguous_base = true;
      instantiated.alias_precision = AliasPrecision::Ambiguous;
      return instantiated;
    }
    const BaseObjectInfo base_info = CUDAMemoryModel::getBaseObjectInfo(actual);
    instantiated.pointer = actual;
    instantiated.base = base_info.primary();
    instantiated.base_objects = base_info.objects;
    instantiated.has_ambiguous_base = base_info.ambiguous;
    const MemorySpaceInfo space_info = CUDAMemoryModel::classify(actual);
    if (space_info.space != MemorySpace::Unknown) {
      instantiated.space = space_info.space;
    }
    instantiated.alias_precision =
        base_info.ambiguous ? AliasPrecision::Ambiguous : AliasPrecision::Exact;
    return instantiated;
  };

  auto query_region_alias = [&](const AccessInfo &lhs, const AccessInfo &rhs) {
    detail::AliasQueryResult alias =
        detail::queryAlias(lhs, rhs, m_alias_analysis);
    if (alias.relation == AliasResult::NoAlias &&
        (isa<Argument>(lhs.base) || isa<Argument>(rhs.base))) {
      alias.relation = AliasResult::MayAlias;
      alias.precision = AliasPrecision::Ambiguous;
      alias.source = AliasSource::Local;
    }
    return alias;
  };

  for (const auto &func : m_module) {
    if (!func.isDeclaration() && detail::isNVVMKernel(&func)) {
      recordModelGap(
          &func,
          "Inter-kernel race analysis requires "
          "launch-context indexing for accurate summary lookup; "
          "conservative over-approximation may report false positives",
          0.3);
      break;
    }
  }
  for (size_t i = 0; i < m_launches.size(); ++i) {
    for (size_t j = i + 1; j < m_launches.size(); ++j) {
      const KernelLaunchInfo &launch_a = m_launches[i];
      const KernelLaunchInfo &launch_b = m_launches[j];

      if (!launch_a.kernel || !launch_b.kernel) {
        if (!detail::launchesOrdered(m_launches, i, j)) {
          InterKernelRaceInfo race;
          race.first_launch = launch_a.launch;
          race.second_launch = launch_b.launch;
          race.first_kernel = launch_a.kernel;
          race.second_kernel = launch_b.kernel;
          race.ordered = false;
          race.ordering_reason =
              "unordered opaque launch has unknown device-memory effects";
          race.symbolic = true;
          race.kind = RaceKind::InterKernelHazard;
          race.alias_precision = AliasPrecision::Ambiguous;
          race.required_fence_scope = SynchronizationScope::Device;
          race.confidence = 0.3;
          m_inter_kernel_races.push_back(std::move(race));
        }
        continue;
      }

      LaunchContextKey key_a;
      key_a.kernel = launch_a.kernel;
      key_a.launch_site = launch_a.launch;
      key_a.dimensions = launch_a.dimensions;
      key_a.argument_array = launch_a.argument_array;
      key_a.dynamic_shared_memory = launch_a.dynamic_shared_memory;
      LaunchContextKey key_b;
      key_b.kernel = launch_b.kernel;
      key_b.launch_site = launch_b.launch;
      key_b.dimensions = launch_b.dimensions;
      key_b.argument_array = launch_b.argument_array;
      key_b.dynamic_shared_memory = launch_b.dynamic_shared_memory;

      if (!m_launch_context_index.count(key_a) ||
          !m_launch_context_index.count(key_b)) {
        continue;
      }

      size_t idx_a = m_launch_context_index[key_a];
      size_t idx_b = m_launch_context_index[key_b];
      const KernelSummary &summary_a = m_kernel_summaries[idx_a];
      const KernelSummary &summary_b = m_kernel_summaries[idx_b];
      bool ordered = detail::launchesOrdered(m_launches, i, j);

      for (const AccessInfo &access_a : summary_a.accesses) {
        const AccessInfo instantiated_a =
            instantiate_access(access_a, launch_a);
        if (!instantiated_a.base) {
          continue;
        }
        if (instantiated_a.space != MemorySpace::Global &&
            instantiated_a.space != MemorySpace::Device) {
          continue;
        }

        for (const AccessInfo &access_b : summary_b.accesses) {
          const AccessInfo instantiated_b =
              instantiate_access(access_b, launch_b);
          if (!instantiated_b.base) {
            continue;
          }
          if (!instantiated_a.is_write && !instantiated_b.is_write) {
            continue;
          }
          if (instantiated_b.space != instantiated_a.space &&
              !((instantiated_a.space == MemorySpace::Global ||
                 instantiated_a.space == MemorySpace::Device) &&
                (instantiated_b.space == MemorySpace::Global ||
                 instantiated_b.space == MemorySpace::Device))) {
            continue;
          }
          const detail::AliasQueryResult alias =
              query_region_alias(instantiated_a, instantiated_b);
          if (alias.relation == AliasResult::NoAlias) {
            continue;
          }
          if (instantiated_a.is_atomic && instantiated_b.is_atomic) {
            continue;
          }

          InterKernelRaceInfo race;
          race.first_launch = launch_a.launch;
          race.second_launch = launch_b.launch;
          race.first_kernel = launch_a.kernel;
          race.second_kernel = launch_b.kernel;
          race.shared_base = instantiated_a.base;
          race.ordered = ordered;
          race.ordering_reason =
              race.ordered ? toString(launch_b.ordering_source) : "unordered";
          race.ordering_inst =
              launch_b.ordering_source != LaunchOrderingSource::None
                  ? launch_b.launch
                  : nullptr;
          race.ordering_source = launch_b.ordering_source;
          race.stream = launch_b.stream;
          race.stream_known = launch_b.stream_known;
          race.symbolic = launch_a.dimensions.hasSymbolicGrid() ||
                          launch_a.dimensions.hasSymbolicBlock() ||
                          launch_b.dimensions.hasSymbolicGrid() ||
                          launch_b.dimensions.hasSymbolicBlock();
          race.kind = (instantiated_a.is_atomic || instantiated_b.is_atomic)
                          ? RaceKind::AtomicOrderingRisk
                          : RaceKind::InterKernelHazard;
          race.alias_precision = alias.precision;
          race.alias_source = alias.source;
          race.missing_ordering = launch_b.predecessor;
          race.required_fence_scope = SynchronizationScope::Device;
          race.confidence =
              race.alias_precision == AliasPrecision::Exact ? 0.9 : 0.6;
          if (race.ordered) {
            continue;
          }
          m_inter_kernel_races.push_back(std::move(race));
        }
      }
    }
  }

  for (const KernelLaunchInfo &launch : m_launches) {
    LaunchContextKey key;
    key.kernel = launch.kernel;
    key.launch_site = launch.launch;
    key.dimensions = launch.dimensions;
    key.argument_array = launch.argument_array;
    key.dynamic_shared_memory = launch.dynamic_shared_memory;
    auto summary_it = m_launch_context_index.find(key);
    if (!launch.kernel || summary_it == m_launch_context_index.end()) {
      if (launch.has_unknown_memory_effect) {
        for (const TransferRegion &region : transfer_regions) {
          const auto *transfer_call =
              dyn_cast_or_null<CallBase>(region.transfer->inst);
          const HostStreamKind transfer_kind = detail::classifyHostStream(
              transfer_call, region.transfer->stream);
          if (operations_ordered(launch.launch, launch.stream_kind,
                                 launch.stream, region.transfer->inst,
                                 transfer_kind, region.transfer->stream)) {
            continue;
          }
          InterKernelRaceInfo race;
          race.first_launch = launch.launch;
          race.second_transfer = region.transfer->inst;
          race.ordered = false;
          race.ordering_reason =
              "opaque launch may conflict with asynchronous transfer";
          race.symbolic = true;
          race.kind = RaceKind::InterKernelHazard;
          race.alias_precision = AliasPrecision::Ambiguous;
          race.required_fence_scope = SynchronizationScope::Device;
          race.confidence = 0.25;
          m_inter_kernel_races.push_back(std::move(race));
        }
      }
      continue;
    }
    const KernelSummary &summary = m_kernel_summaries[summary_it->second];
    for (const AccessInfo &kernel_access : summary.accesses) {
      const AccessInfo instantiated_access =
          instantiate_access(kernel_access, launch);
      if (!instantiated_access.base ||
          (instantiated_access.space != MemorySpace::Global &&
           instantiated_access.space != MemorySpace::Device)) {
        continue;
      }
      for (const TransferRegion &region : transfer_regions) {
        if (!instantiated_access.is_write && !region.access.is_write) {
          continue;
        }
        const detail::AliasQueryResult alias =
            query_region_alias(instantiated_access, region.access);
        if (alias.relation == AliasResult::NoAlias) {
          continue;
        }
        const auto *transfer_call =
            dyn_cast_or_null<CallBase>(region.transfer->inst);
        const HostStreamKind transfer_kind =
            detail::classifyHostStream(transfer_call, region.transfer->stream);
        if (operations_ordered(launch.launch, launch.stream_kind, launch.stream,
                               region.transfer->inst, transfer_kind,
                               region.transfer->stream)) {
          continue;
        }
        InterKernelRaceInfo race;
        race.first_launch = launch.launch;
        race.first_kernel = launch.kernel;
        race.second_transfer = region.transfer->inst;
        race.shared_base = instantiated_access.base;
        race.ordered = false;
        race.ordering_reason = "unordered kernel and asynchronous transfer";
        race.stream = region.transfer->stream;
        race.stream_known = region.transfer->stream_known;
        race.kind = RaceKind::InterKernelHazard;
        race.alias_precision = alias.precision;
        race.alias_source = alias.source;
        race.required_fence_scope = SynchronizationScope::Device;
        race.confidence =
            alias.precision == AliasPrecision::Exact ? 0.85 : 0.55;
        m_inter_kernel_races.push_back(std::move(race));
      }
    }
  }

  for (const KernelLaunchInfo &launch : m_launches) {
    for (const MemoryTransferInfo *transfer : opaque_transfers) {
      const auto *transfer_call = dyn_cast_or_null<CallBase>(transfer->inst);
      const HostStreamKind transfer_kind =
          detail::classifyHostStream(transfer_call, transfer->stream);
      if (operations_ordered(launch.launch, launch.stream_kind, launch.stream,
                             transfer->inst, transfer_kind, transfer->stream)) {
        continue;
      }
      InterKernelRaceInfo race;
      race.first_launch = launch.launch;
      race.first_kernel = launch.kernel;
      race.second_transfer = transfer->inst;
      race.ordered = false;
      race.ordering_reason =
          "opaque asynchronous transfer may overlap device memory";
      race.symbolic = true;
      race.kind = RaceKind::InterKernelHazard;
      race.alias_precision = AliasPrecision::Ambiguous;
      race.required_fence_scope = SynchronizationScope::Device;
      race.confidence = 0.25;
      m_inter_kernel_races.push_back(std::move(race));
    }
  }

  for (size_t i = 0; i < transfer_regions.size(); ++i) {
    for (size_t j = i + 1; j < transfer_regions.size(); ++j) {
      const TransferRegion &lhs = transfer_regions[i];
      const TransferRegion &rhs = transfer_regions[j];
      if (lhs.transfer == rhs.transfer ||
          (!lhs.access.is_write && !rhs.access.is_write)) {
        continue;
      }
      const detail::AliasQueryResult alias =
          query_region_alias(lhs.access, rhs.access);
      if (alias.relation == AliasResult::NoAlias) {
        continue;
      }
      const auto *lhs_call = dyn_cast_or_null<CallBase>(lhs.transfer->inst);
      const auto *rhs_call = dyn_cast_or_null<CallBase>(rhs.transfer->inst);
      const HostStreamKind lhs_kind =
          detail::classifyHostStream(lhs_call, lhs.transfer->stream);
      const HostStreamKind rhs_kind =
          detail::classifyHostStream(rhs_call, rhs.transfer->stream);
      if (operations_ordered(lhs.transfer->inst, lhs_kind, lhs.transfer->stream,
                             rhs.transfer->inst, rhs_kind,
                             rhs.transfer->stream)) {
        continue;
      }
      InterKernelRaceInfo race;
      race.first_transfer = lhs.transfer->inst;
      race.second_transfer = rhs.transfer->inst;
      race.shared_base = lhs.access.base;
      race.ordered = false;
      race.ordering_reason = "unordered asynchronous transfers";
      race.stream = rhs.transfer->stream;
      race.stream_known = rhs.transfer->stream_known;
      race.kind = RaceKind::InterKernelHazard;
      race.alias_precision = alias.precision;
      race.alias_source = alias.source;
      race.required_fence_scope = SynchronizationScope::Device;
      race.confidence = alias.precision == AliasPrecision::Exact ? 0.85 : 0.55;
      m_inter_kernel_races.push_back(std::move(race));
    }
  }

  for (size_t i = 0; i < opaque_transfers.size(); ++i) {
    for (size_t j = i + 1; j < opaque_transfers.size(); ++j) {
      const MemoryTransferInfo *lhs = opaque_transfers[i];
      const MemoryTransferInfo *rhs = opaque_transfers[j];
      const auto *lhs_call = dyn_cast_or_null<CallBase>(lhs->inst);
      const auto *rhs_call = dyn_cast_or_null<CallBase>(rhs->inst);
      const HostStreamKind lhs_kind =
          detail::classifyHostStream(lhs_call, lhs->stream);
      const HostStreamKind rhs_kind =
          detail::classifyHostStream(rhs_call, rhs->stream);
      if (operations_ordered(lhs->inst, lhs_kind, lhs->stream, rhs->inst,
                             rhs_kind, rhs->stream)) {
        continue;
      }
      InterKernelRaceInfo race;
      race.first_transfer = lhs->inst;
      race.second_transfer = rhs->inst;
      race.ordering_reason = "unordered opaque asynchronous transfers";
      race.symbolic = true;
      race.kind = RaceKind::InterKernelHazard;
      race.alias_precision = AliasPrecision::Ambiguous;
      race.required_fence_scope = SynchronizationScope::Device;
      race.confidence = 0.2;
      m_inter_kernel_races.push_back(std::move(race));
    }
  }
}

LaunchDimensions CUDAAnalysis::getLaunchDimensions(const Instruction *launch) {
  LaunchDimensions dims;
  const auto *call = dyn_cast_or_null<CallBase>(launch);
  if (!call) {
    return dims;
  }

  const detail::DecodedLaunch decoded = detail::decodeLaunch(call);
  if (decoded.recognized_layout) {
    return decoded.dimensions;
  }

  const Function *callee = call->getCalledFunction();
  if (!callee || !CUDAModel::isLegacyKernelConfiguration(callee->getName())) {
    return dims;
  }

  detail::setUnitDimensions(dims.grid);
  detail::setUnitDimensions(dims.block);
  const unsigned dim_base = 0;

  if (call->arg_size() > dim_base + 0) {
    dims.grid[0] = classifyDimension(call->getArgOperand(dim_base + 0));
  }
  if (call->arg_size() > dim_base + 1) {
    dims.block[0] = classifyDimension(call->getArgOperand(dim_base + 1));
  }
  if (call->arg_size() > dim_base + 2) {
    dims.grid[1] = classifyDimension(call->getArgOperand(dim_base + 2));
  }
  if (call->arg_size() > dim_base + 3) {
    dims.block[1] = classifyDimension(call->getArgOperand(dim_base + 3));
  }
  if (call->arg_size() > dim_base + 4) {
    dims.grid[2] = classifyDimension(call->getArgOperand(dim_base + 4));
  }
  if (call->arg_size() > dim_base + 5) {
    dims.block[2] = classifyDimension(call->getArgOperand(dim_base + 5));
  }

  return dims;
}

void CUDAAnalysis::initializeDefaultAliasAnalysis() {
  m_owned_alias_analysis =
      lotus::AliasAnalysisFactory::createAserPTA(m_module, 0);
  if (m_owned_alias_analysis && m_owned_alias_analysis->isInitialized()) {
    m_alias_analysis = m_owned_alias_analysis.get();
    return;
  }
  m_owned_alias_analysis =
      lotus::AliasAnalysisFactory::createSparrowAA(m_module, 0);
  if (m_owned_alias_analysis && m_owned_alias_analysis->isInitialized()) {
    m_alias_analysis = m_owned_alias_analysis.get();
    return;
  }
  m_owned_alias_analysis.reset();
  m_alias_analysis = nullptr;
}

bool CUDAAnalysis::hasAliasAnalysis() const {
  return m_alias_analysis && m_alias_analysis->isInitialized();
}

} // namespace concurrency::cuda
