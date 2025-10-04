/*
 * Interprocedural Taint Analysis using IFDS
 * 
 * This implements a concrete taint analysis as an example of using the IFDS framework.
 */

#pragma once

#include <Analysis/IFDS/IFDSFramework.h>

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
    
public:
    TaintFact();
    
    static TaintFact zero();
    static TaintFact tainted_var(const llvm::Value* v);
    static TaintFact tainted_memory(const llvm::Value* loc);
    
    bool operator==(const TaintFact& other) const;
    bool operator<(const TaintFact& other) const;
    bool operator!=(const TaintFact& other) const;
    
    Type get_type() const;
    const llvm::Value* get_value() const;
    const llvm::Value* get_memory_location() const;
    
    bool is_zero() const;
    bool is_tainted_var() const;
    bool is_tainted_memory() const;
    
    friend std::ostream& operator<<(std::ostream& os, const TaintFact& fact);
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
    
private:
    bool kills_fact(const llvm::CallInst* call, const TaintFact& fact) const;
};

} // namespace ifds