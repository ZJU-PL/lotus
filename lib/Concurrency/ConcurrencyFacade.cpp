#include "Concurrency/ConcurrencyFacade.h"

#include "Concurrency/API/ThreadAPI.h"
#include "Concurrency/CUDA/CUDAAnalysis.h"
#include "Concurrency/MPI/MPIAnalysis.h"
#include "Concurrency/OpenMP/OpenMPTaskGraph.h"

#include <numeric>

#include <llvm/IR/InstIterator.h>

namespace concurrency {

namespace {

std::string modelGapDomainName(mpi::MPIModelGapDomain domain) {
  switch (domain) {
  case mpi::MPIModelGapDomain::None:
    return "none";
  case mpi::MPIModelGapDomain::Rank:
    return "rank";
  case mpi::MPIModelGapDomain::ParticipantSet:
    return "participant_set";
  case mpi::MPIModelGapDomain::Communicator:
    return "communicator";
  case mpi::MPIModelGapDomain::CollectiveProtocol:
    return "collective_protocol";
  case mpi::MPIModelGapDomain::PointToPoint:
    return "point_to_point";
  case mpi::MPIModelGapDomain::RequestLifecycle:
    return "request_lifecycle";
  case mpi::MPIModelGapDomain::RMAEpoch:
    return "rma_epoch";
  case mpi::MPIModelGapDomain::Completion:
    return "completion";
  case mpi::MPIModelGapDomain::Unknown:
    return "unknown";
  }
  return "unknown";
}

std::string cudaModelGapReasonBucket(llvm::StringRef explanation) {
  if (explanation.contains("launch site could not be matched")) {
    return "launch_target_unresolved";
  }
  if (explanation.contains("launch dimensions remain symbolic")) {
    return "launch_dimensions_symbolic";
  }
  if (explanation.contains("without an explicit host-side launch context")) {
    return "launch_context_missing";
  }
  if (explanation.contains("Inter-kernel race analysis")) {
    return "inter_kernel_context_imprecise";
  }
  if (explanation.contains("memcpy-like operation")) {
    return "memory_transfer_unresolved";
  }
  if (explanation.contains("Managed allocation")) {
    return "managed_allocation_conservative";
  }
  if (explanation.contains("Unified-memory operation")) {
    return "unified_memory_pointer_unresolved";
  }
  if (explanation.contains("alias") || explanation.contains("Alias")) {
    return "alias_imprecise";
  }
  return "other";
}

} // namespace

size_t ConcurrencyFacade::OpenMPSummary::getRelationCount(
    concurrency::RelationKind kind, concurrency::ProofStrength proof) const {
  auto it = relation_counts.find({kind, proof});
  return it == relation_counts.end() ? 0 : it->second;
}

size_t ConcurrencyFacade::MPISummary::getRelationCount(
    concurrency::RelationKind kind, concurrency::ProofStrength proof) const {
  auto it = relation_counts.find({kind, proof});
  return it == relation_counts.end() ? 0 : it->second;
}

ConcurrencyFacade::OpenMPSummary
ConcurrencyFacade::analyzeOpenMP(llvm::Module &module) {
  OpenMPSummary summary;
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  if (!api->getConfig().enable_openmp()) {
    summary.status = OpenMPAnalysisStatus::Disabled;
    return summary;
  }
  bool has_openmp_event = false;
  for (const llvm::Function &function : module) {
    for (llvm::const_inst_iterator it = llvm::inst_begin(function),
                                   end = llvm::inst_end(function);
         it != end; ++it) {
      const auto *call = llvm::dyn_cast<llvm::CallBase>(&*it);
      if (call && api->getType(call) != ThreadAPI::TD_DUMMY &&
          api->getRuntimeLibrary(call) ==
              ThreadAPI::RuntimeLibrary::OpenMP) {
        has_openmp_event = true;
        break;
      }
    }
    if (has_openmp_event) {
      break;
    }
  }
  if (!has_openmp_event) {
    return summary;
  }

  OpenMP::OpenMPTaskGraph graph(module);
  graph.analyze();

  const auto &graph_summary = graph.getSummary();
  summary.status = OpenMPAnalysisStatus::Complete;
  summary.task_count = graph_summary.task_count;
  summary.task_with_dependencies_count =
      graph_summary.task_with_dependencies_count;
  summary.included_task_count = graph_summary.included_task_count;
  summary.final_task_count = graph_summary.final_task_count;
  summary.untied_task_count = graph_summary.untied_task_count;
  summary.detached_task_count = graph_summary.detached_task_count;
  summary.taskloop_count = graph_summary.taskloop_count;
  summary.taskyield_count = graph_summary.taskyield_count;
  summary.parallel_region_count = graph_summary.parallel_region_count;
  summary.wait_boundary_count = graph_summary.wait_boundary_count;
  summary.partial_wait_boundary_count =
      graph_summary.partial_wait_boundary_count;
  summary.barrier_count = graph_summary.barrier_count;
  summary.taskgroup_region_count = graph_summary.taskgroup_region_count;
  summary.single_region_count = graph_summary.single_region_count;
  summary.master_region_count = graph_summary.master_region_count;
  summary.ordered_region_count = graph_summary.ordered_region_count;
  summary.sections_region_count = graph_summary.sections_region_count;
  summary.worksharing_loop_count = graph_summary.worksharing_loop_count;
  summary.reduction_region_count = graph_summary.reduction_region_count;
  summary.worksharing_region_count =
      graph_summary.single_region_count + graph_summary.sections_region_count +
      graph_summary.worksharing_loop_count +
      graph_summary.reduction_region_count +
      graph_summary.ordered_region_count + graph_summary.master_region_count;
  summary.critical_region_count = graph_summary.critical_region_count;
  summary.lock_api_count = graph_summary.lock_api_count;
  summary.atomic_region_count = graph_summary.atomic_region_count;
  summary.flush_count = graph_summary.flush_count;
  summary.cancel_count = graph_summary.cancel_count;
  summary.cancellation_point_count = graph_summary.cancellation_point_count;
  summary.target_region_count = graph_summary.target_region_count;
  summary.target_data_region_count = graph_summary.target_data_region_count;
  summary.detach_completion_count = graph_summary.detach_completion_count;
  summary.happens_before_relation_count =
      graph.getRelationCount(concurrency::RelationKind::MustHappenBefore) +
      graph.getRelationCount(concurrency::RelationKind::SelectiveHappenBefore);
  summary.exclusion_relation_count =
      graph.getRelationCount(concurrency::RelationKind::MutuallyExclusive);
  summary.unknown_relation_count =
      graph.getRelationCount(concurrency::RelationKind::UnknownDueToModelGap);
  summary.unknown_reason_bucket_count = graph.getUnknownReasonCounts().size();
  summary.unknown_reason_counts.insert(graph.getUnknownReasonCounts().begin(),
                                       graph.getUnknownReasonCounts().end());
  for (const auto &entry : graph.getRelations()) {
    ++summary.relation_counts[{entry.second.kind, entry.second.proof}];
  }
  summary.deferred_wait_dep_count = graph.getDeferredWaitDepsCount();
  summary.deferred_conflict_count = graph.getDeferredImpreciseConflictCount();
  return summary;
}

ConcurrencyFacade::MPISummary
ConcurrencyFacade::analyzeMPI(llvm::Module &module) {
  mpi::MPIAnalysis analysis(module);
  analysis.runAnalysis();

  const auto &results = analysis.getResults();
  const auto &operations = analysis.getProcessModel().getAllOperations();
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  MPISummary summary;
  summary.operation_count = operations.size();
  summary.init_count = analysis.getOperationCount(mpi::MPIOpKind::INIT);
  summary.finalize_count = analysis.getOperationCount(mpi::MPIOpKind::FINALIZE);
  summary.blocking_point_to_point_count =
      analysis.getOperationCount(mpi::MPIOpKind::SEND_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::RECV_BLOCKING);
  summary.nonblocking_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::SEND_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::RECV_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
  summary.nonblocking_point_to_point_count =
      analysis.getOperationCount(mpi::MPIOpKind::SEND_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::RECV_NONBLOCKING);
  summary.probe_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::PROBE_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::PROBE_NONBLOCKING);
  summary.wait_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::WAIT);
  summary.test_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::TEST);
  summary.collective_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
  summary.blocking_collective_count =
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_BLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_BLOCKING);
  summary.nonblocking_collective_count =
      analysis.getOperationCount(mpi::MPIOpKind::BARRIER_NONBLOCKING) +
      analysis.getOperationCount(mpi::MPIOpKind::COLLECTIVE_NONBLOCKING);
  summary.communicator_management_count =
      analysis.getOperationCount(mpi::MPIOpKind::COMM_MANAGEMENT);
  summary.request_management_count =
      analysis.getOperationCount(mpi::MPIOpKind::REQUEST_MANAGEMENT);
  for (const auto &op : operations) {
    if (op.td_type == ThreadAPI::TD_MPI_SENDRECV) {
      ++summary.sendrecv_operation_count;
    }
    if (op.td_type == ThreadAPI::TD_MPI_PERSISTENT_SEND_INIT ||
        op.td_type == ThreadAPI::TD_MPI_PERSISTENT_RECV_INIT) {
      ++summary.persistent_request_init_count;
    }
    if (op.td_type == ThreadAPI::TD_MPI_REQUEST_START) {
      ++summary.request_start_count;
    }
    if (op.protocol_reachability == mpi::ProtocolReachability::SomeRanks &&
        (op.kind == mpi::MPIOpKind::BARRIER_BLOCKING ||
         op.kind == mpi::MPIOpKind::BARRIER_NONBLOCKING ||
         op.kind == mpi::MPIOpKind::COLLECTIVE_BLOCKING ||
         op.kind == mpi::MPIOpKind::COLLECTIVE_NONBLOCKING)) {
      ++summary.rank_restricted_operation_count;
    }
    if (op.kind == mpi::MPIOpKind::SEND_BLOCKING ||
        op.kind == mpi::MPIOpKind::SEND_NONBLOCKING) {
      if (op.dest_rank < 0 || op.tag < 0) {
        ++summary.wildcard_endpoint_operation_count;
      }
      continue;
    }
    if (op.kind == mpi::MPIOpKind::RECV_BLOCKING ||
        op.kind == mpi::MPIOpKind::RECV_NONBLOCKING ||
        op.kind == mpi::MPIOpKind::PROBE_BLOCKING ||
        op.kind == mpi::MPIOpKind::PROBE_NONBLOCKING) {
      if (op.source_rank < 0 || op.tag < 0) {
        ++summary.wildcard_endpoint_operation_count;
      }
    }
  }
  summary.sendrecv_operation_count /= 2;
  summary.rma_window_count =
      analysis.getOperationCount(mpi::MPIOpKind::RMA_WINDOW);
  summary.rma_operation_count =
      analysis.getOperationCount(mpi::MPIOpKind::RMA_DATA);
  summary.rma_sync_count = analysis.getOperationCount(mpi::MPIOpKind::RMA_SYNC);
  auto requestStatePriority = [](mpi::RequestCompletionState state) {
    switch (state) {
    case mpi::RequestCompletionState::Unbound:
      return 0;
    case mpi::RequestCompletionState::PersistentTemplate:
      return 1;
    case mpi::RequestCompletionState::InactivePersistent:
      return 2;
    case mpi::RequestCompletionState::Active:
      return 3;
    case mpi::RequestCompletionState::MayComplete:
      return 4;
    case mpi::RequestCompletionState::MustComplete:
      return 5;
    case mpi::RequestCompletionState::Canceled:
      return 6;
    case mpi::RequestCompletionState::Freed:
      return 7;
    case mpi::RequestCompletionState::Escaped:
      return 8;
    case mpi::RequestCompletionState::Unknown:
      return 9;
    }
    return 0;
  };
  std::unordered_map<mpi::RequestID, mpi::RequestCompletionState>
      request_states;
  for (const auto &op : operations) {
    if (op.kind != mpi::MPIOpKind::SEND_NONBLOCKING &&
        op.kind != mpi::MPIOpKind::RECV_NONBLOCKING &&
        op.kind != mpi::MPIOpKind::BARRIER_NONBLOCKING &&
        op.kind != mpi::MPIOpKind::COLLECTIVE_NONBLOCKING) {
      continue;
    }
    if (!op.request) {
      continue;
    }
    auto it = request_states.find(op.request);
    if (it == request_states.end() || requestStatePriority(op.request_state) >
                                          requestStatePriority(it->second)) {
      request_states[op.request] = op.request_state;
    }
  }
  for (const auto &entry : request_states) {
    if (entry.second == mpi::RequestCompletionState::MayComplete) {
      ++summary.may_complete_request_count;
    }
    if (entry.second == mpi::RequestCompletionState::Canceled ||
        entry.second == mpi::RequestCompletionState::Freed) {
      ++summary.terminal_request_count;
    }
    if (entry.second == mpi::RequestCompletionState::Freed) {
      ++summary.freed_request_count;
    }
  }
  summary.orphaned_request_count = results.orphaned_requests.size();
  summary.potential_deadlock_count = results.potential_deadlocks.size();
  summary.mismatched_collective_count = results.mismatched_collectives.size();
  summary.conditional_collective_count = results.conditional_collectives.size();
  summary.collective_partial_reachability_count =
      analysis.getProtocolDiagnosticCount("collective_partial_reachability");
  summary.unsynchronized_rma_count = results.unsynchronized_rma.size();
  summary.rma_race_count = results.rma_races.size();
  summary.tracked_window_count = analysis.getTrackedWindowCount();
  summary.leaked_window_count = results.leaked_windows.size();
  summary.collective_slot_count =
      analysis.getProtocolDiagnosticCount("collective_slots_tracked");
  summary.deferred_semantic_lowering_count = std::accumulate(
      deferred.begin(), deferred.end(), size_t{0},
      [](size_t total, const std::pair<const std::string, size_t> &entry) {
        if (entry.first == "unknown_flag_value" ||
            entry.first == "unknown_completed_index_set") {
          return total;
        }
        return total + entry.second;
      });
  const auto &normalization =
      analysis.getProcessModel().getNormalizationConfidenceCounts();
  auto normalizationCount = [&](mpi::NormalizationConfidence confidence) {
    auto it = normalization.find(confidence);
    return it == normalization.end() ? size_t{0} : it->second;
  };
  summary.normalization_exact_count =
      normalizationCount(mpi::NormalizationConfidence::ExactMPI);
  summary.normalization_pmpi_wrapper_count =
      normalizationCount(mpi::NormalizationConfidence::PMPIWrapper);
  summary.normalization_openmpi_forwarder_count =
      normalizationCount(mpi::NormalizationConfidence::KnownOpenMPIForwarder);
  summary.normalization_unknown_internal_count =
      normalizationCount(mpi::NormalizationConfidence::UnknownVendorInternal);

  summary.missing_finalize = results.missing_finalize;
  summary.double_finalize_count = results.double_finalize.size();
  summary.tag_mismatch_count = results.tag_mismatches.size();
  summary.count_datatype_mismatch_count =
      results.count_datatype_mismatches.size();
  summary.rank_out_of_bounds_count = results.rank_out_of_bounds.size();
  summary.persistent_request_leak_count =
      results.persistent_request_leaks.size();
  summary.wrong_root_rank_count = results.wrong_root_ranks.size();
  summary.cancel_without_wait_count = results.cancel_without_wait.size();
  summary.buffer_overlap_count = results.buffer_overlaps.size();
  summary.wildcard_in_collective_count =
      results.wildcard_in_collective.size();
  summary.in_place_conflict_count = results.in_place_conflicts.size();
  summary.null_handle_count = results.null_handles.size();
  summary.negative_root_count = results.negative_root.size();
  summary.invalid_tag_count = results.invalid_tags.size();
  summary.invalid_rank_count = results.invalid_ranks.size();
  summary.type_size_mismatch_count = results.type_size_mismatches.size();
  summary.destroy_null_communicator_count = results.destroy_null_comm.size();
  summary.request_free_after_wait_count =
      results.request_free_after_wait.size();
  summary.in_place_wrong_operation_count = results.in_place_wrong_op.size();
  summary.invalid_rma_transition_count =
      results.invalid_rma_transitions.size();
  summary.use_after_free_window_count = results.use_after_free_windows.size();
  summary.double_window_free_count = results.double_window_free.size();
  summary.model_gap_count = results.model_gaps.size();

  auto addDiagnostic = [&](const llvm::Instruction *inst, llvm::StringRef code,
                           llvm::StringRef detail = {}) {
    MPIDiagnosticSummary diagnostic;
    diagnostic.inst = inst;
    diagnostic.code = code.str();
    diagnostic.detail = detail.str();
    diagnostic.reason_bucket = diagnostic.code;
    ++summary.diagnostic_code_counts[diagnostic.code];
    summary.diagnostics.push_back(std::move(diagnostic));
  };
  if (results.missing_finalize) {
    addDiagnostic(nullptr, "missing_finalize");
  }
  for (const llvm::Instruction *inst : results.double_finalize) {
    addDiagnostic(inst, "double_finalize");
  }
  for (const llvm::Instruction *inst : results.invalid_tags) {
    addDiagnostic(inst, "invalid_tag");
  }
  for (const llvm::Instruction *inst : results.invalid_ranks) {
    addDiagnostic(inst, "invalid_rank");
  }
  for (const auto &pair : results.tag_mismatches) {
    addDiagnostic(pair.first, "tag_mismatch");
  }
  for (const auto &pair : results.count_datatype_mismatches) {
    addDiagnostic(pair.first, "count_datatype_mismatch");
  }
  for (const llvm::Instruction *inst : results.rank_out_of_bounds) {
    addDiagnostic(inst, "rank_out_of_bounds");
  }
  for (mpi::RequestID request : results.persistent_request_leaks) {
    addDiagnostic(llvm::dyn_cast_or_null<llvm::Instruction>(request),
                  "persistent_request_leak");
  }
  for (const auto &pair : results.wrong_root_ranks) {
    addDiagnostic(pair.first.inst, "wrong_root_rank");
  }
  for (const llvm::Instruction *inst : results.cancel_without_wait) {
    addDiagnostic(inst, "cancel_without_wait");
  }
  for (const auto &pair : results.buffer_overlaps) {
    addDiagnostic(pair.first, "buffer_overlap");
  }
  for (const llvm::Instruction *inst : results.wildcard_in_collective) {
    addDiagnostic(inst, "wildcard_in_collective");
  }
  for (const llvm::Instruction *inst : results.in_place_conflicts) {
    addDiagnostic(inst, "in_place_conflict");
  }
  for (const llvm::Instruction *inst : results.null_handles) {
    addDiagnostic(inst, "null_handle");
  }
  for (const llvm::Instruction *inst : results.negative_root) {
    addDiagnostic(inst, "negative_root");
  }
  for (const auto &pair : results.type_size_mismatches) {
    addDiagnostic(pair.first, "type_size_mismatch");
  }
  for (const llvm::Instruction *inst : results.destroy_null_comm) {
    addDiagnostic(inst, "destroy_null_communicator");
  }
  for (const llvm::Instruction *inst : results.request_free_after_wait) {
    addDiagnostic(inst, "request_free_after_wait");
  }
  for (const llvm::Instruction *inst : results.in_place_wrong_op) {
    addDiagnostic(inst, "in_place_wrong_operation");
  }
  for (const llvm::Instruction *inst : results.invalid_rma_transitions) {
    addDiagnostic(inst, "invalid_rma_transition");
  }
  for (const llvm::Instruction *inst : results.use_after_free_windows) {
    addDiagnostic(inst, "use_after_free_window");
  }
  for (const llvm::Instruction *inst : results.double_window_free) {
    addDiagnostic(inst, "double_window_free");
  }
  for (const auto &request : results.orphaned_requests) {
    addDiagnostic(request.issue_inst, "orphaned_request");
  }
  for (const auto &pair : results.potential_deadlocks) {
    addDiagnostic(pair.first, "potential_deadlock");
  }
  for (const auto &pair : results.mismatched_collectives) {
    addDiagnostic(pair.first.inst, "mismatched_collective");
  }
  for (const llvm::Instruction *inst : results.conditional_collectives) {
    addDiagnostic(inst, "conditional_collective");
  }
  for (const auto &operation : results.unsynchronized_rma) {
    addDiagnostic(operation.inst, "unsynchronized_rma");
  }
  for (const auto &pair : results.rma_races) {
    addDiagnostic(pair.first.inst, "rma_race");
  }
  for (const auto &diagnostic : results.diagnostics) {
    MPIDiagnosticSummary projected;
    projected.inst = diagnostic.inst;
    projected.has_relation = true;
    projected.relation = diagnostic.relation;
    projected.communicator_class_id = diagnostic.communicator_class_id;
    projected.participant_class_id = diagnostic.participant_class_id;
    projected.channel_class_id = diagnostic.channel_class_id;
    projected.request_set_id = diagnostic.request_set_id;
    projected.model_gap_domain =
        modelGapDomainName(diagnostic.model_gap_domain);
    projected.subsystem = diagnostic.subsystem;
    projected.normalization_confidence = mpi::toString(diagnostic.confidence);
    projected.reason_bucket = diagnostic.reason_bucket.empty()
                                  ? diagnostic.relation.reason
                                  : diagnostic.reason_bucket;
    projected.code = diagnostic.code;
    projected.detail = diagnostic.detail;
    ++summary.diagnostic_code_counts[projected.code];
    if (diagnostic.model_gap_domain != mpi::MPIModelGapDomain::None) {
      ++summary.model_gap_domain_counts[projected.model_gap_domain];
    }
    summary.diagnostics.push_back(std::move(projected));
  }
  auto countRelation = [&](const concurrency::Relation &relation) {
    ++summary.relation_counts[{relation.kind, relation.proof}];
  };
  for (const auto &fact : results.request_set_facts) {
    countRelation(fact.relation);
  }
  for (const auto &fact : results.channel_obligations) {
    countRelation(fact.relation);
  }
  for (const auto &fact : results.protocol_frontiers) {
    countRelation(fact.relation);
  }
  for (const auto &fact : results.rma_synchronization_facts) {
    countRelation(fact.relation);
  }
  summary.diagnostic_count = summary.diagnostics.size();
  return summary;
}

