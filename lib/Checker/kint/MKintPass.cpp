#include "Checker/kint/MKintPass.h"
#include "Checker/kint/Options.h"
#include "Checker/kint/Log.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/Support/raw_ostream.h>
#include <z3++.h>

using namespace llvm;

namespace kint {

MKintPass::MKintPass() : m_solver(llvm::None), m_function_timeout(FunctionTimeout) {
    m_range_analysis = std::make_unique<RangeAnalysis>();
    m_taint_analysis = std::make_unique<TaintAnalysis>();
    m_bug_detection = std::make_unique<BugDetection>();
    
    // Register bug types with BugReportMgr (Clearblue pattern)
    BugReportMgr& mgr = BugReportMgr::get_instance();
    m_intOverflowTypeId = mgr.register_bug_type("Integer Overflow", BugDescription::BI_HIGH,
                                                 BugDescription::BC_SECURITY, "CWE-190");
    m_divByZeroTypeId = mgr.register_bug_type("Divide by Zero", BugDescription::BI_MEDIUM,
                                               BugDescription::BC_ERROR, "CWE-369");
    m_badShiftTypeId = mgr.register_bug_type("Bad Shift", BugDescription::BI_MEDIUM,
                                              BugDescription::BC_ERROR, "Invalid shift amount");
    m_arrayOOBTypeId = mgr.register_bug_type("Array Out of Bounds", BugDescription::BI_HIGH,
                                              BugDescription::BC_SECURITY, "CWE-119, CWE-125");
    m_deadBranchTypeId = mgr.register_bug_type("Dead Branch", BugDescription::BI_LOW,
                                                BugDescription::BC_ERROR, "Unreachable code");
}

void MKintPass::backedge_analysis(const Function& F) {
    for (const auto& bb_ref : F) {
        auto bb = &bb_ref;
        if (m_backedges.count(bb) == 0) {
            // compute backedges of bb
            m_backedges[bb] = {};
            std::vector<const BasicBlock*> remote_succs { bb };
            while (!remote_succs.empty()) {
                auto cur_succ = remote_succs.back();
                remote_succs.pop_back();
                for (const auto succ : successors(cur_succ)) {
                    if (succ != bb && !m_backedges[bb].contains(succ)) {
                        m_backedges[bb].insert(succ);
                        remote_succs.push_back(succ);
                    }
                }
            }
        }
    }
}

PreservedAnalyses MKintPass::run(Module& M, ModuleAnalysisManager& MAM) {
    // Explicitly mark the unused parameters to avoid warnings
    (void)MAM;

    MKINT_LOG() << "Running MKint pass on module " << M.getName();
    
    // Apply the CheckAll flag if set to true
    if (CheckAll) {
        CheckIntOverflow = true;
        CheckDivByZero = true;
        CheckBadShift = true;
        CheckArrayOOB = true;
        CheckDeadBranch = true;
    }
    
    // Print checker configuration
    MKINT_LOG() << "Checker Configuration:";
    MKINT_LOG() << "  Integer Overflow: " << (CheckIntOverflow ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Division by Zero: " << (CheckDivByZero ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Bad Shift: " << (CheckBadShift ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Array Out of Bounds: " << (CheckArrayOOB ? "Enabled" : "Disabled");
    MKINT_LOG() << "  Dead Branch: " << (CheckDeadBranch ? "Enabled" : "Disabled");
    
    // Warn if no checkers are enabled
    if (!CheckIntOverflow && !CheckDivByZero && !CheckBadShift && !CheckArrayOOB && !CheckDeadBranch) {
        MKINT_WARN() << "No bug checkers are enabled. No bugs will be detected.";
        MKINT_WARN() << "Use --check-all=true or enable individual checkers with --check-<checker-name>=true";
    }

    // FIXME: This is a hack.
    auto ctx = new z3::context; // let it leak.
    m_solver = z3::solver(*ctx);

    // Mark taint sources.
    for (auto& F : M) {
        auto taint_sources = m_taint_analysis->get_taint_source(F);
        m_taint_analysis->mark_func_sinks(F, m_callback_tsrc_fn);
        if (TaintAnalysis::is_taint_src(F.getName()))
            m_func2tsrc[&F] = std::move(taint_sources);
    }

    // Propagate taint across functions
    m_taint_analysis->propagate_taint_across_functions(M, m_func2tsrc, m_taint_funcs);

    // Also add main function to analysis if it exists and is not already in taint_funcs
    for (auto& F : M) {
        if (!F.isDeclaration()) {
            backedge_analysis(F);
            // Add main function to analysis if it's not already there
            if (F.getName() == "main" && !m_taint_funcs.contains(&F)) {
                m_taint_funcs.insert(&F);
                MKINT_LOG() << "Added main function to analysis";
            }
        }
    }

    MKINT_LOG() << "Module after taint:";
    MKINT_LOG() << M;

    this->init_ranges(M);
    
    constexpr size_t max_try = 128;
    size_t try_count = 0;

    while (true) { // iterative range analysis.
        const auto old_fn_rng = m_func2range_info;
        const auto old_glb_rng = m_global2range;
        const auto old_glb_arrrng = m_garr2ranges;
        const auto old_fn_ret_rng = m_func2ret_range;

        for (auto F : m_range_analysis_funcs) {
            m_range_analysis->range_analysis(*F, m_func2range_info, m_backedges, 
                                           m_global2range, m_garr2ranges, m_func2ret_range,
                                           m_impossible_branches, m_gep_oob, m_func2tsrc, m_callback_tsrc_fn);
        }

        if (m_func2range_info == old_fn_rng && old_glb_rng == m_global2range && old_fn_ret_rng == m_func2ret_range
            && old_glb_arrrng == m_garr2ranges)
            break;
        if (++try_count > max_try) {
            MKINT_LOG() << "[Iterative Range Analysis] "
                        << "Max try " << max_try << " reached, aborting.";
            break;
        }
    }
    
    this->pring_all_ranges();

    this->smt_solving(M);

    m_bug_detection->mark_errors(m_impossible_branches, m_gep_oob, 
                                m_overflow_insts, m_bad_shift_insts, m_div_zero_insts);

    // Report bugs to BugReportMgr (Clearblue pattern)
    reportBugsToManager();

    // Note: SARIF/JSON output is now handled centrally by BugReportMgr
    // in the tool driver, not by individual checkers

    return PreservedAnalyses::all();
}

void MKintPass::init_ranges(Module& M) {
    m_range_analysis->init_ranges(M, m_func2range_info, m_func2ret_range, 
                                 m_range_analysis_funcs, m_global2range, m_garr2ranges,
                                 m_taint_funcs, m_callback_tsrc_fn);
}

void MKintPass::pring_all_ranges() const {
    m_range_analysis->print_all_ranges(m_func2ret_range, m_global2range, m_garr2ranges,
                                      m_func2range_info, m_impossible_branches, m_gep_oob);
}

void MKintPass::smt_solving(Module& /*M*/) {
    for (auto F : m_taint_funcs) {
        if (F->isDeclaration())
            continue;

        // Record start time for this function
        m_function_start_time = std::chrono::steady_clock::now();
        MKINT_LOG() << "Beginning analysis of function " << F->getName();

        // Get a path tree.
        for (auto& bb : F->getBasicBlockList()) {
            for (const auto& pred : predecessors(&bb)) {
                if (m_backedges[&bb].contains(pred) || &bb == pred)
                    continue;

                m_bbpaths[pred].push_back(&bb);
            }
        }

        m_solver.getValue().push();
        // add function arg constraints.
        for (auto& arg : F->args()) {
            if (!arg.getType()->isIntegerTy())
                continue;
            const auto arg_name = F->getName() + "." + std::to_string(arg.getArgNo());
            const auto argv
                = m_solver.getValue().ctx().bv_const(arg_name.str().c_str(), arg.getType()->getIntegerBitWidth());
            m_v2sym[&arg] = argv;
            m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(&arg, &(F->getEntryBlock()), m_func2range_info), argv, m_solver.getValue());
        }

        path_solving(&(F->getEntryBlock()), nullptr);
        m_solver.getValue().pop();
        
        // Report analysis time
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_function_start_time).count();
        MKINT_LOG() << "Completed analysis of function " << F->getName() 
                   << " in " << elapsed << " seconds";
    }
}

void MKintPass::path_solving(BasicBlock* cur, BasicBlock* pred) {
    // Check for timeout
    if (m_function_timeout > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_function_start_time).count();
        if (elapsed > static_cast<int64_t>(m_function_timeout)) {
            MKINT_WARN() << "Timeout reached for function " << cur->getParent()->getName() 
                         << " after " << elapsed << " seconds. Analysis incomplete.";
            return;
        }
    }
    
