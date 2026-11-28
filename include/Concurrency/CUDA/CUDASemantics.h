#pragma once

#include "Concurrency/API/ThreadAPI.h"

#include <llvm/IR/Instructions.h>

#include <string>

namespace concurrency::cuda {

/// Semantic effect kinds for CUDA operations
enum class CUDAEffectKind {
  Unknown,
  KernelLaunch,
  DeviceSync,
  StreamSync,
  Barrier,
  WarpBarrier,
  MemoryFence,
  Atomic,
  Memcpy,
  Memset,
  Malloc,
  Free,
  UnifiedMalloc,
  UnifiedFree,
  PrefetchAsync,
  StreamCreate,
  StreamDestroy,
  EventCreate,
  EventRecord,
  EventWait,
  EventSynchronize,
  EventDestroy
};

/// Semantic family grouping
enum class CUDASemanticFamily {
  Unknown,
  KernelLaunch,
  MemoryTransfer,
  Synchronization,
  AtomicOperation,
  MemoryManagement,
  StreamOperation,
  EventOperation
};

/// Descriptor mapping ThreadAPI types to CUDA semantic properties
struct CUDASemanticDescriptor {
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  CUDAEffectKind kind = CUDAEffectKind::Unknown;
  CUDASemanticFamily family = CUDASemanticFamily::Unknown;
  int stream_arg_idx = -1;
  int device_arg_idx = -1;
  int size_arg_idx = -1;
  int dst_arg_idx = -1;
  int src_arg_idx = -1;
};

/// Lookup semantic descriptor by ThreadAPI type
/// Returns nullptr if type is not a recognized CUDA operation
const CUDASemanticDescriptor *lookupCUDASemantic(ThreadAPI::TD_TYPE type);

/// Resolve an operation variant from the concrete CUDA API identity.
const CUDASemanticDescriptor *lookupCUDASemantic(const llvm::CallBase *call);

/// Convert effect kind to string
const char *toString(CUDAEffectKind kind);

/// Convert effect kind to a std::string (canonical string mapping)
std::string cudaEffectToString(CUDAEffectKind kind);

/// Convert family to string
const char *toString(CUDASemanticFamily family);

bool isStreamOrderingOperation(ThreadAPI::TD_TYPE type);
bool isEventOperation(ThreadAPI::TD_TYPE type);
bool isMemoryTransferOperation(ThreadAPI::TD_TYPE type);

} // namespace concurrency::cuda
