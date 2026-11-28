#pragma once

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/CUDA/CUDAAbstractState.h"
#include "Concurrency/CUDA/CUDAMemoryModel.h"
#include "Concurrency/CUDA/CUDASymbolicModel.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace concurrency::cuda {

enum class CoalescingQuality {
  Unknown,
  FullyCoalesced,
  PartiallyCoalesced,
  Uncoalesced
};

enum class SynchronizationScope { None, Warp, Block, Device, System };

enum class RaceKind {
  DataRace,
  AtomicOrderingRisk,
  MissingFence,
  InterKernelHazard
};

enum class LaunchOrderingSource {
  None,
  DeviceSynchronize,
  StreamSynchronize,
  MemoryBarrier,
  ProgramOrder,
  Unknown
};

enum class HostStreamKind {
  Unknown,
  LegacyDefault,
  PerThreadDefault,
  BlockingExplicit,
  NonBlockingExplicit,
  Explicit = BlockingExplicit
};

enum class AliasPrecision { Exact, SymbolicAffine, Ambiguous, NonAffine };

enum class AliasSource { Local, AserPTA, Wrapper };

enum class SynchronizationPrimitive {
  None,
  WarpBarrier,
  BlockBarrier,
  BlockFence,
  DeviceFence,
  SystemFence,
  DeviceSynchronize,
  StreamProgramOrder
};

enum class ParticipationKind { Exact, Conditional, Partial };

struct LaunchDimensions {
  std::array<SymbolicDimension, 3> grid{};
  std::array<SymbolicDimension, 3> block{};

  bool hasSymbolicGrid() const;
  bool hasSymbolicBlock() const;
};

struct DeviceConfig {
  uint32_t warp_size = 32;
  uint32_t shared_bank_count = 32;
  uint32_t shared_bank_width = 4;
  uint32_t global_transaction_bytes = 128;
  uint32_t sm_major = 7;
};

struct KernelLaunchInfo {
  const llvm::Instruction *launch = nullptr;
  const llvm::Function *host_function = nullptr;
  const llvm::Function *kernel = nullptr;
  LaunchDimensions dimensions;
  const llvm::Value *argument_array = nullptr;
  llvm::SmallVector<const llvm::Value *, 8> argument_values;
  const llvm::Value *dynamic_shared_memory = nullptr;
  SymbolicDimension dynamic_shared_memory_size;
  size_t sequence = 0;
  llvm::SmallVector<size_t, 4> ordered_dependencies;
  SynchronizationPrimitive predecessor = SynchronizationPrimitive::None;
  SynchronizationScope ordering_scope = SynchronizationScope::None;
  bool ordered_after_previous = false;
  LaunchOrderingSource ordering_source = LaunchOrderingSource::None;
  const llvm::Value *stream = nullptr;
  bool stream_known = false;
  HostStreamKind stream_kind = HostStreamKind::Unknown;
  bool host_happens_before = false;
  bool is_opaque = false;
  bool has_unknown_memory_effect = false;
};

struct AccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *pointer = nullptr;
  const llvm::Value *base = nullptr;
  llvm::SmallVector<const llvm::Value *, 4> base_objects;
  MemorySpace space = MemorySpace::Unknown;
  bool is_write = false;
  bool is_atomic = false;
  bool is_volatile = false;
  llvm::AtomicOrdering atomic_ordering = llvm::AtomicOrdering::NotAtomic;
  bool has_ambiguous_base = false;
  bool has_unresolved_base = false;
  bool depends_on_thread_idx = false;
  bool depends_on_block_idx = false;
  bool depends_on_lane_id = false;
  bool exact_space = false;
  uint32_t access_size = 0;
  uint32_t address_space = 0;
  UniformityClass uniformity = UniformityClass::Unknown;
  ParticipationScope participation = ParticipationScope::Unknown;
  SynchronizationScope ordering_scope = SynchronizationScope::None;
  bool exact_address = false;
  bool has_fence_relevance = false;
  bool fence_precedes = false;
  AliasPrecision alias_precision = AliasPrecision::NonAffine;
  AliasSource alias_source = AliasSource::Local;
  AffineAccessPattern address_pattern;
};

