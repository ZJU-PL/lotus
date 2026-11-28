#pragma once

#include "Concurrency/Core/ConcurrencyRelation.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace concurrency {

namespace cuda {
class CUDAAnalysis;
} // namespace cuda

class ConcurrencyFacade {
public:
  using RelationCountKey =
      std::pair<concurrency::RelationKind, concurrency::ProofStrength>;

  enum class OpenMPAnalysisStatus { Complete, Disabled, NotApplicable };

  struct OpenMPSummary {
    OpenMPAnalysisStatus status = OpenMPAnalysisStatus::NotApplicable;
    size_t task_count = 0;
    size_t task_with_dependencies_count = 0;
    size_t included_task_count = 0;
    size_t final_task_count = 0;
    size_t untied_task_count = 0;
    size_t detached_task_count = 0;
    size_t taskloop_count = 0;
    size_t taskyield_count = 0;
    size_t parallel_region_count = 0;
    size_t wait_boundary_count = 0;
    size_t partial_wait_boundary_count = 0;
    size_t barrier_count = 0;
    size_t taskgroup_region_count = 0;
    size_t single_region_count = 0;
    size_t master_region_count = 0;
    size_t ordered_region_count = 0;
    size_t sections_region_count = 0;
    size_t worksharing_loop_count = 0;
    size_t reduction_region_count = 0;
    size_t worksharing_region_count = 0;
    size_t critical_region_count = 0;
    size_t lock_api_count = 0;
    size_t atomic_region_count = 0;
    size_t flush_count = 0;
    size_t cancel_count = 0;
    size_t cancellation_point_count = 0;
    size_t target_region_count = 0;
    size_t target_data_region_count = 0;
    size_t detach_completion_count = 0;
    size_t happens_before_relation_count = 0;
    size_t exclusion_relation_count = 0;
    size_t unknown_relation_count = 0;
    size_t unknown_reason_bucket_count = 0;
    size_t deferred_wait_dep_count = 0;
    size_t deferred_conflict_count = 0;
    std::map<RelationCountKey, size_t> relation_counts;
    std::map<std::string, size_t> unknown_reason_counts;

    size_t getRelationCount(concurrency::RelationKind kind,
                            concurrency::ProofStrength proof) const;
  };

  struct MPIDiagnosticSummary {
    const llvm::Instruction *inst = nullptr;
    bool has_relation = false;
    concurrency::Relation relation;
    // Zero denotes that the diagnostic is not scoped by that fact class.
    size_t communicator_class_id = 0;
    size_t participant_class_id = 0;
    size_t channel_class_id = 0;
    size_t request_set_id = 0;
    std::string model_gap_domain;
    std::string subsystem;
    std::string normalization_confidence;
    std::string reason_bucket;
    std::string code;
    std::string detail;
  };

