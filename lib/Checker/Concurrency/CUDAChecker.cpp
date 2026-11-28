#include "Checker/Concurrency/CUDAChecker.h"

#include <set>

using namespace llvm;

namespace concurrency {

CUDAChecker::CUDAChecker(Module &module, cuda::CUDAAnalysis *analysis)
    : m_module(module), m_analysis(analysis) {}

void CUDAChecker::ensureAnalysis() {
  if (m_analysis) {
    return;
  }
  m_owned_analysis = std::make_unique<cuda::CUDAAnalysis>(m_module);
  m_owned_analysis->runAnalysis();
  m_analysis = m_owned_analysis.get();
}

std::vector<ConcurrencyBugReport> CUDAChecker::checkCUDABugs() {
  ensureAnalysis();
  std::vector<ConcurrencyBugReport> reports;
  if (!m_analysis) {
    return reports;
  }

  std::set<std::tuple<const llvm::Instruction *, const llvm::Instruction *,
                      cuda::MemorySpace, cuda::RaceKind>>
      emitted_races;

  for (const cuda::KernelSummary &summary : m_analysis->getKernelSummaries()) {
    if (summary.has_warp_divergence) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_WARP_DIVERGENCE,
          "CUDA warp divergence from thread-dependent control flow",
          BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE);
      for (const auto &region : summary.divergence_regions) {
        report.addStep(
            region.branch,
            region.depends_on_lane_id
                ? "Lane-dependent branch splits a warp execution path"
                : (region.depends_on_block_idx
                       ? "Block/thread-dependent branch diverges across kernel "
                         "instances"
                       : "Thread-index-dependent branch splits a warp "
                         "execution path"));
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_shared_race) {
      ConcurrencyBugReport report(ConcurrencyBugType::CUDA_SHARED_MEMORY_RACE,
                                  "Potential shared-memory race inside a block",
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);
      ConcurrencyBugReport ordering(
          ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK,
          "CUDA shared-memory ordering/fence risk", BugDescription::BI_MEDIUM,
          BugDescription::BC_WARNING);
      for (const auto &race : summary.shared_races) {
        if (!emitted_races
                 .emplace(race.first, race.second, race.space, race.kind)
                 .second) {
          continue;
        }
        if (race.kind == cuda::RaceKind::MissingFence ||
            race.kind == cuda::RaceKind::AtomicOrderingRisk) {
          ordering.addStep(
              race.first,
              std::string(cuda::CUDAAnalysis::toString(race.kind)) + " at " +
                  cuda::CUDAAnalysis::toString(race.scope) + " scope");
          ordering.addStep(
              race.second,
              race.ordering_reason
                  ? race.ordering_reason
                  : "Ordering is insufficient for shared communication");
          continue;
        }
        report.addStep(
            race.first,
            std::string("Conflicting shared-memory access participates in a ") +
                cuda::CUDAAnalysis::toString(race.kind) + " at " +
                cuda::CUDAAnalysis::toString(race.scope) + " scope");
        report.addStep(race.second, race.ordering_reason
                                        ? race.ordering_reason
                                        : "Another block-local thread may "
                                          "access the same shared location");
      }
      if (!report.steps.empty()) {
        reports.push_back(std::move(report));
      }
      if (!ordering.steps.empty()) {
        reports.push_back(std::move(ordering));
      }

      bool has_symbolic = llvm::any_of(
          summary.shared_races, [](const auto &race) { return race.symbolic; });
      if (has_symbolic) {
        ConcurrencyBugReport symbolic(
            ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK,
            "Shared-memory race depends on symbolic thread/block parameters",
            BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
        for (const auto &race : summary.shared_races) {
          if (!race.symbolic) {
            continue;
          }
          symbolic.addStep(race.first, "First conflicting access remains "
                                       "feasible under symbolic block sizing");
          symbolic.addStep(race.second, "Second conflicting access aliases "
                                        "under a parametric thread pair");
        }
        if (!symbolic.steps.empty()) {
          reports.push_back(std::move(symbolic));
        }
      }
    }

    if (summary.has_global_race) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_GLOBAL_MEMORY_RACE,
          "Potential global/device-memory race across CUDA threads",
          BugDescription::BI_HIGH, BugDescription::BC_ERROR);
      ConcurrencyBugReport ordering(
          ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK,
          "CUDA global/device ordering risk", BugDescription::BI_MEDIUM,
          BugDescription::BC_WARNING);
      for (const auto &race : summary.global_races) {
        if (!emitted_races
                 .emplace(race.first, race.second, race.space, race.kind)
                 .second) {
          continue;
        }
        if (race.kind == cuda::RaceKind::MissingFence ||
            race.kind == cuda::RaceKind::AtomicOrderingRisk) {
          ordering.addStep(
              race.first,
              std::string(cuda::CUDAAnalysis::toString(race.kind)) + " at " +
                  cuda::CUDAAnalysis::toString(race.scope) + " scope");
          ordering.addStep(
              race.second,
              race.ordering_reason
                  ? race.ordering_reason
                  : "Global/device communication lacks required ordering");
          continue;
        }
        report.addStep(race.first,
                       std::string(cuda::CUDAAnalysis::toString(race.kind)) +
                           " at " + cuda::CUDAAnalysis::toString(race.scope) +
                           " scope");
        report.addStep(race.second, race.ordering_reason
                                        ? race.ordering_reason
                                        : "Second conflicting access aliases "
                                          "the same global/device object");
      }
      if (!report.steps.empty()) {
        reports.push_back(std::move(report));
      }
      if (!ordering.steps.empty()) {
        reports.push_back(std::move(ordering));
      }

      bool has_symbolic = llvm::any_of(
          summary.global_races, [](const auto &race) { return race.symbolic; });
      if (has_symbolic) {
        ConcurrencyBugReport symbolic(
            ConcurrencyBugType::CUDA_PARAMETRIC_RACE_RISK,
            "Global/device-memory race depends on symbolic grid/block "
            "parameters",
            BugDescription::BI_MEDIUM, BugDescription::BC_WARNING);
        for (const auto &race : summary.global_races) {
          if (!race.symbolic) {
            continue;
          }
          symbolic.addStep(race.first,
                           race.cross_block
                               ? "Conflicting access remains feasible for "
                                 "symbolic block pairs"
                               : "Conflicting access remains feasible for "
                                 "symbolic thread pairs");
          symbolic.addStep(race.second,
                           "Alias survives parametric CUDA launch dimensions");
        }
        if (!symbolic.steps.empty()) {
          reports.push_back(std::move(symbolic));
        }
      }
    }