struct DivergenceRegion {
  const llvm::Instruction *branch = nullptr;
  const llvm::BasicBlock *merge_block = nullptr;
  llvm::SmallVector<const llvm::Instruction *, 4> nested_barriers;
  llvm::SmallVector<const llvm::BasicBlock *, 8> region_blocks;
  bool depends_on_thread_idx = false;
  bool depends_on_block_idx = false;
  bool depends_on_lane_id = false;
};

struct BankConflictInfo {
  const llvm::Instruction *inst = nullptr;
  uint32_t bank_count = 0;
  uint32_t bank_width = 0;
  uint32_t conflict_degree = 0;
  uint32_t threads_per_bank = 0;
  uint32_t bank_stride_bytes = 0;
  uint32_t unique_banks = 0;
  bool is_broadcast = false;
  bool exact = false;
};

struct CoalescingInfo {
  const llvm::Instruction *inst = nullptr;
  CoalescingQuality quality = CoalescingQuality::Unknown;
  uint32_t estimated_transactions = 0;
  uint32_t transaction_bytes = 0;
  uint32_t covered_bytes = 0;
  uint32_t participating_lanes = 0;
  uint32_t unique_segments = 0;
  bool exact = false;
};

struct MemoryTransferInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Function *host_function = nullptr;
  const llvm::Value *src = nullptr;
  const llvm::Value *dst = nullptr;
  const llvm::Value *stream = nullptr;
  MemorySpace src_space = MemorySpace::Unknown;
  MemorySpace dst_space = MemorySpace::Unknown;
  TransferKind kind = TransferKind::Unknown;
  bool is_async = false;
  bool stream_known = false;
  uint64_t size = 0;
  uint64_t width = 0;
  uint64_t height = 1;
  uint64_t depth = 1;
  uint64_t src_pitch = 0;
  uint64_t dst_pitch = 0;
  bool region_unknown = false;
};

struct ConstantAccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *base = nullptr;
  uint32_t access_size = 0;
  bool strided = false;
  int64_t stride = 0;
};

struct UnifiedMemoryInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *ptr = nullptr;
  const llvm::Value *stream = nullptr;
  uint64_t size = 0;
  bool is_managed = false;
  bool is_prefetch = false;
  bool is_advise = false;
  bool is_attach = false;
  int device_id = -1;
};

struct TextureAccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *texref = nullptr;
  uint32_t dimensions = 0;
  bool is_write = false;
};

struct SurfaceAccessInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *surfref = nullptr;
  uint32_t dimensions = 0;
  bool is_write = false;
};

struct RaceInfo {
  const llvm::Instruction *first = nullptr;
  const llvm::Instruction *second = nullptr;
  const llvm::Value *base = nullptr;
  llvm::SmallVector<const llvm::Value *, 4> bases;
  MemorySpace space = MemorySpace::Unknown;
  bool same_block_only = false;
  bool cross_block = false;
  bool symbolic = false;
  RaceKind kind = RaceKind::DataRace;
  SynchronizationScope scope = SynchronizationScope::None;
  const char *ordering_reason = nullptr;
  bool exact = false;
  const llvm::Instruction *ordering_inst = nullptr;
  SynchronizationScope required_fence_scope = SynchronizationScope::None;
  AliasPrecision alias_precision = AliasPrecision::NonAffine;
  AliasSource alias_source = AliasSource::Local;
  SynchronizationPrimitive missing_ordering = SynchronizationPrimitive::None;
  double confidence = 0.0;
};

struct BarrierMismatchInfo {
  const llvm::Instruction *branch = nullptr;
  const llvm::Instruction *barrier = nullptr;
};

struct WarpUniformInfo {
  const llvm::Instruction *branch = nullptr;
  bool uniform_within_warp = false;
  bool uniform_within_block = false;
  llvm::SmallVector<const llvm::BasicBlock *, 8> uniform_blocks;
};

struct SynchronizationRecord {
  const llvm::Instruction *inst = nullptr;
  SynchronizationPrimitive primitive = SynchronizationPrimitive::None;
  SynchronizationScope scope = SynchronizationScope::None;
  bool orders_memory = false;
  bool execution_rendezvous = false;
  bool orders_atomics = false;
  ParticipationScope participating_threads = ParticipationScope::Unknown;
  ParticipationKind participation = ParticipationKind::Partial;
  llvm::SmallVector<const llvm::BasicBlock *, 8> preceding_blocks;
  llvm::SmallVector<const llvm::BasicBlock *, 8> following_blocks;
  bool exact = false;
};

