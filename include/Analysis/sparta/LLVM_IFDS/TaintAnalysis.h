/*
 * Interprocedural Taint Analysis using IFDS
 * 
 * This implements a concrete taint analysis as an example of using the IFDS framework.
 */

#pragma once

#include <Analysis/sparta/LLVM_IFDS/IFDSFramework.h>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

#include <string>
#include <unordered_set>

namespace sparta {
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
    TaintFact() : m_type(ZERO), m_value(nullptr), m_memory_location(nullptr) {}
    
    static TaintFact zero() { return TaintFact(); }
    
    static TaintFact tainted_var(const llvm::Value* v) {
        TaintFact fact;
        fact.m_type = TAINTED_VAR;
        fact.m_value = v;
        return fact;
    }
    
    static TaintFact tainted_memory(const llvm::Value* loc) {
        TaintFact fact;
        fact.m_type = TAINTED_MEMORY;
        fact.m_memory_location = loc;
        return fact;
    }
    
    bool operator==(const TaintFact& other) const {
        if (m_type != other.m_type) return false;
        switch (m_type) {
            case ZERO: return true;
            case TAINTED_VAR: return m_value == other.m_value;
            case TAINTED_MEMORY: return m_memory_location == other.m_memory_location;
        }
        return false;
    }
    
    bool operator<(const TaintFact& other) const {
        if (m_type != other.m_type) return m_type < other.m_type;
        switch (m_type) {
            case ZERO: return false;
            case TAINTED_VAR: return m_value < other.m_value;
            case TAINTED_MEMORY: return m_memory_location < other.m_memory_location;
        }
        return false;
    }
    
    Type get_type() const { return m_type; }
    const llvm::Value* get_value() const { return m_value; }
    const llvm::Value* get_memory_location() const { return m_memory_location; }
    
    bool is_zero() const { return m_type == ZERO; }
    bool is_tainted_var() const { return m_type == TAINTED_VAR; }
    bool is_tainted_memory() const { return m_type == TAINTED_MEMORY; }
    
    friend std::ostream& operator<<(std::ostream& os, const TaintFact& fact) {
        switch (fact.m_type) {
            case ZERO: os << "⊥"; break;
            case TAINTED_VAR: 
                os << "Tainted(" << fact.m_value->getName().str() << ")"; 
                break;
            case TAINTED_MEMORY: 
                os << "TaintedMem(" << fact.m_memory_location->getName().str() << ")"; 
                break;
        }
        return os;
    }
};

} // namespace ifds
} // namespace sparta

// Hash function for TaintFact
namespace std {
template<>
struct hash<sparta::ifds::TaintFact> {
    size_t operator()(const sparta::ifds::TaintFact& fact) const {
        size_t h1 = std::hash<int>{}(static_cast<int>(fact.get_type()));
        size_t h2 = 0;
        if (fact.get_value()) {
            h2 = std::hash<const llvm::Value*>{}(fact.get_value());
        } else if (fact.get_memory_location()) {
            h2 = std::hash<const llvm::Value*>{}(fact.get_memory_location());
        }
        return h1 ^ (h2 << 1);
    }
};
}

namespace sparta {
namespace ifds {

// ============================================================================
// Interprocedural Taint Analysis using IFDS
// ============================================================================

class TaintAnalysis : public IFDSProblem<TaintFact> {
private:
    std::unordered_set<std::string> m_source_functions;
    std::unordered_set<std::string> m_sink_functions;
    
public:
    TaintAnalysis() {
        // Common taint sources
        m_source_functions.insert("gets");
        m_source_functions.insert("fgets");
        m_source_functions.insert("getchar");
        m_source_functions.insert("scanf");
        m_source_functions.insert("fscanf");
        m_source_functions.insert("read");
        m_source_functions.insert("recv");
        m_source_functions.insert("recvfrom");
        
        // Common taint sinks
        m_sink_functions.insert("system");
        m_sink_functions.insert("exec");
        m_sink_functions.insert("execl");
        m_sink_functions.insert("execv");
        m_sink_functions.insert("popen");
        m_sink_functions.insert("printf");
        m_sink_functions.insert("fprintf");
        m_sink_functions.insert("sprintf");
        m_sink_functions.insert("strcpy");
        m_sink_functions.insert("strcat");
    }
    
    // IFDS interface implementation
    TaintFact zero_fact() const override {
        return TaintFact::zero();
    }
    
