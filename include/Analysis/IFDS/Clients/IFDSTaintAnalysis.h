/*
 * Interprocedural Taint Analysis using IFDS
 * 
 * This implements a concrete taint analysis as an example of using the IFDS framework.
 */

#pragma once

#include <Analysis/IFDS/IFDSFramework.h>
#include <Analysis/IFDS/IFDSSolvers.h>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <unordered_set>

namespace ifds {

// ============================================================================
// Taint Fact Definition
// ============================================================================

class TaintFact {
public:
    enum Type { ZERO, TAINTED_VAR, TAINTED_MEMORY };
    
private:
    Type m_type;
    const llvm::Value* m_value;  // For variables
    const llvm::Value* m_memory_location;  // For memory locations
    const llvm::Instruction* m_source_inst;  // Where this taint originated
    
public:
    TaintFact();
    
    static TaintFact zero();
    static TaintFact tainted_var(const llvm::Value* v, const llvm::Instruction* source = nullptr);
    static TaintFact tainted_memory(const llvm::Value* loc, const llvm::Instruction* source = nullptr);
    
    bool operator==(const TaintFact& other) const;
    bool operator<(const TaintFact& other) const;
    bool operator!=(const TaintFact& other) const;
    
    Type get_type() const;
    const llvm::Value* get_value() const;
    const llvm::Value* get_memory_location() const;
    const llvm::Instruction* get_source() const;
    
    bool is_zero() const;
    bool is_tainted_var() const;
    bool is_tainted_memory() const;
    
    // Create a new fact with the same taint but different source
    TaintFact with_source(const llvm::Instruction* source) const;
    
    friend std::ostream& operator<<(std::ostream& os, const TaintFact& fact);
};

// Tracing strategies enumeration (outside class for template usage)
enum class TracingStrategy {
    BOUNDARY_ONLY,      // Function boundary-only tracing
    SUMMARY_BASED       // Summary edge-based reconstruction (fastest)
};

} // namespace ifds

// Hash function for TaintFact
namespace std {
template<>
struct hash<ifds::TaintFact> {
    size_t operator()(const ifds::TaintFact& fact) const;
};
}

namespace ifds {

// ============================================================================
// Interprocedural Taint Analysis using IFDS
// ============================================================================

class TaintAnalysis : public IFDSProblem<TaintFact> {
private:
    std::unordered_set<std::string> m_source_functions;
    std::unordered_set<std::string> m_sink_functions;
    
public:
    TaintAnalysis();
    
    // IFDS interface implementation
    TaintFact zero_fact() const override;
    FactSet normal_flow(const llvm::Instruction* stmt, const TaintFact& fact) override;
    FactSet call_flow(const llvm::CallInst* call, const llvm::Function* callee, 
                     const TaintFact& fact) override;
    FactSet return_flow(const llvm::CallInst* call, const llvm::Function* callee,
                       const TaintFact& exit_fact, const TaintFact& call_fact) override;
    FactSet call_to_return_flow(const llvm::CallInst* call, const TaintFact& fact) override;
    FactSet initial_facts(const llvm::Function* main) override;
    
    // Override source/sink detection
    bool is_source(const llvm::Instruction* inst) const override;
    bool is_sink(const llvm::Instruction* inst) const override;
    
    // Add custom source/sink functions
    void add_source_function(const std::string& func_name);
    void add_sink_function(const std::string& func_name);
    
    // Vulnerability detection and reporting
    void report_vulnerabilities(const IFDSSolver<TaintAnalysis>& solver,
                               llvm::raw_ostream& OS,
                               size_t max_vulnerabilities = 10) const;

    void report_vulnerabilities_parallel(const ParallelIFDSSolver<TaintAnalysis>& solver,
                                        llvm::raw_ostream& OS,
                                        size_t max_vulnerabilities = 10) const;

    // Enhanced reporting methods with configurable tracing strategies
    void try_report_vulnerabilities_with_strategy(const IFDSSolver<TaintAnalysis>& solver,
                                                llvm::raw_ostream& OS,
                                                ifds::TracingStrategy strategy,
                                                size_t max_vulnerabilities = 10) const;
    
    // Exposed for reporting utilities
    struct TaintPath {
        std::vector<const llvm::Instruction*> sources;
        std::vector<const llvm::Function*> intermediate_functions;
    };

    // Tracing methods for reconstructing taint propagation paths
    TaintPath trace_taint_sources_boundary_only(
        const IFDSSolver<TaintAnalysis>& solver,
        const llvm::CallInst* sink_call,
        const TaintFact& tainted_fact) const;

    TaintPath trace_taint_sources_summary_based(
        const IFDSSolver<TaintAnalysis>& solver,
        const llvm::CallInst* sink_call,
        const TaintFact& tainted_fact) const;

    TaintPath trace_taint_sources_parallel(
        const ParallelIFDSSolver<TaintAnalysis>& solver,
        const llvm::CallInst* sink_call,
        const TaintFact& tainted_fact) const;

    bool is_argument_tainted(const llvm::Value* arg, const TaintFact& fact) const;
    std::string format_tainted_arg(unsigned arg_index, const TaintFact& fact, const llvm::CallInst* call) const;
    void analyze_tainted_arguments(const llvm::CallInst* call, const FactSet& facts,
                                  std::string& tainted_args,
                                  std::vector<const llvm::Instruction*> all_sources,
                                  std::vector<const llvm::Function*> propagation_path) const;
    void output_vulnerability_report(llvm::raw_ostream& OS, size_t vuln_num,
                                   const std::string& func_name, const llvm::CallInst* call,
                                   const std::string& tainted_args,
                                   const std::vector<const llvm::Instruction*>& all_sources,
                                   const std::vector<const llvm::Function*>& propagation_path,
                                   size_t max_vulnerabilities) const;

    // Helper for boundary-only tracing
    bool comes_before(const llvm::Instruction* first, const llvm::Instruction* second) const;

private:
    bool kills_fact(const llvm::CallInst* call, const TaintFact& fact) const;

    // Internal helpers that need access to alias analysis utilities
    void propagate_tainted_memory_aliases(const llvm::Value* ptr, FactSet& result) const;
    void handle_source_function_specs(const llvm::CallInst* call, FactSet& result) const;
    void handle_pipe_specifications(const llvm::CallInst* call, const TaintFact& fact, FactSet& result) const;

};

} // namespace ifds