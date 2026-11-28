#pragma once

#include <llvm/ADT/StringRef.h>

#include <initializer_list>

namespace CUDAModel {

struct KernelLaunchLayout {
  static constexpr unsigned NoArgument = ~0u;
  unsigned kernel_arg = NoArgument;
  unsigned payload_arg = NoArgument;

  bool isValid() const { return kernel_arg != NoArgument; }
};

inline llvm::StringRef normalizeLaunchName(llvm::StringRef funcName) {
  if (funcName.endswith("_v2"))
    return funcName.drop_back(3);
  return funcName;
}

inline KernelLaunchLayout
getKernelLaunchLayout(const llvm::StringRef &rawName) {
  const llvm::StringRef funcName = normalizeLaunchName(rawName);
  if (funcName.equals("cudaLaunchKernelExC"))
    return {1, 2};
  if (funcName.equals("cuLaunchKernelEx"))
    return {1, 2};
  if (funcName.equals("cudaLaunchKernel") ||
      funcName.equals("cudaLaunchCooperativeKernel"))
    return {0, 3};
  if (funcName.equals("cuLaunchKernel") ||
      funcName.equals("cuLaunchCooperativeKernel"))
    return {0, 9};
  return {};
}

inline bool isAggregateKernelLaunch(const llvm::StringRef &rawName) {
  const llvm::StringRef funcName = normalizeLaunchName(rawName);
  return funcName.equals("cudaLaunchCooperativeKernelMultiDevice") ||
         funcName.equals("cuLaunchCooperativeKernelMultiDevice");
}

inline bool hasAnySubstring(const llvm::StringRef &funcName,
                            std::initializer_list<llvm::StringRef> needles) {
  for (const llvm::StringRef needle : needles) {
    if (funcName.contains(needle)) {
      return true;
    }
  }
  return false;
}

inline bool startsWithAny(const llvm::StringRef &funcName,
                          std::initializer_list<llvm::StringRef> prefixes) {
  for (const llvm::StringRef prefix : prefixes) {
    if (funcName.startswith(prefix)) {
      return true;
    }
  }
  return false;
}

inline bool isKernelLaunch(const llvm::StringRef &funcName) {
  return getKernelLaunchLayout(funcName).isValid();
}

inline bool isLegacyKernelConfiguration(const llvm::StringRef &funcName) {
  return funcName.equals("__set_CUDAConfig") ||
         funcName.equals("__cudaPushCallConfiguration") ||
         funcName.equals("__cudaPopCallConfiguration") ||
         funcName.equals("cudaConfigureCall");
}

inline bool isKernelGraphOperation(const llvm::StringRef &funcName) {
  return funcName.startswith("cudaGraphAddKernelNode") ||
         funcName.startswith("cudaGraphKernelNodeSetParams") ||
         funcName.startswith("cudaGraphExecKernelNodeSetParams") ||
         funcName.equals("cudaGraphLaunch") ||
         funcName.startswith("cuGraphAddKernelNode") ||
         funcName.startswith("cuGraphKernelNodeSetParams") ||
         funcName.startswith("cuGraphExecKernelNodeSetParams") ||
         funcName.equals("cuGraphLaunch");
}

inline bool isDeviceSynchronize(const llvm::StringRef &funcName) {
  return funcName.equals("cudaDeviceSynchronize") ||
         funcName.equals("cudaThreadSynchronize") ||
         funcName.equals("cuCtxSynchronize");
}

inline bool isBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("llvm.nvvm.barrier0") ||
         funcName.equals("__syncthreads") ||
         funcName.equals("cuda.syncthreads");
}

inline bool isWarpBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("llvm.nvvm.bar.warp.sync") ||
         funcName.equals("__syncwarp");
}

inline bool isMemoryBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("llvm.nvvm.membar.cta") ||
         funcName.equals("llvm.nvvm.membar.gl") ||
         funcName.equals("llvm.nvvm.membar.sys") ||
         funcName.equals("__threadfence_block") ||
         funcName.equals("__threadfence") ||
         funcName.equals("__threadfence_system");
}

inline bool isAtomic(const llvm::StringRef &funcName) {
  return funcName.startswith("llvm.nvvm.atomic.") ||
         funcName.startswith("llvm.nvvm.red.") ||
         funcName.startswith("__nv_atomic") ||
         funcName.startswith("atomicAdd") || funcName.startswith("atomicSub") ||
         funcName.startswith("atomicExch") || funcName.startswith("atomicMin") ||
         funcName.startswith("atomicMax") || funcName.startswith("atomicInc") ||
         funcName.startswith("atomicDec") || funcName.startswith("atomicCAS") ||
         funcName.startswith("atomicAnd") || funcName.startswith("atomicOr") ||
         funcName.startswith("atomicXor");
}

inline bool isMemcpy(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMemcpy") ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddMemcpyNode",
                          "cudaGraphMemcpyNodeSetParams",
                          "cudaGraphExecMemcpyNodeSetParams"}) ||
         startsWithAny(funcName, {"cuMemcpy", "cuGraphMemcpyNode"}) ||
         funcName.contains("cudaMemPrefetchAsync") ||
         (funcName.startswith("llvm.memcpy.") &&
          (funcName.contains(".p1") || funcName.contains(".p3") ||
           funcName.contains(".p4") || funcName.contains(".p5")));
}

inline bool isMemset(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMemset") ||
         startsWithAny(funcName, {"cuMemset"}) ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddMemsetNode",
                          "cudaGraphMemsetNodeSetParams",
                          "cudaGraphExecMemsetNodeSetParams"});
}