  struct MPISummary {
    size_t operation_count = 0;
    size_t init_count = 0;
    size_t finalize_count = 0;
    size_t blocking_point_to_point_count = 0;
    size_t nonblocking_operation_count = 0;
    size_t nonblocking_point_to_point_count = 0;
    size_t probe_operation_count = 0;
    size_t wait_operation_count = 0;
    size_t test_operation_count = 0;
    size_t collective_operation_count = 0;
    size_t blocking_collective_count = 0;
    size_t nonblocking_collective_count = 0;
    size_t communicator_management_count = 0;
    size_t request_management_count = 0;
    size_t sendrecv_operation_count = 0;
    size_t persistent_request_init_count = 0;
    size_t request_start_count = 0;
    size_t rma_window_count = 0;
    size_t rma_operation_count = 0;
    size_t rma_sync_count = 0;
    size_t may_complete_request_count = 0;
    size_t terminal_request_count = 0;
    size_t freed_request_count = 0;
    size_t rank_restricted_operation_count = 0;
    size_t wildcard_endpoint_operation_count = 0;
    size_t orphaned_request_count = 0;
    size_t potential_deadlock_count = 0;
    size_t mismatched_collective_count = 0;
    size_t conditional_collective_count = 0;
    size_t collective_partial_reachability_count = 0;
    size_t unsynchronized_rma_count = 0;
    size_t rma_race_count = 0;
    size_t tracked_window_count = 0;
    size_t leaked_window_count = 0;
    size_t collective_slot_count = 0;
    size_t deferred_semantic_lowering_count = 0;
    size_t normalization_exact_count = 0;
    size_t normalization_pmpi_wrapper_count = 0;
    size_t normalization_openmpi_forwarder_count = 0;
    size_t normalization_unknown_internal_count = 0;
    bool missing_finalize = false;
    size_t double_finalize_count = 0;
    size_t tag_mismatch_count = 0;
    size_t count_datatype_mismatch_count = 0;
    size_t rank_out_of_bounds_count = 0;
    size_t persistent_request_leak_count = 0;
    size_t wrong_root_rank_count = 0;
    size_t cancel_without_wait_count = 0;
    size_t buffer_overlap_count = 0;
    size_t wildcard_in_collective_count = 0;
    size_t in_place_conflict_count = 0;
    size_t null_handle_count = 0;
    size_t negative_root_count = 0;
    size_t invalid_tag_count = 0;
    size_t invalid_rank_count = 0;
    size_t type_size_mismatch_count = 0;
    size_t destroy_null_communicator_count = 0;
    size_t request_free_after_wait_count = 0;
    size_t in_place_wrong_operation_count = 0;
    size_t invalid_rma_transition_count = 0;
    size_t use_after_free_window_count = 0;
    size_t double_window_free_count = 0;
    size_t model_gap_count = 0;
    size_t diagnostic_count = 0;
    std::map<std::string, size_t> diagnostic_code_counts;
    std::map<std::string, size_t> model_gap_domain_counts;
    std::map<RelationCountKey, size_t> relation_counts;
    std::vector<MPIDiagnosticSummary> diagnostics;

    size_t getRelationCount(concurrency::RelationKind kind,
                            concurrency::ProofStrength proof) const;
  };

  enum class CUDAAnalysisStatus {
    Complete,
    Disabled,
    NotRun,
    ModuleMismatch,
    StaleAnalysis
  };

  struct CUDAModelGapSummary {
    const llvm::Instruction *inst = nullptr;
    std::string reason_bucket;
    std::string explanation;
    double confidence = 0.0;
    size_t related_instruction_count = 0;
  };

  struct CUDASummary {
    CUDAAnalysisStatus status = CUDAAnalysisStatus::NotRun;
    size_t operation_count = 0;
    size_t kernel_launch_count = 0;
    size_t device_sync_count = 0;
    size_t barrier_count = 0;
    size_t warp_barrier_count = 0;
    size_t memory_barrier_count = 0;
    size_t atomic_count = 0;
    size_t symbolic_launch_count = 0;
    size_t kernel_count = 0;
    size_t warp_divergence_count = 0;
    size_t shared_race_count = 0;
    size_t global_race_count = 0;
    size_t barrier_mismatch_count = 0;
    size_t bank_conflict_count = 0;
    size_t uncoalesced_access_count = 0;
    size_t volatile_missing_count = 0;
    size_t inter_kernel_hazard_count = 0;
    size_t transfer_count = 0;
    size_t async_transfer_count = 0;
    size_t unified_memory_count = 0;
    size_t managed_allocation_count = 0;
    size_t unified_prefetch_count = 0;
    size_t unified_host_allocation_count = 0;
    size_t shared_access_count = 0;
    size_t device_access_count = 0;
    size_t global_access_count = 0;
    size_t constant_access_count = 0;
    size_t local_access_count = 0;
    size_t model_gap_count = 0;
    std::map<std::string, size_t> model_gap_reason_counts;
    std::vector<CUDAModelGapSummary> model_gaps;

    bool isComplete() const { return status == CUDAAnalysisStatus::Complete; }
  };

  static OpenMPSummary analyzeOpenMP(llvm::Module &module);
  static MPISummary analyzeMPI(llvm::Module &module);
  static CUDASummary analyzeCUDA(llvm::Module &module);
  static CUDASummary summarizeCUDA(const cuda::CUDAAnalysis &analysis);
  static CUDASummary summarizeCUDA(const cuda::CUDAAnalysis &analysis,
                                   const llvm::Module &module);
  static void printOpenMPResults(llvm::Module &module, llvm::raw_ostream &os);
};

} // namespace concurrency