struct InterKernelRaceInfo {
  const llvm::Instruction *first_launch = nullptr;
  const llvm::Instruction *second_launch = nullptr;
  const llvm::Instruction *first_transfer = nullptr;
  const llvm::Instruction *second_transfer = nullptr;
  const llvm::Function *first_kernel = nullptr;
  const llvm::Function *second_kernel = nullptr;
  const llvm::Value *shared_base = nullptr;
  bool ordered = false;
  RaceKind kind = RaceKind::InterKernelHazard;
  const char *ordering_reason = nullptr;
  bool symbolic = false;
  const llvm::Instruction *ordering_inst = nullptr;
  LaunchOrderingSource ordering_source = LaunchOrderingSource::None;
  const llvm::Value *stream = nullptr;
  bool stream_known = false;
  AliasPrecision alias_precision = AliasPrecision::NonAffine;
  AliasSource alias_source = AliasSource::Local;
  SynchronizationPrimitive missing_ordering = SynchronizationPrimitive::None;
  SynchronizationScope required_fence_scope = SynchronizationScope::None;
  double confidence = 0.0;
};

struct VolatileMissingInfo {
  const llvm::Instruction *inst = nullptr;
  const llvm::Value *base = nullptr;
};

struct KernelSummary {
  const llvm::Function *kernel = nullptr;
  LaunchDimensions dimensions;
  bool has_symbolic_grid = false;
  bool has_symbolic_block = false;
  bool has_warp_divergence = false;
  bool has_bank_conflict = false;
  bool has_uncoalesced_access = false;
  bool has_barrier_mismatch = false;
  bool has_shared_race = false;
  bool has_global_race = false;
  bool has_volatile_missing = false;
  size_t shared_access_count = 0;
  size_t device_access_count = 0;
  size_t global_access_count = 0;
  size_t constant_access_count = 0;
  size_t local_access_count = 0;
  size_t atomic_count = 0;
  size_t transfer_count = 0;
  bool has_uncoalesced_constant = false;
  bool has_texture_access = false;
  bool has_surface_access = false;
  llvm::SmallVector<DivergenceRegion, 4> divergence_regions;
  llvm::SmallVector<BankConflictInfo, 4> bank_conflicts;
  llvm::SmallVector<CoalescingInfo, 4> coalescing_issues;
  llvm::SmallVector<RaceInfo, 4> shared_races;
  llvm::SmallVector<RaceInfo, 4> global_races;
  llvm::SmallVector<BarrierMismatchInfo, 4> barrier_mismatches;
  llvm::SmallVector<VolatileMissingInfo, 4> volatile_missing;
  llvm::SmallVector<WarpUniformInfo, 4> warp_uniform_regions;
  llvm::SmallVector<SynchronizationRecord, 4> synchronizations;
  llvm::SmallVector<MemoryTransferInfo, 4> memory_transfers;
  llvm::SmallVector<ConstantAccessInfo, 4> constant_accesses;
  llvm::SmallVector<TextureAccessInfo, 4> texture_accesses;
  llvm::SmallVector<SurfaceAccessInfo, 4> surface_accesses;
  std::vector<AccessInfo> accesses;
};

class CUDAAnalysis {
public:
  explicit CUDAAnalysis(llvm::Module &module,
                        lotus::AliasAnalysisWrapper *alias_analysis,
                        DeviceConfig config = DeviceConfig{});
  explicit CUDAAnalysis(llvm::Module &module,
                        DeviceConfig config = DeviceConfig{});

  void runAnalysis();

  const llvm::Module &getModule() const { return m_module; }
  bool hasCompletedAnalysis() const { return m_has_completed_analysis; }
  bool hasCurrentModuleSnapshot() const;
  bool isCUDAEnabled() const { return m_cuda_enabled; }
  size_t getOperationCount() const { return m_operation_count; }
  size_t getDeviceSyncCount() const { return m_device_sync_count; }
  size_t getBarrierCount() const { return m_barrier_count; }
  size_t getWarpBarrierCount() const { return m_warp_barrier_count; }
  size_t getMemoryBarrierCount() const { return m_memory_barrier_count; }

