// Implementation of bug reporting methods for MKintPass
// This file adds Clearblue-style bug reporting to BugReportMgr

#include "Checker/kint/MKintPass.h"
#include "Checker/kint/Options.h"
#include "Checker/kint/Log.h"

namespace kint {

void MKintPass::reportBugsToManager() {
    const auto& bug_paths = m_bug_detection->getBugPaths();
    
    MKINT_LOG() << "Reporting " << bug_paths.size() << " bugs to BugReportMgr";
    
    for (const auto& pair : bug_paths) {
        const Instruction* inst = pair.first;
        const BugPath& bug_path = pair.second;
        reportBug(bug_path.bugType, inst, bug_path.path);
    }
    
    // Also report simple bugs without paths
    for (const auto* inst : m_overflow_insts) {
        reportBug(interr::INT_OVERFLOW, inst);
    }
    
    for (const auto* inst : m_div_zero_insts) {
        reportBug(interr::DIV_BY_ZERO, inst);
    }
    
    for (const auto* inst : m_bad_shift_insts) {
        reportBug(interr::BAD_SHIFT, inst);
    }
    
    for (const auto* gep : m_gep_oob) {
        reportBug(interr::ARRAY_OOB, gep);
    }
    
    for (const auto& pair : m_impossible_branches) {
        const ICmpInst* cmp = pair.first;
        bool is_true_branch = pair.second;
        reportBug(is_true_branch ? interr::DEAD_TRUE_BR : interr::DEAD_FALSE_BR, cmp);
    }
}

void MKintPass::reportBug(interr bug_type, const Instruction* inst, const std::vector<PathPoint>& path) {
    if (bug_type == interr::NONE) return;
    
    // Determine bug type ID and description
    int bug_type_id;
    std::string main_desc;
    
    switch (bug_type) {
        case interr::INT_OVERFLOW:
            if (!CheckIntOverflow) return;
            bug_type_id = m_intOverflowTypeId;
            main_desc = "Integer overflow detected";
            break;
        case interr::DIV_BY_ZERO:
            if (!CheckDivByZero) return;
            bug_type_id = m_divByZeroTypeId;
            main_desc = "Division by zero detected";
            break;
        case interr::BAD_SHIFT:
            if (!CheckBadShift) return;
            bug_type_id = m_badShiftTypeId;
            main_desc = "Invalid shift amount detected";
            break;
        case interr::ARRAY_OOB:
            if (!CheckArrayOOB) return;
            bug_type_id = m_arrayOOBTypeId;
            main_desc = "Array out of bounds access detected";
            break;
        case interr::DEAD_TRUE_BR:
        case interr::DEAD_FALSE_BR:
            if (!CheckDeadBranch) return;
            bug_type_id = m_deadBranchTypeId;
            main_desc = (bug_type == interr::DEAD_TRUE_BR) ? 
                "Dead true branch detected" : "Dead false branch detected";
            break;
        default:
            return;
    }
    
    // Create bug report
    BugReport* report = new BugReport(bug_type_id);
    
    // Add path trace if available
    if (!path.empty()) {
        for (const auto& point : path) {
            if (point.inst) {
                report->append_step(const_cast<Instruction*>(point.inst), point.description);
            }
        }
    }
    
    // Add the bug instruction as the final step
    report->append_step(const_cast<Instruction*>(inst), main_desc);
    
    // Set confidence score (SMT-based results are high confidence)
    report->set_conf_score(85);
    
    // Report to manager
    BugReportMgr::get_instance().insert_report(bug_type_id, report);
}

} // namespace kint