ConcurrencyFacade::CUDASummary
ConcurrencyFacade::analyzeCUDA(llvm::Module &module) {
  if (!ThreadAPI::getThreadAPI()->getConfig().enable_cuda()) {
    CUDASummary summary;
    summary.status = CUDAAnalysisStatus::Disabled;
    return summary;
  }
  cuda::CUDAAnalysis analysis(module);
  analysis.runAnalysis();
  return summarizeCUDA(analysis);
}

ConcurrencyFacade::CUDASummary
ConcurrencyFacade::summarizeCUDA(const cuda::CUDAAnalysis &analysis) {
  return summarizeCUDA(analysis, analysis.getModule());
}

ConcurrencyFacade::CUDASummary
ConcurrencyFacade::summarizeCUDA(const cuda::CUDAAnalysis &analysis,
                                 const llvm::Module &module) {
  CUDASummary summary;
  if (&analysis.getModule() != &module) {
    summary.status = CUDAAnalysisStatus::ModuleMismatch;
    return summary;
  }
  if (!analysis.hasCompletedAnalysis()) {
    summary.status = CUDAAnalysisStatus::NotRun;
    return summary;
  }
  if (!analysis.hasCurrentModuleSnapshot()) {
    summary.status = CUDAAnalysisStatus::StaleAnalysis;
    return summary;
  }
  if (!analysis.isCUDAEnabled()) {
    summary.status = CUDAAnalysisStatus::Disabled;
    return summary;
  }
  summary.status = CUDAAnalysisStatus::Complete;

  summary.operation_count = analysis.getOperationCount();
  summary.device_sync_count = analysis.getDeviceSyncCount();
  summary.barrier_count = analysis.getBarrierCount();
  summary.warp_barrier_count = analysis.getWarpBarrierCount();
  summary.memory_barrier_count = analysis.getMemoryBarrierCount();

  summary.kernel_count = analysis.getKernelSummaries().size();
  summary.kernel_launch_count = analysis.getLaunches().size();
  summary.inter_kernel_hazard_count = analysis.getInterKernelRaces().size();
  summary.transfer_count = analysis.getMemoryTransfers().size();
  summary.unified_memory_count = analysis.getUnifiedMemory().size();
  for (const auto &transfer : analysis.getMemoryTransfers()) {
    if (transfer.is_async) {
      ++summary.async_transfer_count;
    }
  }
  for (const auto &info : analysis.getUnifiedMemory()) {
    if (info.is_prefetch) {
      ++summary.unified_prefetch_count;
    } else if (info.is_managed) {
      ++summary.managed_allocation_count;
    } else {
      ++summary.unified_host_allocation_count;
    }
  }
  for (const auto &launch : analysis.getLaunches()) {
    if (launch.dimensions.hasSymbolicGrid() ||
        launch.dimensions.hasSymbolicBlock()) {
      ++summary.symbolic_launch_count;
    }
  }
  for (const auto &kernel : analysis.getKernelSummaries()) {
    summary.shared_access_count += kernel.shared_access_count;
    summary.device_access_count += kernel.device_access_count;
    summary.global_access_count += kernel.global_access_count;
    summary.constant_access_count += kernel.constant_access_count;
    summary.local_access_count += kernel.local_access_count;
    summary.atomic_count += kernel.atomic_count;
    summary.warp_divergence_count += kernel.divergence_regions.size();
    summary.shared_race_count += kernel.shared_races.size();
    summary.global_race_count += kernel.global_races.size();
    summary.barrier_mismatch_count += kernel.barrier_mismatches.size();
    summary.bank_conflict_count += kernel.bank_conflicts.size();
    summary.uncoalesced_access_count += kernel.coalescing_issues.size();
    summary.volatile_missing_count += kernel.volatile_missing.size();
  }

  for (const auto &gap : analysis.getAbstractState().getModelGaps()) {
    CUDAModelGapSummary projected;
    projected.inst = gap.related_instructions.empty()
                         ? nullptr
                         : gap.related_instructions.front();
    projected.reason_bucket = cudaModelGapReasonBucket(gap.explanation);
    projected.explanation = gap.explanation;
    projected.confidence = gap.confidence;
    projected.related_instruction_count = gap.related_instructions.size();
    ++summary.model_gap_reason_counts[projected.reason_bucket];
    summary.model_gaps.push_back(std::move(projected));
  }
  summary.model_gap_count = summary.model_gaps.size();

  return summary;
}