  const std::vector<KernelLaunchInfo> &getLaunches() const {
    return m_launches;
  }
  const std::vector<KernelSummary> &getKernelSummaries() const {
    return m_kernel_summaries;
  }
  const std::vector<InterKernelRaceInfo> &getInterKernelRaces() const {
    return m_inter_kernel_races;
  }
  const std::vector<MemoryTransferInfo> &getMemoryTransfers() const {
    return m_memory_transfers;
  }
  const std::vector<UnifiedMemoryInfo> &getUnifiedMemory() const {
    return m_unified_memory;
  }
  const DeviceConfig &getDeviceConfig() const { return m_device_config; }

  const CUDAAbstractState &getAbstractState() const { return m_abstract_state; }
  const std::vector<CUDAKernelFact> &getKernelFacts() const {
    return m_abstract_state.getKernelFacts();
  }
  const std::vector<CUDAMemoryTransferFact> &getMemoryTransferFacts() const {
    return m_abstract_state.getMemoryTransferFacts();
  }
  const std::vector<CUDASynchronizationFact> &getSynchronizationFacts() const {
    return m_abstract_state.getSynchronizationFacts();
  }
  const std::vector<CUDAStreamAutomaton> &getStreamAutomata() const {
    return m_abstract_state.getStreamAutomata();
  }
  const std::vector<CUDAEventAutomaton> &getEventAutomata() const {
    return m_abstract_state.getEventAutomata();
  }
  const std::vector<CUDAProtocolEpoch> &getBarrierEpochs() const {
    return m_abstract_state.getBarrierEpochs();
  }
  const std::vector<CUDAProtocolEpoch> &getFenceEpochs() const {
    return m_abstract_state.getFenceEpochs();
  }
  const std::map<const llvm::Function *, CUDAFunctionSummary> &
  getFunctionSummaries() const {
    return m_abstract_state.getFunctionSummaries();
  }
  const std::vector<CUDAParticipantSet> &getParticipantSets() const {
    return m_abstract_state.getParticipantSets();
  }

  static MemorySpace classifyMemorySpace(const llvm::Value *value);
  static const char *toString(MemorySpace space);
  static const char *toString(CoalescingQuality quality);
  static const char *toString(UniformityClass uniformity);
  static const char *toString(SynchronizationScope scope);
  static const char *toString(RaceKind kind);
  static const char *toString(LaunchOrderingSource source);
  static const char *toString(AliasPrecision precision);
  static const char *toString(AliasSource source);
  static const char *toString(SynchronizationPrimitive primitive);

private:
  llvm::Module &m_module;
  ThreadAPI *m_thread_api;
  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;
  std::unique_ptr<lotus::AliasAnalysisWrapper> m_owned_alias_analysis;
  DeviceConfig m_device_config;
  std::vector<KernelLaunchInfo> m_launches;
  std::vector<KernelSummary> m_kernel_summaries;
  std::vector<InterKernelRaceInfo> m_inter_kernel_races;
  std::vector<MemoryTransferInfo> m_memory_transfers;
  std::vector<UnifiedMemoryInfo> m_unified_memory;

  struct LaunchContextKey {
    const llvm::Function *kernel = nullptr;
    const llvm::Instruction *launch_site = nullptr;
    LaunchDimensions dimensions;
    const llvm::Value *argument_array = nullptr;
    const llvm::Value *dynamic_shared_memory = nullptr;
    bool operator==(const LaunchContextKey &other) const {
      if (kernel != other.kernel || launch_site != other.launch_site ||
          argument_array != other.argument_array ||
          dynamic_shared_memory != other.dynamic_shared_memory) {
        return false;
      }
      for (int i = 0; i < 3; ++i) {
        if (dimensions.grid[i].kind != other.dimensions.grid[i].kind ||
            dimensions.grid[i].constant != other.dimensions.grid[i].constant ||
            dimensions.grid[i].value != other.dimensions.grid[i].value) {
          return false;
        }
        if (dimensions.block[i].kind != other.dimensions.block[i].kind ||
            dimensions.block[i].constant !=
                other.dimensions.block[i].constant ||
            dimensions.block[i].value != other.dimensions.block[i].value) {
          return false;
        }
      }
      return true;
    }
  };