    if (summary.has_barrier_mismatch) {
      ConcurrencyBugReport report(ConcurrencyBugType::CUDA_BARRIER_MISMATCH,
                                  "Potential CUDA barrier mismatch or deadlock",
                                  BugDescription::BI_HIGH,
                                  BugDescription::BC_ERROR);
      for (const auto &mismatch : summary.barrier_mismatches) {
        report.addStep(
            mismatch.branch,
            "Divergent branch controls whether threads reach a barrier");
        report.addStep(
            mismatch.barrier,
            "Barrier is nested in only part of the divergent region");
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_bank_conflict) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_BANK_CONFLICT,
          "Shared-memory bank conflict detected from per-lane bank mapping",
          BugDescription::BI_MEDIUM, BugDescription::BC_PERFORMANCE);
      for (const auto &conflict : summary.bank_conflicts) {
        std::string description =
            "Shared-memory access maps multiple lanes to the same bank with "
            "estimated conflict degree " +
            std::to_string(conflict.conflict_degree) + ", touching " +
            std::to_string(conflict.unique_banks) + " unique banks";
        report.addStep(conflict.inst, description);
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_uncoalesced_access) {
      ConcurrencyBugReport report(ConcurrencyBugType::CUDA_UNCOALESCED_ACCESS,
                                  "Global-memory access is not fully coalesced",
                                  BugDescription::BI_MEDIUM,
                                  BugDescription::BC_PERFORMANCE);
      for (const auto &issue : summary.coalescing_issues) {
        std::string description =
            "Warp access is " +
            std::string(cuda::CUDAAnalysis::toString(issue.quality)) +
            " with about " + std::to_string(issue.estimated_transactions) +
            " memory transactions over " + std::to_string(issue.covered_bytes) +
            " bytes";
        report.addStep(issue.inst, description);
      }
      reports.push_back(std::move(report));
    }

    if (summary.has_volatile_missing) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_VOLATILE_MISSING,
          "Potential missing volatile qualifier on inter-thread CUDA memory",
          BugDescription::BI_LOW, BugDescription::BC_WARNING);
      for (const auto &missing : summary.volatile_missing) {
        report.addStep(
            missing.inst,
            "Non-atomic inter-thread memory is accessed without volatile");
      }
      reports.push_back(std::move(report));
    }

    if (summary.dimensions.hasSymbolicGrid() ||
        summary.dimensions.hasSymbolicBlock()) {
      ConcurrencyBugReport report(
          ConcurrencyBugType::CUDA_SYMBOLIC_CONFIG_RISK,
          "CUDA launch configuration is symbolic or parameterized",
          BugDescription::BI_LOW, BugDescription::BC_WARNING);
      if (summary.kernel && !summary.kernel->empty() &&
          !summary.kernel->front().empty()) {
        report.addStep(
            &summary.kernel->front().front(),
            "Kernel is analyzed under symbolic grid/block dimensions");
      }
      reports.push_back(std::move(report));
    }

    for (const cuda::AccessInfo &access : summary.accesses) {
      if ((!access.exact_space || access.space == cuda::MemorySpace::Unknown) &&
          access.inst) {
        ConcurrencyBugReport report(
            ConcurrencyBugType::CUDA_SHARED_GLOBAL_SPACE_MISMATCH,
            "CUDA memory space could not be classified precisely",
            BugDescription::BI_LOW, BugDescription::BC_WARNING);
        report.addStep(access.inst, "Address space was not resolved exactly to "
                                    "shared/global/local/constant");
        reports.push_back(std::move(report));
        break;
      }
    }
  }

  for (const auto &race : m_analysis->getInterKernelRaces()) {
    ConcurrencyBugReport report(
        ConcurrencyBugType::CUDA_INTER_KERNEL_HAZARD,
        "Potential inter-kernel global/device-memory hazard",
        BugDescription::BI_HIGH, BugDescription::BC_ERROR);
    report.addStep(
        race.first_launch,
        std::string("Launch of kernel '") +
            (race.first_kernel ? race.first_kernel->getName().str()
                               : "<unknown>") +
            "' is " +
            (race.ordering_reason ? race.ordering_reason : "unordered"));
    report.addStep(
        race.second_launch,
        std::string("Launch of kernel '") +
            (race.second_kernel ? race.second_kernel->getName().str()
                                : "<unknown>") +
            "' may access the same memory without device ordering (" +
            std::string(cuda::CUDAAnalysis::toString(race.ordering_source)) +
            ")");
    reports.push_back(std::move(report));
  }

  return reports;
}

} // namespace concurrency
