#pragma once

#include <string>

namespace lotus::concurrency::OpenMP {

/// Canonical domain enum for fine-grained OpenMP runtime operations.
///
/// Each enumerator corresponds to one TD_OMP_* operation kind recognized by
/// ThreadAPI (see include/Concurrency/API/ThreadAPI.h). Keeping a dedicated
/// OpenMP-scoped enum decouples OpenMP-specific analyses from the generic
/// thread-API type space.
enum class OpenMPOpKind {
  // OpenMP 3.0+ Task Support
  Task,             ///< __kmpc_omp_task - explicit task creation
  TaskWait,         ///< __kmpc_omp_taskwait - wait for child tasks
  TaskWaitDeps,     ///< __kmpc_omp_wait_deps* - partial task dependency wait
  TaskYield,        ///< __kmpc_omp_taskyield - yield to other tasks
  TaskgroupStart,   ///< __kmpc_taskgroup - start task group
  TaskgroupEnd,     ///< __kmpc_end_taskgroup - end task group
  TaskWithDeps,     ///< __kmpc_omp_task_with_deps - task with dependencies
  Taskloop,         ///< __kmpc_taskloop - taskloop construct
  TaskComplete,     ///< detached/inline task completion callback
  SingleStart,      ///< __kmpc_single - single region entry
  SingleEnd,        ///< __kmpc_end_single - implicit single barrier
  MasterStart,      ///< __kmpc_master - master region entry
  MasterEnd,        ///< __kmpc_end_master - master region exit
  OrderedStart,     ///< __kmpc_ordered - ordered region entry
  OrderedEnd,       ///< __kmpc_end_ordered - ordered region exit
  ReduceStart,      ///< __kmpc_reduce - reduction with implicit barrier
  ReduceEnd,        ///< __kmpc_end_reduce - reduction region exit
  ReduceNowaitStart, ///< __kmpc_reduce_nowait - reduction without barrier
  ReduceNowaitEnd,   ///< __kmpc_end_reduce_nowait - reduction-nowait exit
  ForStaticInit,     ///< __kmpc_for_static_init_* - worksharing loop entry
  ForStaticFini,     ///< __kmpc_for_static_fini - worksharing loop end
  ForDispatchInit,   ///< __kmpc_dispatch_init_* - dynamic loop entry
  ForDispatchNext,   ///< __kmpc_dispatch_next_* - loop chunk fetch
  ForDispatchFini,   ///< __kmpc_dispatch_fini_* - dynamic loop end

  // OpenMP Additional Constructs
  SectionsInit,      ///< __kmpc_sections_init - sections construct
  SectionsNext,      ///< __kmpc_next_section - get next section
  SectionsEnd,       ///< __kmpc_end_sections - end sections
  AtomicStart,       ///< __kmpc_atomic_start - atomic region start
  AtomicEnd,         ///< __kmpc_atomic_end - atomic region end
  Flush,             ///< __kmpc_flush - memory fence
  Cancel,            ///< __kmpc_cancel - cancellation
  CriticalStart,     ///< __kmpc_critical - critical section entry
  CriticalEnd,       ///< __kmpc_end_critical - critical section exit
  ParallelStart,     ///< __kmpc_fork_call - parallel region entry
  Target,            ///< __tgt_target* - target offloading
  TargetDataBegin,   ///< __tgt_target_data_begin
  TargetDataEnd,     ///< __tgt_target_data_end
  TargetDataUpdate,  ///< __tgt_target_data_update

  // OpenMP 5.0+ Teams and Distribute
  Teams,               ///< __kmpc_teams* - teams construct
  TeamsHost,           ///< __kmpc_teams_host
  TeamsDistribute,     ///< __kmpc_teams_distribute*
  Distribute,          ///< __kmpc_distribute* - distribute construct
  DistributeStatic,    ///< __kmpc_distribute_static*
  DistributeDynamic,   ///< __kmpc_distribute_dynamic*
  DistributeGuidance,  ///< __kmpc_distribute_guidance*

  // OpenMP 5.0+ Loop
  LoopStaticInit,   ///< __kmpc_loop_static
  LoopDynamicInit,  ///< __kmpc_loop_dynamic
  LoopGuidanceInit, ///< __kmpc_loop_guidance

  // OpenMP 5.0+ Affinity
  Affinity, ///< __kmpc_affinity*

  // OpenMP 5.0+ Scope
  ScopeStart, ///< __kmpc_scope
  ScopeEnd,   ///< __kmpc_end_scope

  // OpenMP 5.0+ Taskloop variants
  TaskloopSimd, ///< __kmpc_taskloop_simd
  TaskloopFini, ///< __kmpc_taskloop_fini

  // OpenMP 5.0+ Interop
  InteropInit, ///< __kmpc_interop*
  InteropFini, ///< __kmpc_interop_fini*

  // OpenMP 5.1+ Doacross
  DoacrossInit,   ///< __kmpc_doacross*
  DoacrossWait,   ///< __kmpc_doacross_wait*
  DoacrossSubmit, ///< __kmpc_doacross_submit*

  Unknown ///< unrecognized / unclassified operation
};

/// Return a stable string name for an OpenMP operation kind.
///
/// The returned strings match the canonical TD_OMP_* names used by ThreadAPI
/// (e.g. OpenMPOpKind::Task -> "TD_OMP_TASK") so logs and diagnostics stay
/// consistent across the concurrency subsystem.
std::string openmpOpToString(OpenMPOpKind op);

} // namespace lotus::concurrency::OpenMP