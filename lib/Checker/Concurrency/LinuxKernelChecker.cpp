#include "Checker/Concurrency/LinuxKernelChecker.h"

using namespace llvm;

namespace concurrency {

LinuxKernelChecker::LinuxKernelChecker(Module &module,
                                       kernel::LinuxKernelAnalysis *analysis)
    : m_module(module), m_analysis(analysis) {}

void LinuxKernelChecker::ensureAnalysis() {
  if (m_analysis) {
    return;
  }
  m_ownedAnalysis = std::make_unique<kernel::LinuxKernelAnalysis>(m_module);
  m_ownedAnalysis->runAnalysis();
  m_analysis = m_ownedAnalysis.get();
}

std::vector<ConcurrencyBugReport> LinuxKernelChecker::checkLinuxKernelBugs() {
  ensureAnalysis();

  std::vector<ConcurrencyBugReport> reports;
  if (!m_analysis) {
    return reports;
  }

  const auto &results = m_analysis->getResults();

  auto emitPair = [&](ConcurrencyBugType type, const std::string &desc,
                      BugDescription::BugImportance imp,
                      const std::pair<const Instruction *, const Instruction *> &p,
                      const std::string &step1, const std::string &step2) {
    ConcurrencyBugReport report(type, desc, imp, BugDescription::BC_ERROR);
    report.addStep(p.first, step1);
    if (p.second && p.second != p.first) {
      report.addStep(p.second, step2);
    }
    reports.push_back(std::move(report));
  };

  auto emitSingle = [&](ConcurrencyBugType type, const std::string &desc,
                        BugDescription::BugImportance imp,
                        const Instruction *inst, const std::string &step) {
    ConcurrencyBugReport report(type, desc, imp, BugDescription::BC_ERROR);
    report.addStep(inst, step);
    reports.push_back(std::move(report));
  };

  for (const auto &p : results.lock_deadlocks) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_DEADLOCK,
             "Potential Linux kernel lock deadlock", BugDescription::BI_HIGH, p,
             "First conflicting kernel lock acquisition",
             "Second conflicting kernel lock acquisition");
  }
  for (const Instruction *inst : results.double_locks) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_DOUBLE_LOCK,
               "Linux kernel double lock on non-recursive lock",
               BugDescription::BI_HIGH, inst, "Repeated lock acquisition");
  }
  for (const Instruction *inst : results.unlock_without_lock) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_UNLOCK_WITHOUT_LOCK,
               "Linux kernel unlock without holding lock",
               BugDescription::BI_HIGH, inst, "Unmatched unlock");
  }
  for (const Instruction *inst : results.lock_without_unlock) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_LOCK_WITHOUT_UNLOCK,
               "Linux kernel lock acquired without matching unlock",
               BugDescription::BI_MEDIUM, inst, "Unbalanced lock acquisition");
  }
  for (const auto &p : results.lock_order_inversions) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_LOCK_ORDER_INVERSION,
             "Linux kernel lock ordering inversion", BugDescription::BI_HIGH, p,
             "Lock order acquisition 1", "Lock order acquisition 2");
  }
  for (const Instruction *inst : results.sleep_in_atomic) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_SLEEP_IN_ATOMIC,
               "Sleeping operation invoked inside Linux kernel atomic/spinlock context",
               BugDescription::BI_HIGH, inst, "Potentially sleeping call in atomic context");
  }
  for (const Instruction *inst : results.mix_raw_and_cooked) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_MIX_RAW_COOKED,
               "Linux kernel raw spinlock mixed with standard spinlock/sleepable lock",
               BugDescription::BI_HIGH, inst, "Mixed raw and cooked lock usage");
  }
  for (const Instruction *inst : results.irq_save_restore_issues) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_IRQ_SAVE_RESTORE,
               "Linux kernel IRQ save/restore mismatch",
               BugDescription::BI_HIGH, inst, "Mismatched IRQ state operation");
  }
  for (const auto &p : results.data_races) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_DATA_RACE,
             "Potential Linux kernel memory-model data race",
             BugDescription::BI_HIGH, p, "Conflicting kernel memory access 1",
             "Conflicting kernel memory access 2");
  }
  for (const Instruction *inst : results.rcu_without_grace_period) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_RCU_ERROR,
               "RCU update/reclamation without grace period synchronization",
               BugDescription::BI_HIGH, inst, "Missing RCU grace period");
  }
  for (const auto &p : results.rcu_conflicts) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_RCU_ERROR,
             "Conflicting RCU read-side and update operations",
             BugDescription::BI_HIGH, p, "RCU operation 1", "RCU operation 2");
  }
  for (const Instruction *inst : results.rcu_double_free) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_RCU_ERROR,
               "Potential RCU double free / duplicate reclamation",
               BugDescription::BI_HIGH, inst, "Duplicate RCU reclamation");
  }
  for (const Instruction *inst : results.deref_after_free) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_RCU_ERROR,
               "RCU pointer dereferenced after reclamation",
               BugDescription::BI_HIGH, inst, "RCU dereference after free");
  }
  for (const Instruction *inst : results.rcu_unsafe_reclamation) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_RCU_ERROR,
               "Unsafe RCU reclamation inside read-side critical section",
               BugDescription::BI_HIGH, inst, "Unsafe RCU reclamation");
  }
  for (const auto &p : results.missing_wake_ups) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_WAIT_ERROR,
             "Linux kernel wait without matching wake-up",
             BugDescription::BI_MEDIUM, p, "Wait operation", "Related wake/wait");
  }
  for (const auto &p : results.spurious_wake_ups) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_WAIT_ERROR,
             "Linux kernel potential spurious wake-up",
             BugDescription::BI_MEDIUM, p, "Wait operation", "Related operation");
  }
  for (const auto &p : results.lost_wakeups) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_WAIT_ERROR,
             "Linux kernel race between wait and wake-up (lost wake-up)",
             BugDescription::BI_HIGH, p, "Wait operation", "Wake-up operation");
  }
  for (const Instruction *inst : results.missing_completion) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_WAIT_ERROR,
               "Linux kernel wait_for_completion without matching complete()",
               BugDescription::BI_MEDIUM, inst, "Unmatched completion wait");
  }
  for (const Instruction *inst : results.double_completion) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_WAIT_ERROR,
               "Linux kernel duplicate completion signaling",
               BugDescription::BI_MEDIUM, inst, "Duplicate complete() call");
  }
  for (const Instruction *inst : results.timer_not_deleted) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_TIMER_ERROR,
               "Linux kernel timer armed without matching deletion",
               BugDescription::BI_MEDIUM, inst, "Armed kernel timer");
  }
  for (const Instruction *inst : results.timer_use_after_delete) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_TIMER_ERROR,
               "Linux kernel timer used after deletion",
               BugDescription::BI_HIGH, inst, "Timer use after delete");
  }
  for (const Instruction *inst : results.use_after_free) {
    emitSingle(ConcurrencyBugType::LINUX_KERNEL_LIFETIME_ERROR,
               "Linux kernel potential use-after-free in concurrent lifetime",
               BugDescription::BI_HIGH, inst, "Potential use-after-free");
  }
  for (const auto &p : results.async_lifetime_hazards) {
    emitPair(ConcurrencyBugType::LINUX_KERNEL_LIFETIME_ERROR,
             "Linux kernel asynchronous work/callback lifetime hazard",
             BugDescription::BI_HIGH, p, "Asynchronous scheduling site",
             "Conflicting lifetime teardown");
  }

  return reports;
}

} // namespace concurrency