    FactSet normal_flow(const llvm::Instruction* stmt, const TaintFact& fact) override {
        FactSet result;
        
        // Always propagate zero fact
        if (fact.is_zero()) {
            result.insert(fact);
        }
        
        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
            // Store: if storing tainted value, taint the memory location and all aliases
            const llvm::Value* value = store->getValueOperand();
            const llvm::Value* ptr = store->getPointerOperand();
            
            if (fact.is_tainted_var() && fact.get_value() == value) {
                // Taint the direct pointer
                result.insert(TaintFact::tainted_memory(ptr));
                
                // Also taint all memory locations that may alias with ptr
                if (m_alias_analysis) {
                    auto alias_set = get_alias_set(ptr);
                    for (const llvm::Value* alias : alias_set) {
                        if (alias != ptr && alias->getType()->isPointerTy()) {
                            result.insert(TaintFact::tainted_memory(alias));
                        }
                    }
                }
            }
            
            // Check if we're storing to a memory location that's already tainted
            if (fact.is_tainted_memory() && may_alias(fact.get_memory_location(), ptr)) {
                // The stored value becomes tainted
                result.insert(TaintFact::tainted_var(value));
            }
            
            // Propagate existing facts
            if (!fact.is_zero()) {
                result.insert(fact);
            }
            
        } else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
            // Load: if loading from tainted memory, taint the result
            const llvm::Value* ptr = load->getPointerOperand();
            
            // Check if loading from any tainted memory location
            if (fact.is_tainted_memory() && may_alias(fact.get_memory_location(), ptr)) {
                result.insert(TaintFact::tainted_var(load));
            }
            
            // Also check if the pointer itself is tainted (loading through tainted pointer)
            if (fact.is_tainted_var() && fact.get_value() == ptr) {
                result.insert(TaintFact::tainted_var(load));
            }
            
            // Propagate existing facts
            if (!fact.is_zero()) {
                result.insert(fact);
            }
            
        } else if (auto* binop = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
            // Binary operation: if any operand is tainted, result is tainted
            const llvm::Value* lhs = binop->getOperand(0);
            const llvm::Value* rhs = binop->getOperand(1);
            
            if (fact.is_tainted_var() && 
                (fact.get_value() == lhs || fact.get_value() == rhs)) {
                result.insert(TaintFact::tainted_var(binop));
            }
            
            // Propagate existing facts
            if (!fact.is_zero()) {
                result.insert(fact);
            }
            
        } else if (auto* cast = llvm::dyn_cast<llvm::CastInst>(stmt)) {
            // Cast: propagate taint through casts
            const llvm::Value* operand = cast->getOperand(0);
            
            if (fact.is_tainted_var() && fact.get_value() == operand) {
                result.insert(TaintFact::tainted_var(cast));
            }
            
            // Propagate existing facts
            if (!fact.is_zero()) {
                result.insert(fact);
            }
            
        } else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
            // GEP: propagate pointer taint
            const llvm::Value* ptr = gep->getPointerOperand();
            
            if (fact.is_tainted_var() && fact.get_value() == ptr) {
                result.insert(TaintFact::tainted_var(gep));
            }
            
            // Propagate existing facts
            if (!fact.is_zero()) {
                result.insert(fact);
            }
            
        } else {
            // Default: just propagate existing facts
            result.insert(fact);
        }
        