inline bool isMalloc(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMalloc") ||
         funcName.contains("cudaMallocManaged") ||
         funcName.contains("cudaMallocHost") ||
         funcName.contains("cudaHostAlloc") ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddMemAllocNode",
                          "cudaGraphMemAllocNodeGetParams"}) ||
         startsWithAny(funcName, {"cuMemAlloc", "cuArrayCreate",
                                  "cuMipmappedArrayCreate"});
}

inline bool isFree(const llvm::StringRef &funcName) {
  return funcName.contains("cudaFree") ||
         funcName.contains("cudaFreeHost") ||
         hasAnySubstring(funcName, {"cudaGraphAddMemFreeNode"}) ||
         startsWithAny(funcName, {"cuMemFree", "cuArrayDestroy",
                                  "cuMipmappedArrayDestroy"});
}

inline bool isStreamOperation(const llvm::StringRef &funcName) {
  return funcName.contains("cudaStreamCreate") ||
         funcName.contains("cudaStreamDestroy") ||
         funcName.contains("cudaStreamSynchronize") ||
         funcName.contains("cudaStreamWaitEvent") ||
         funcName.contains("cudaStreamQuery") ||
         funcName.contains("cudaStreamBeginCapture") ||
         funcName.contains("cudaStreamEndCapture") ||
         funcName.contains("cudaStreamIsCapturing") ||
         funcName.contains("cudaThreadExchangeStreamCaptureMode") ||
         startsWithAny(funcName, {"cuStreamCreate", "cuStreamDestroy",
                                  "cuStreamSynchronize", "cuStreamWaitEvent",
                                  "cuStreamQuery", "cuStreamBeginCapture",
                                  "cuStreamEndCapture",
                                  "cuStreamIsCapturing",
                                  "cuThreadExchangeStreamCaptureMode"});
}

inline bool isEventOperation(const llvm::StringRef &funcName) {
  return funcName.contains("cudaEventCreate") ||
         funcName.contains("cudaEventDestroy") ||
         funcName.contains("cudaEventRecord") ||
         funcName.contains("cudaEventSynchronize") ||
         funcName.contains("cudaEventQuery") ||
         funcName.contains("cudaEventElapsedTime") ||
         hasAnySubstring(funcName,
                         {"cudaGraphAddEventRecordNode",
                          "cudaGraphAddEventWaitNode",
                          "cudaGraphEventRecordNodeSetEvent",
                          "cudaGraphEventWaitNodeSetEvent",
                          "cudaGraphExecEventRecordNodeSetEvent",
                          "cudaGraphExecEventWaitNodeSetEvent"}) ||
         startsWithAny(funcName, {"cuEventCreate", "cuEventDestroy",
                                  "cuEventRecord", "cuEventSynchronize",
                                  "cuEventQuery", "cuEventElapsedTime"});
}

inline bool isTexture(const llvm::StringRef &funcName) {
  return funcName.startswith("llvm.nvvm.tex") ||
         funcName.startswith("tex1D") || funcName.startswith("tex2D") ||
         funcName.startswith("tex3D") || funcName.startswith("cudaBindTexture") ||
         funcName.startswith("cudaCreateTextureObject");
}

inline bool isSurface(const llvm::StringRef &funcName) {
  return funcName.startswith("llvm.nvvm.surf") ||
         funcName.startswith("surf1D") || funcName.startswith("surf2D") ||
         funcName.startswith("surf3D") ||
         funcName.startswith("cudaBindSurface");
}

inline bool isUnifiedMemory(const llvm::StringRef &funcName) {
  return funcName.contains("cudaMallocManaged") ||
         funcName.contains("cudaMallocHost") ||
         funcName.contains("cudaHostAlloc") ||
         funcName.contains("cudaMemPrefetchAsync") ||
         funcName.contains("cudaMemPrefetchBatchAsync") ||
         funcName.contains("cudaMemAdvise") ||
         funcName.contains("cudaMemRangeGetAttribute") ||
         funcName.contains("cudaMemRangeGetAttributes") ||
         funcName.contains("cudaStreamAttachMemAsync") ||
         startsWithAny(funcName, {"cuMemAllocManaged", "cuMemPrefetchAsync",
                                  "cuMemAdvise", "cuMemRangeGetAttribute"});
}

inline bool isDeviceManagement(const llvm::StringRef &funcName) {
  return funcName.contains("cudaGetDevice") ||
         funcName.contains("cudaSetDevice") ||
         funcName.contains("cudaGetDeviceCount") ||
         funcName.contains("cudaGetDeviceProperties") ||
         funcName.contains("cudaChooseDevice") ||
         funcName.contains("cudaSetValidDevices") ||
         funcName.contains("cudaDeviceGet") ||
         funcName.contains("cudaDeviceSet") ||
         funcName.contains("cudaDeviceReset") ||
         startsWithAny(funcName, {"cuInit", "cuDeviceGet", "cuDeviceGetCount",
                                  "cuCtxCreate", "cuCtxDestroy",
                                  "cuCtxSetCurrent", "cuCtxGetCurrent",
                                  "cuCtxPushCurrent", "cuCtxPopCurrent"});
}

inline bool isErrorHandling(const llvm::StringRef &funcName) {
  return funcName.contains("cudaGetLastError") ||
         funcName.contains("cudaPeekAtLastError") ||
         funcName.contains("cudaGetErrorString") ||
         funcName.contains("cudaGetErrorName") ||
         funcName.contains("cuGetErrorName") ||
         funcName.contains("cuGetErrorString");
}

} // namespace CUDAModel