void ConcurrencyFacade::printOpenMPResults(llvm::Module &module,
                                           llvm::raw_ostream &os) {
  const OpenMPSummary summary = analyzeOpenMP(module);
  os << "========================================\n";
  os << "OpenMP Analysis Results\n";
  os << "========================================\n\n";
  os << "Tasks: " << summary.task_count << "\n";
  os << "  Task-with-deps: " << summary.task_with_dependencies_count << "\n";
  os << "  Included/final/untied/detached: " << summary.included_task_count
     << "/" << summary.final_task_count << "/" << summary.untied_task_count
     << "/" << summary.detached_task_count << "\n";
  os << "  Taskloop/taskyield: " << summary.taskloop_count << "/"
     << summary.taskyield_count << "\n";
  os << "Parallel/barrier regions: " << summary.parallel_region_count << "/"
     << summary.barrier_count << "\n";
  os << "Scheduling boundaries (wait/partial/taskgroup): "
     << summary.wait_boundary_count << "/"
     << summary.partial_wait_boundary_count << "/"
     << summary.taskgroup_region_count << "\n";
  os << "Worksharing regions "
        "(total/single/master/ordered/sections/loops/reduction): "
     << summary.worksharing_region_count << "/" << summary.single_region_count
     << "/" << summary.master_region_count << "/"
     << summary.ordered_region_count << "/" << summary.sections_region_count
     << "/" << summary.worksharing_loop_count << "/"
     << summary.reduction_region_count << "\n";
  os << "Critical/lock APIs: " << summary.critical_region_count << "/"
     << summary.lock_api_count << "\n";
  os << "Atomic/flush/cancel/cancel-point: " << summary.atomic_region_count
     << "/" << summary.flush_count << "/" << summary.cancel_count << "/"
     << summary.cancellation_point_count << "\n";
  os << "Target regions (target/target-data): " << summary.target_region_count
     << "/" << summary.target_data_region_count << "\n";
  os << "Detach completions: " << summary.detach_completion_count << "\n";
  os << "Task relations (HB/exclusion/unknown): "
     << summary.happens_before_relation_count << "/"
     << summary.exclusion_relation_count << "/"
     << summary.unknown_relation_count << "\n";
  os << "Unknown relation reason buckets: "
     << summary.unknown_reason_bucket_count << "\n";
  os << "Deferred modeling (wait-deps/conflicts): "
     << summary.deferred_wait_dep_count << "/"
     << summary.deferred_conflict_count << "\n";
  os << "========================================\n";
}

} // namespace concurrency