  struct LaunchContextKeyHash {
    size_t operator()(const LaunchContextKey &key) const {
      size_t h = reinterpret_cast<size_t>(key.kernel);
      h ^= reinterpret_cast<size_t>(key.launch_site) << 1;
      h ^= reinterpret_cast<size_t>(key.argument_array) << 2;
      h ^= reinterpret_cast<size_t>(key.dynamic_shared_memory) << 3;
      for (int i = 0; i < 3; ++i) {
        h ^= std::hash<int>{}(static_cast<int>(key.dimensions.grid[i].kind));
        h ^= std::hash<int64_t>{}(key.dimensions.grid[i].constant);
        h ^= reinterpret_cast<size_t>(key.dimensions.grid[i].value) << 3;
        h ^= std::hash<int>{}(static_cast<int>(key.dimensions.block[i].kind));
        h ^= std::hash<int64_t>{}(key.dimensions.block[i].constant);
        h ^= reinterpret_cast<size_t>(key.dimensions.block[i].value) << 4;
      }
      return h;
    }
  };

  std::unordered_map<LaunchContextKey, size_t, LaunchContextKeyHash>
      m_launch_context_index;
  CUDAAbstractState m_abstract_state;
  bool m_has_completed_analysis = false;
  bool m_cuda_enabled = true;
  size_t m_operation_count = 0;
  size_t m_device_sync_count = 0;
  size_t m_barrier_count = 0;
  size_t m_warp_barrier_count = 0;
  size_t m_memory_barrier_count = 0;
  uint64_t m_module_fingerprint = 0;
  bool m_has_module_fingerprint = false;

  void analyzeKernel(const llvm::Function *kernel,
                     const KernelLaunchInfo *launch);
  void recordAccess(KernelSummary &summary, const llvm::Instruction *inst,
                    const llvm::Value *pointer, bool is_write);
  void analyzeDivergence(KernelSummary &summary, const llvm::Function *kernel);
  void analyzeRaces(KernelSummary &summary);
  void analyzeVolatile(KernelSummary &summary);
  void analyzeWarpUniformity(KernelSummary &summary,
                             const llvm::Function *kernel);
  void analyzeSynchronization(KernelSummary &summary,
                              const llvm::Function *kernel);
  void analyzeInterKernelRaces();
  void analyzeMemoryTransfers();
  void analyzeUnifiedMemory();
  void analyzeConstantAccesses(KernelSummary &summary);
  void analyzeTextureAndSurfaceAccesses(KernelSummary &summary);
  void recordModelGap(const llvm::Instruction *inst, llvm::StringRef message,
                      double confidence = 0.5);
  void recordModelGap(const llvm::Function *function, llvm::StringRef message,
                      double confidence = 0.5);

  static const llvm::Value *getMemoryOperand(const llvm::Instruction *inst);
  static const llvm::Value *getCanonicalBase(const llvm::Value *value) {
    return CUDAMemoryModel::getCanonicalBase(value);
  }
  static BuiltinKind classifyBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::classifyBuiltin(value);
  }
  static bool dependsOnThreadBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::dependsOnThreadBuiltin(value);
  }
  static bool dependsOnBlockBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::dependsOnBlockBuiltin(value);
  }
  static bool dependsOnLaneBuiltin(const llvm::Value *value) {
    return CUDASymbolicModel::dependsOnLaneBuiltin(value);
  }
  static UniformityClass classifyUniformity(const llvm::Value *value) {
    return CUDASymbolicModel::classifyUniformity(value);
  }
  static ParticipationScope classifyParticipation(const llvm::Value *value) {
    return CUDASymbolicModel::classifyParticipation(value);
  }
  static std::optional<int64_t> evaluateConstantInt(const llvm::Value *value) {
    return CUDASymbolicModel::evaluateConstantInt(value);
  }
  static AffineAccessPattern
  extractAffineAccessPattern(const llvm::Value *value) {
    return CUDASymbolicModel::extractAffineAccessPattern(value);
  }
  static SymbolicDimension classifyDimension(const llvm::Value *value) {
    return CUDASymbolicModel::classifyDimension(value);
  }
  void initializeDefaultAliasAnalysis();
  bool hasAliasAnalysis() const;
  static LaunchDimensions getLaunchDimensions(const llvm::Instruction *launch);
};

} // namespace concurrency::cuda
