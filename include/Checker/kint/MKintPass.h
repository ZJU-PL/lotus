#pragma once

#include "Checker/kint/RangeAnalysis.h"
#include "Checker/kint/KINTTaintAnalysis.h"
#include "Checker/kint/BugDetection.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"

// Forward declarations
namespace kint {
    struct crange;
    using bbrange_t = DenseMap<const BasicBlock*, DenseMap<const Value*, crange>>;
}

#include <llvm/IR/PassManager.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/IR/Module.h>
#include <z3++.h>

#include <map>
#include <set>
#include <chrono>

using namespace llvm;

namespace kint {

struct MKintPass : public PassInfoMixin<MKintPass> {
    MKintPass();
    MKintPass(const MKintPass&) = delete;
    MKintPass& operator=(const MKintPass&) = delete;
    MKintPass(MKintPass&&) noexcept = default;
    MKintPass& operator=(MKintPass&&) noexcept = default;
    ~MKintPass() = default;

    PreservedAnalyses run(Module& M, ModuleAnalysisManager& MAM);

    // SARIF reporting
    void generateSarifReport(const std::string& filename);

private:
    // Bug reporting to BugReportMgr 
    void reportBugsToManager();
    void reportBug(interr bug_type, const Instruction* inst, const std::vector<PathPoint>& path = {});
    
    // Bug type IDs (registered with BugReportMgr)
    int m_intOverflowTypeId;
    int m_divByZeroTypeId;
    int m_badShiftTypeId;
    int m_arrayOOBTypeId;
    int m_deadBranchTypeId;
    void backedge_analysis(const Function& F);
    void init_ranges(Module& M);
    void pring_all_ranges() const;
    void smt_solving(Module& M);
    void path_solving(BasicBlock* cur, BasicBlock* pred);
    static std::string get_bb_label(const BasicBlock* bb);

    // Data members
    MapVector<Function*, std::vector<CallInst*>> m_func2tsrc;
    SetVector<Function*> m_taint_funcs;
    DenseMap<const BasicBlock*, SetVector<const BasicBlock*>> m_backedges;
    SetVector<StringRef> m_callback_tsrc_fn;

    // Range analysis components
    std::map<const Function*, bbrange_t> m_func2range_info;
    std::map<const Function*, crange> m_func2ret_range;
    SetVector<Function*> m_range_analysis_funcs;
    std::map<const GlobalVariable*, crange> m_global2range;
    std::map<const GlobalVariable*, SmallVector<crange, 4>> m_garr2ranges;

    // Error checking
    std::map<ICmpInst*, bool> m_impossible_branches;
    std::set<GetElementPtrInst*> m_gep_oob;
    std::set<Instruction*> m_overflow_insts;
    std::set<Instruction*> m_bad_shift_insts;
    std::set<Instruction*> m_div_zero_insts;

    // SMT solving
    llvm::Optional<z3::solver> m_solver;
    DenseMap<const Value*, llvm::Optional<z3::expr>> m_v2sym;
    std::map<const BasicBlock*, SmallVector<BasicBlock*, 2>> m_bbpaths;
    std::chrono::time_point<std::chrono::steady_clock> m_function_start_time;
    unsigned m_function_timeout;

    // Analysis components
    std::unique_ptr<RangeAnalysis> m_range_analysis;
    std::unique_ptr<TaintAnalysis> m_taint_analysis;
    std::unique_ptr<BugDetection> m_bug_detection;
};

} // namespace kint
