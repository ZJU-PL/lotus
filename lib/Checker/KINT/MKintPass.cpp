#include "Checker/KINT/MKintPass.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <functional>
#include <optional>

#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <z3++.h>

using namespace llvm;

namespace kint {

MKintPass::MKintPass()
    : m_solver(std::nullopt), m_function_timeout(FunctionTimeout),
      m_path_limit(MaxPathsPerFunction) {
  m_taint_analysis = std::make_unique<TaintAnalysis>();
  m_bug_detection = std::make_unique<BugDetection>();

  // Register bug types with BugReportMgr (shared pattern)
  BugReportMgr &mgr = BugReportMgr::get_instance();
  m_intOverflowTypeId =
      mgr.register_bug_type("Integer Overflow", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-190");
  m_divByZeroTypeId =
      mgr.register_bug_type("Divide by Zero", BugDescription::BI_MEDIUM,
                            BugDescription::BC_ERROR, "CWE-369");
  m_badShiftTypeId =
      mgr.register_bug_type("Bad Shift", BugDescription::BI_MEDIUM,
                            BugDescription::BC_ERROR, "Invalid shift amount");
  m_arrayOOBTypeId =
      mgr.register_bug_type("Array Out of Bounds", BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-119, CWE-125");
  m_deadBranchTypeId =
      mgr.register_bug_type("Dead Branch", BugDescription::BI_LOW,
                            BugDescription::BC_ERROR, "Unreachable code");
}

void MKintPass::backedge_analysis(const Function &F) {
  // Compute true loop backedges using a DFS-based algorithm.
  // An edge pred->succ is a backedge iff succ dominates pred (i.e., succ is
  // an ancestor of pred in the DFS spanning tree).
  // We store, for each block B, the set of predecessors P such that P->B is
  // a backedge.  The existing usage is: m_backedges[cur].contains(pred).

  // Initialize all entries so every block has an (empty) set.
  for (const auto &bb_ref : F) {
    const auto *bb = &bb_ref;
    if (m_backedges.count(bb) == 0)
      m_backedges[bb] = {};
  }

  // DFS colouring: 0 = white (unvisited), 1 = grey (on stack), 2 = black
  // (done).
  DenseMap<const BasicBlock *, int> color;
  std::vector<std::pair<const BasicBlock *, bool>> stack; // (block, entered)
  stack.push_back(std::make_pair(&F.getEntryBlock(), false));

  while (!stack.empty()) {
    const BasicBlock *bb = stack.back().first;
    const bool leaving = stack.back().second;
    stack.pop_back();

    if (leaving) {
      color[bb] = 2; // black
      continue;
    }

    if (color[bb] == 1)
      continue; // already on stack (cycle detected earlier)
    if (color[bb] == 2)
      continue; // already fully processed

    color[bb] = 1;                             // grey: on the DFS stack
    stack.push_back(std::make_pair(bb, true)); // push "leaving" marker

    for (const auto *succ : successors(bb)) {
      if (color[succ] == 1) {
        // succ is an ancestor in the DFS tree -> bb->succ is a backedge.
        m_backedges[succ].insert(bb);
      } else if (color[succ] == 0) {
        stack.push_back(std::make_pair(succ, false));
      }
    }
  }
}

void MKintPass::ensureBackedges(const Function &F) {
  if (F.isDeclaration() || F.empty())
    return;
  if (m_backedges_computed.insert(&F).second) {
    backedge_analysis(F);
  }
}

void MKintPass::resetSmtState() {
  // Destroy every Z3-holding member BEFORE releasing m_ctx.
  m_summary_cache.clear();
  m_building_summary = nullptr;
  m_obj_change_log.clear();
  m_obj_change_frames.clear();
  m_sym_change_log.clear();
  m_sym_change_frames.clear();
  m_path_constraints.clear();
  m_constraint_frames.clear();
  m_universal_vars.clear();
  m_universal_var_ids.clear();
  m_obj_base.clear();
  m_obj_size.clear();
  m_obj_list.clear();
  m_obj_mem.clear();
  m_obj_alias.clear();
  m_int_alias.clear();
  m_ptr_offset.clear();
  m_int_offset.clear();
  m_obj_freed.clear();
  m_obj_size_known.clear();
  m_v2sym.clear();
  m_bbpaths.clear();
  m_smt_mem.reset();
  m_solver.reset();
  m_ctx.reset();
}

PreservedAnalyses MKintPass::run(Module &M, ModuleAnalysisManager &MAM) {
  MKINT_LOG() << "Running MKint pass on module " << M.getName();

  // Refresh performance options (they may change across runs).
  m_function_timeout = FunctionTimeout;
  m_path_limit = MaxPathsPerFunction;
  m_robust_reachability = RobustReachability;
  m_dump_ef_path = DumpEFConstraints;
  m_robust_universal_unknown_loads = RobustUniversalUnknownLoads;
  m_robust_universal_external_globals = RobustUniversalExternalGlobals;
  m_robust_universal_inline_asm = RobustUniversalInlineAsm;
  m_summary_timeout = SummaryTimeout;
  m_summary_path_limit = SummaryMaxPathsPerFunction;
  m_max_loop_unroll = MaxLoopUnroll;
  parseRobustBugFilter(RobustChecks);

  // Print checker configuration
  MKINT_LOG() << "Checker Configuration:";
  MKINT_LOG() << "  Integer Overflow: "
              << (CheckIntOverflow ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Division by Zero: "
              << (CheckDivByZero ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Bad Shift: " << (CheckBadShift ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Array Out of Bounds: "
              << (CheckArrayOOB ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Dead Branch: "
              << (CheckDeadBranch ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Robust Reachability: "
              << (m_robust_reachability ? "Enabled" : "Disabled");
  if (!m_dump_ef_path.empty()) {
    MKINT_LOG() << "  Dump EF Constraints: " << m_dump_ef_path;
  }
  if (m_robust_reachability) {
    MKINT_LOG() << "  Robust Universals: unknown-loads="
                << (m_robust_universal_unknown_loads ? "on" : "off")
                << ", external-globals="
                << (m_robust_universal_external_globals ? "on" : "off")
                << ", inline-asm="
                << (m_robust_universal_inline_asm ? "on" : "off");
    if (!m_robust_bug_filter.empty()) {
      MKINT_LOG() << "  Robust Bug Filter: custom list";
    }
  }

  // Warn if no checkers are enabled
  if (!CheckIntOverflow && !CheckDivByZero && !CheckBadShift &&
      !CheckArrayOOB && !CheckDeadBranch) {
    MKINT_WARN() << "No bug checkers are enabled. No bugs will be detected.";
    MKINT_WARN() << "Select one or more checkers with --checks=<id[,id...]>";
  }

  resetSmtState();
  m_ctx = std::make_unique<z3::context>();
  m_solver = z3::solver(*m_ctx);
  m_dl = &M.getDataLayout();
  m_ptr_bits = m_dl->getPointerSizeInBits(0);
  if (m_ptr_bits == 0)
    m_ptr_bits = 64;
  m_smt_mem = std::make_unique<SmtMemory>(*m_ctx, m_ptr_bits);
  m_module = &M;
  m_func2tsrc.clear();
  m_taint_funcs.clear();
  m_backedges.clear();
  m_backedges_computed.clear();
  m_callback_tsrc_fn.clear();
  m_branch_feasibility.clear();
  m_impossible_branches.clear();
  m_gep_oob.clear();
  m_oob_insts.clear();
  m_overflow_insts.clear();
  m_bad_shift_insts.clear();
  m_div_zero_insts.clear();
  m_value_ids.clear();
  m_next_value_id = 0;
  m_fresh_sym_counter = 0;
  m_in_main_entry = false;
  m_has_global_ctors = (M.getGlobalVariable("llvm.global_ctors") != nullptr);
  m_loop_cut_hit = false;
  m_backedge_counts.clear();
  if (m_bug_detection) {
    m_bug_detection->clearState();
  }
  m_aa = nullptr;
  m_mssa = nullptr;
  m_fam = nullptr;
  m_summary_failed = false;
  m_summary_failure_reason.clear();

  // Mark taint sources.
  for (auto &F : M) {
    auto taint_sources = m_taint_analysis->get_taint_source(F);
    m_taint_analysis->mark_func_sinks(F, m_callback_tsrc_fn);
    if (TaintAnalysis::is_taint_src(F.getName()))
      m_func2tsrc[&F] = std::move(taint_sources);
  }

  // Propagate taint across functions
  m_taint_analysis->propagate_taint_across_functions(M, m_func2tsrc,
                                                     m_taint_funcs);

  // Also add main function to analysis if it exists and is not already in
  // taint_funcs
  if (Function *mainFn = M.getFunction("main")) {
    if (!mainFn->isDeclaration() && !m_taint_funcs.contains(mainFn)) {
      m_taint_funcs.insert(mainFn);
      MKINT_LOG() << "Added main function to analysis";
    }
  }

  MKINT_DEBUG() << "Module after taint has " << M.size() << " functions.";

  auto &FAM = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  m_fam = &FAM;

  this->smt_solving(M);

  m_bug_detection->mark_errors(m_impossible_branches, m_gep_oob,
                               m_overflow_insts, m_bad_shift_insts,
                               m_div_zero_insts);
  if (CheckArrayOOB) {
    for (const auto *inst : m_oob_insts) {
      if (inst && !isa<GetElementPtrInst>(inst)) {
        BugDetection::mark_err<interr::ARRAY_OOB>(
            const_cast<Instruction *>(inst));
      }
    }
  }

  // Report bugs to BugReportMgr (shared pattern)
  reportBugsToManager();

  // Note: SARIF/JSON output is now handled centrally by BugReportMgr
  // in the tool driver, not by individual checkers

  return PreservedAnalyses::all();
}

void MKintPass::buildSummaries(Module &M) {
  if (InterprocSummaryMode == SummaryMode::Off)
    return;
  for (auto &F : M) {
    if (!F.isDeclaration())
      (void)buildSummary(F);
  }
}

void MKintPass::smt_solving(Module &M) {
  SetVector<Function *> funcs_to_analyze;
  if (AnalyzeAllFunctions) {
    for (auto &F : M) {
      if (!F.isDeclaration())
        funcs_to_analyze.insert(&F);
    }
  }
  for (auto *F : m_taint_funcs)
    funcs_to_analyze.insert(F);

  // Summaries are built lazily on demand in applySummary() and cached in
  // m_summary_cache; avoid eager whole-module summary construction over
  // uncalled functions.

  for (auto *F : funcs_to_analyze) {
    if (F->isDeclaration() || F->empty())
      continue;

    ensureBackedges(*F);

    // Reset per-function SMT state (kept inside the solver push/pop).
    m_bbpaths.clear();
    m_v2sym.clear();
    m_smt_mem->reset();
    m_obj_base.clear();
    m_obj_size.clear();
    m_obj_list.clear();
    m_obj_mem.clear();
    m_obj_alias.clear();
    m_int_alias.clear();
    m_ptr_offset.clear();
    m_int_offset.clear();
    m_obj_freed.clear();
    m_obj_size_known.clear();
    m_obj_change_log.clear();
    m_obj_change_frames.clear();
    m_sym_change_log.clear();
    m_sym_change_frames.clear();
    m_path_constraints.clear();
    m_constraint_frames.clear();
    m_universal_vars.clear();
    m_universal_var_ids.clear();
    m_branch_feasibility.clear();
    m_backedge_counts.clear();
    m_in_main_entry = (F->getName() == "main");
    if (m_bug_detection) {
      m_bug_detection->clearCurrentPath();
    }

    // Record start time for this function
    m_function_start_time = std::chrono::steady_clock::now();
    m_paths_explored = 0;
    m_path_limit_hit = false;
    m_timeout_hit = false;
    m_loop_cut_hit = false;
    m_summary_backedge_hit = false;
    MKINT_LOG() << "Beginning analysis of function " << F->getName();

    if (m_fam && (m_aa = m_fam->getCachedResult<llvm::AAManager>(*F))) {
      // cached
    } else if (m_fam) {
      m_aa = &m_fam->getResult<llvm::AAManager>(*F);
    } else {
      m_aa = nullptr;
    }
    if (m_fam) {
      if (auto *MSSARes = m_fam->getCachedResult<llvm::MemorySSAAnalysis>(*F)) {
        m_mssa = &MSSARes->getMSSA();
      } else {
        m_mssa = &m_fam->getResult<llvm::MemorySSAAnalysis>(*F).getMSSA();
      }
    } else {
      m_mssa = nullptr;
    }

    // Globals and stack allocas are materialized lazily on first access
    // (ensureGlobalObject / ensureAllocaObject), avoiding O(G^2) pairwise
    // disjointness constraints over untouched module globals.

    // Build deduplicated CFG successor list. Include backedges when bounded
    // loop unrolling is enabled (enforced per edge in path_solving).
    for (auto &bb : F->getBasicBlockList()) {
      for (const auto *pred : predecessors(&bb)) {
        const bool isBackedge = m_backedges[&bb].contains(pred) || &bb == pred;
        if (isBackedge && m_max_loop_unroll == 0)
          continue;

        auto &succs = m_bbpaths[pred];
        if (std::find(succs.begin(), succs.end(), &bb) == succs.end())
          succs.push_back(&bb);
      }
    }

    m_solver.value().push();
    pushSymFrame();
    pushObjectFrame();
    m_smt_mem->push();
    pushConstraintFrame();

    // add function arg constraints (integers + pointers).
    for (auto &arg : F->args()) {
      const std::string arg_name =
          (F->getName() + "." + std::to_string(arg.getArgNo())).str();
      if (arg.getType()->isIntegerTy()) {
        const auto argv = m_solver.value().ctx().bv_const(
            arg_name.c_str(), arg.getType()->getIntegerBitWidth());
        setSym(&arg, argv);
      } else if (arg.getType()->isPointerTy()) {
        const auto sizev = m_solver.value().ctx().bv_const(
            (arg_name + ".size").c_str(), m_ptr_bits);
        ensureObject(&arg, arg_name + ".obj", sizev, /*sizeKnown=*/false,
                     /*hasRealSize=*/false);
        setSym(&arg, m_obj_base[&arg].value());
      }
    }

    path_solving(&(F->getEntryBlock()), nullptr);

    m_smt_mem->pop();
    popSymFrame();
    popObjectFrame();
    popConstraintFrame();
    m_solver.value().pop();

    // Dead-branch is a universal property ("no feasible path can take this
    // branch"): only report it when exploration of F completed without hitting
    // timeout, path limit, or loop-unroll truncation.
    if (!m_timeout_hit && !m_path_limit_hit && !m_loop_cut_hit) {
      for (const auto &kv : m_branch_feasibility) {
        ICmpInst *cmp = kv.first;
        const BranchFeasibility &bf = kv.second;
        if (bf.unsat_true && !bf.feasible_true) {
          m_impossible_branches[cmp] = true;
        } else if (bf.unsat_false && !bf.feasible_false) {
          m_impossible_branches[cmp] = false;
        }
      }
    }

    // Report analysis time
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - m_function_start_time)
                       .count();
    MKINT_LOG() << "Completed analysis of function " << F->getName() << " in "
                << elapsed << " seconds";
  }
}

void MKintPass::generateSarifReport(const std::string &filename) {
  if (m_bug_detection) {
    m_bug_detection->generateSarifReport(filename, m_impossible_branches,
                                         m_gep_oob, m_overflow_insts,
                                         m_bad_shift_insts, m_div_zero_insts);
  }
}

} // namespace kint