    if (m_backedges[cur].contains(pred))
        return;

    // Track this basic block in the current execution path
    std::string bbDesc = "Basic block ";
    if (cur->hasName()) {
        bbDesc += cur->getName().str();
    } else {
        bbDesc += "<unnamed>";
    }
    if (cur->getParent()) {
        bbDesc += " in function " + cur->getParent()->getName().str();
    }
    
    PathPoint pathPoint(cur, nullptr, bbDesc);
    m_bug_detection->addPathPoint(pathPoint);

    auto cur_brng = m_func2range_info[cur->getParent()][cur];

    if (nullptr != pred) {
        auto terminator = pred->getTerminator();
        auto br = dyn_cast<BranchInst>(terminator);
        if (br) {
            if (br->isConditional()) {
                if (auto cmp = dyn_cast<ICmpInst>(br->getCondition())) {
                    // br: a op b == true or false
                    // makeAllowedICmpRegion turning a op b into a range.
                    auto lhs = cmp->getOperand(0);
                    auto rhs = cmp->getOperand(1);

                    if (!lhs->getType()->isIntegerTy() || !rhs->getType()->isIntegerTy()) {
                        // This should be covered by `ICmpInst`.
                        MKINT_WARN() << "The br operands are not both integers: " << *cmp;
                    } else {
                        bool is_true_br = br->getSuccessor(0) == cur;

                        // Skip impossible branch check if checker is disabled
                        if (CheckDeadBranch && m_impossible_branches.count(cmp) && m_impossible_branches[cmp] == is_true_br) {
                            return;
                        }

                        const auto get_tbr_assert = [lhs, rhs, cmp, this]() -> z3::expr {
                            switch (cmp->getPredicate()) {
                            case ICmpInst::ICMP_EQ: // =
                                return m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()) == m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue());
                            case ICmpInst::ICMP_NE: // !=
                                return m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()) != m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue());
                            case ICmpInst::ICMP_SGT: // singed >
                                return z3::sgt(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            case ICmpInst::ICMP_SGE: // singed >=
                                return z3::sge(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            case ICmpInst::ICMP_SLT: // singed <
                                return z3::slt(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            case ICmpInst::ICMP_SLE: // singed <=
                                return z3::sle(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            case ICmpInst::ICMP_UGT: // unsigned >
                                return z3::ugt(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            case ICmpInst::ICMP_UGE: // unsigned >=
                                return z3::uge(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            case ICmpInst::ICMP_ULT: // unsigned <
                                return z3::ult(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            case ICmpInst::ICMP_ULE: // unsigned <=
                                return z3::ule(m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()), m_bug_detection->v2sym(rhs, m_v2sym, m_solver.getValue()));
                            default:
                                MKINT_CHECK_ABORT(false) << "unsupported icmp predicate: " << *cmp;
                                // Add a default return to satisfy compiler
                                return m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()) == m_bug_detection->v2sym(lhs, m_v2sym, m_solver.getValue()); // Always true expression as a fallback
                            }
                        };

                        const auto check = [cmp, is_true_br, this] {
                            if (m_solver.getValue().check() == z3::unsat) { // counter example
                                MKINT_WARN() << "[SMT Solving] cannot continue " << (is_true_br ? "true" : "false")
                                             << " branch of " << *cmp;
                                return false;
                            }
                            return true;
                        };

                        if (is_true_br) { // T branch
                            m_solver.getValue().add(get_tbr_assert());
                            if (!check())
                                return;
                            m_v2sym[cmp] = m_solver.getValue().ctx().bv_val(true, 1);
                            
                            // Add branch decision to path
                            std::string branchDesc = "Taking true branch from condition: ";
                            llvm::raw_string_ostream brOS(branchDesc);
                            brOS << *cmp;
                            PathPoint branchPoint(pred, cmp, brOS.str());
                            m_bug_detection->addPathPoint(branchPoint);
                        } else { // F branch
                            m_solver.getValue().add(!get_tbr_assert());
                            if (!check())
                                return;
                            m_v2sym[cmp] = m_solver.getValue().ctx().bv_val(false, 1);
                            
                            // Add branch decision to path
                            std::string branchDesc = "Taking false branch from condition: ";
                            llvm::raw_string_ostream brOS(branchDesc);
                            brOS << *cmp;
                            PathPoint branchPoint(pred, cmp, brOS.str());
                            m_bug_detection->addPathPoint(branchPoint);
                        }
                    }
                }
            }
        } else if (auto swt = dyn_cast<SwitchInst>(terminator)) {
            auto cond = swt->getCondition();
            if (cond->getType()->isIntegerTy()) {
                if (swt->getDefaultDest() == cur) { // default
                    // not (all)
                    for (auto c : swt->cases()) {
                        auto case_val = c.getCaseValue();
                        m_solver.getValue().add(m_bug_detection->v2sym(cond, m_v2sym, m_solver.getValue())
                            != m_solver.getValue().ctx().bv_val(
                                case_val->getZExtValue(), cond->getType()->getIntegerBitWidth()));
                    }
                } else {
                    for (auto c : swt->cases()) {
                        if (c.getCaseSuccessor() == cur) {
                            auto case_val = c.getCaseValue();
                            m_solver.getValue().add(m_bug_detection->v2sym(cond, m_v2sym, m_solver.getValue())
                                == m_solver.getValue().ctx().bv_val(
                                    case_val->getZExtValue(), cond->getType()->getIntegerBitWidth()));
                            break;
                        }
                    }
                }
            }
        } else {
            // try catch... (thank god, C does not have try-catch)
            // indirectbr... ?
            MKINT_CHECK_ABORT(false) << "Unknown terminator: " << *pred->getTerminator();
        }
    }

    for (auto& inst : cur->getInstList()) {
        if (!cur_brng.count(&inst) || !inst.getType()->isIntegerTy())
            continue;

        if (auto op = dyn_cast<BinaryOperator>(&inst)) {
            m_bug_detection->binary_check(op, m_solver.getValue(), m_v2sym, 
                                        m_overflow_insts, m_bad_shift_insts, m_div_zero_insts);
            m_v2sym[op] = m_bug_detection->binary_op_propagate(op, m_v2sym, m_solver.getValue());
            if (!m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(&inst, inst.getParent(), m_func2range_info), m_bug_detection->v2sym(&inst, m_v2sym, m_solver.getValue()), m_solver.getValue()))
                return;
        } else if (auto op = dyn_cast<CastInst>(&inst)) {
            m_v2sym[op] = m_bug_detection->cast_op_propagate(op, m_v2sym, m_solver.getValue());
            if (!m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(&inst, inst.getParent(), m_func2range_info), m_bug_detection->v2sym(&inst, m_v2sym, m_solver.getValue()), m_solver.getValue()))
                return;
        } else {
            const auto name = "\%vid" + std::to_string(inst.getValueID());
            m_v2sym[&inst] = m_solver.getValue().ctx().bv_const(name.c_str(), inst.getType()->getIntegerBitWidth());
            if (!m_bug_detection->add_range_cons(m_range_analysis->get_range_by_bb(&inst, inst.getParent(), m_func2range_info), m_bug_detection->v2sym(&inst, m_v2sym, m_solver.getValue()), m_solver.getValue()))
                return;
        }
    }

    for (auto succ : m_bbpaths[cur]) {
        m_solver.getValue().push();
        path_solving(succ, cur);
        m_solver.getValue().pop();
    }
    
    // Pop the current basic block from the path when backtracking
    auto currentPath = m_bug_detection->getCurrentPath();
    if (!currentPath.empty()) {
        currentPath.pop_back();
        m_bug_detection->setCurrentPath(currentPath);
    }
}


std::string MKintPass::get_bb_label(const BasicBlock* bb) {
    // Check if func and module are available, avoiding 'Segmentfault'
    if (!bb || !bb->getParent() || bb->getParent()->getName().empty() || !bb->getParent()->getParent()) return "<badref>";
    std::string str;
    llvm::raw_string_ostream os(str);
    bb->printAsOperand(os, false);
    return str;
}

void MKintPass::generateSarifReport(const std::string& filename) {
    if (m_bug_detection) {
        m_bug_detection->generateSarifReport(filename, m_impossible_branches, m_gep_oob,
                                            m_overflow_insts, m_bad_shift_insts, m_div_zero_insts);
    }
}

} // namespace kint