        return result;
    }
    
    FactSet call_flow(const llvm::CallInst* call, const llvm::Function* callee, 
                     const TaintFact& fact) override {
        FactSet result;
        
        // Always propagate zero fact
        if (fact.is_zero()) {
            result.insert(fact);
        }
        
        if (!callee || callee->isDeclaration()) {
            return result;
        }
        
        // Map caller facts to callee facts
        if (fact.is_tainted_var()) {
            // Check if the tainted value is passed as an argument
            for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
                const llvm::Value* arg = call->getOperand(i);
                
                // Direct match
                if (arg == fact.get_value()) {
                    auto param_it = callee->arg_begin();
                    std::advance(param_it, i);
                    result.insert(TaintFact::tainted_var(&*param_it));
                }
                
                // Also check for alias relationships
                if (may_alias(arg, fact.get_value())) {
                    auto param_it = callee->arg_begin();
                    std::advance(param_it, i);
                    result.insert(TaintFact::tainted_var(&*param_it));
                }
            }
        }
        
        // Handle memory taint propagation through pointer arguments
        if (fact.is_tainted_memory()) {
            for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
                const llvm::Value* arg = call->getOperand(i);
                
                if (arg->getType()->isPointerTy() && 
                    may_alias(arg, fact.get_memory_location())) {
                    // The memory location is accessible in the callee through this parameter
                    auto param_it = callee->arg_begin();
                    std::advance(param_it, i);
                    result.insert(TaintFact::tainted_memory(&*param_it));
                }
            }
        }
        
        return result;
    }
    
    FactSet return_flow(const llvm::CallInst* call, const llvm::Function* callee,
                       const TaintFact& exit_fact, const TaintFact& call_fact) override {
        FactSet result;
        
        // Always propagate zero fact
        if (exit_fact.is_zero()) {
            result.insert(exit_fact);
        }
        
        // Map return values back to call site
        if (exit_fact.is_tainted_var()) {
            // Check if the exit fact is about the return value
            for (const llvm::BasicBlock& bb : *callee) {
                for (const llvm::Instruction& inst : bb) {
                    if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                        if (ret->getReturnValue() == exit_fact.get_value()) {
                            result.insert(TaintFact::tainted_var(call));
                        }
                    }
                }
            }
        }
        
        // Also propagate call site facts that weren't killed
        if (!call_fact.is_zero()) {
            result.insert(call_fact);
        }
        
        return result;
    }
    
    FactSet call_to_return_flow(const llvm::CallInst* call, const TaintFact& fact) override {
        FactSet result;
        
        // Always propagate zero fact
        if (fact.is_zero()) {
            result.insert(fact);
        }
        
        const llvm::Function* callee = call->getCalledFunction();
        if (!callee) {
            // Indirect call - be conservative
            if (!fact.is_zero()) {
                result.insert(fact);
            }
            return result;
        }
        
        std::string func_name = callee->getName().str();
        
        // Handle source functions
        if (m_source_functions.count(func_name)) {
            result.insert(TaintFact::tainted_var(call));
        }
        
        // Handle sink functions - check for tainted arguments
        if (m_sink_functions.count(func_name) && fact.is_tainted_var()) {
            for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
                if (call->getOperand(i) == fact.get_value()) {
                    // Found tainted data flowing to sink - this is a potential vulnerability
                    // We'll report this in the main analysis loop instead of here
                    // to avoid duplicate reporting
                }
            }
        }
        
        // Propagate facts that are not killed by the call
        if (!fact.is_zero() && !kills_fact(call, fact)) {
            result.insert(fact);
        }
        
        return result;
    }
    
    FactSet initial_facts(const llvm::Function* main) override {
        FactSet result;
        result.insert(zero_fact());
        
        // Add initial taint facts if needed
        // For example, command line arguments could be considered tainted
        for (const llvm::Argument& arg : main->args()) {
            if (arg.getType()->isPointerTy()) {
                result.insert(TaintFact::tainted_var(&arg));
            }
        }
        
        return result;
    }
    
    // Override source/sink detection
    bool is_source(const llvm::Instruction* inst) const override {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
            if (const llvm::Function* callee = call->getCalledFunction()) {
                return m_source_functions.count(callee->getName().str()) > 0;
            }
        }
        return false;
    }
    
    bool is_sink(const llvm::Instruction* inst) const override {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
            if (const llvm::Function* callee = call->getCalledFunction()) {
                return m_sink_functions.count(callee->getName().str()) > 0;
            }
        }
        return false;
    }
    
    // Add custom source/sink functions
    void add_source_function(const std::string& func_name) {
        m_source_functions.insert(func_name);
    }
    
    void add_sink_function(const std::string& func_name) {
        m_sink_functions.insert(func_name);
    }
    
private:
    bool kills_fact(const llvm::CallInst* call, const TaintFact& fact) const {
        // Determine if a call kills a particular fact
        // For taint analysis, most calls don't kill existing facts
        
        const llvm::Function* callee = call->getCalledFunction();
        if (!callee) return false;
        
        std::string func_name = callee->getName().str();
        
        // Some functions might "sanitize" taint
        static const std::unordered_set<std::string> sanitizers = {
            "strlen", "strcmp", "strncmp", "isdigit", "isalpha"
        };
        
        if (sanitizers.count(func_name) && fact.is_tainted_var()) {
            // Check if the tainted value is passed to a sanitizer
            for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
                if (call->getOperand(i) == fact.get_value()) {
                    return true; // This fact is sanitized
                }
            }
        }
        
        return false;
    }
};

} // namespace ifds
} // namespace sparta
