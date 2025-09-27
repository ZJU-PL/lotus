/*
 * Taint Analysis Implementation
 */

#include <Analysis/IFDS/TaintAnalysis.h>
#include <Checker/TaintConfigManager.h>

#include <llvm/Support/raw_ostream.h>

namespace sparta {
namespace ifds {

// ============================================================================
// TaintFact Implementation
// ============================================================================

TaintFact::TaintFact() : m_type(ZERO), m_value(nullptr), m_memory_location(nullptr) {}

TaintFact TaintFact::zero() { 
    return TaintFact(); 
}

TaintFact TaintFact::tainted_var(const llvm::Value* v) {
    TaintFact fact;
    fact.m_type = TAINTED_VAR;
    fact.m_value = v;
    return fact;
}

TaintFact TaintFact::tainted_memory(const llvm::Value* loc) {
    TaintFact fact;
    fact.m_type = TAINTED_MEMORY;
    fact.m_memory_location = loc;
    return fact;
}

bool TaintFact::operator==(const TaintFact& other) const {
    if (m_type != other.m_type) return false;
    switch (m_type) {
        case ZERO: return true;
        case TAINTED_VAR: return m_value == other.m_value;
        case TAINTED_MEMORY: return m_memory_location == other.m_memory_location;
    }
    return false;
}

bool TaintFact::operator<(const TaintFact& other) const {
    if (m_type != other.m_type) return m_type < other.m_type;
    switch (m_type) {
        case ZERO: return false;
        case TAINTED_VAR: return m_value < other.m_value;
        case TAINTED_MEMORY: return m_memory_location < other.m_memory_location;
    }
    return false;
}

TaintFact::Type TaintFact::get_type() const { 
    return m_type; 
}

const llvm::Value* TaintFact::get_value() const { 
    return m_value; 
}

const llvm::Value* TaintFact::get_memory_location() const { 
    return m_memory_location; 
}

bool TaintFact::is_zero() const { 
    return m_type == ZERO; 
}

bool TaintFact::is_tainted_var() const { 
    return m_type == TAINTED_VAR; 
}

bool TaintFact::is_tainted_memory() const { 
    return m_type == TAINTED_MEMORY; 
}

std::ostream& operator<<(std::ostream& os, const TaintFact& fact) {
    switch (fact.m_type) {
        case TaintFact::ZERO: os << "⊥"; break;
        case TaintFact::TAINTED_VAR: 
            os << "Tainted(" << fact.m_value->getName().str() << ")"; 
            break;
        case TaintFact::TAINTED_MEMORY: 
            os << "TaintedMem(" << fact.m_memory_location->getName().str() << ")"; 
            break;
    }
    return os;
}

// ============================================================================
// TaintAnalysis Implementation
// ============================================================================

TaintAnalysis::TaintAnalysis() {
    // Load the unified taint configuration
    if (!checker::taint_config::load_default_config()) {
        llvm::errs() << "Error: Could not load taint configuration\n";
        return;
    }
    
    // Load sources and sinks from configuration
    auto sources = checker::TaintConfigManager::getInstance().get_all_source_functions();
    auto sinks = checker::TaintConfigManager::getInstance().get_all_sink_functions();
    
    for (const auto& source : sources) {
        m_source_functions.insert(source);
    }
    
    for (const auto& sink : sinks) {
        m_sink_functions.insert(sink);
    }
    
    llvm::outs() << "Loaded " << sources.size() << " sources and " << sinks.size() << " sinks from configuration\n";
}

TaintFact TaintAnalysis::zero_fact() const {
    return TaintFact::zero();
}

TaintAnalysis::FactSet TaintAnalysis::normal_flow(const llvm::Instruction* stmt, const TaintFact& fact) {
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

TaintAnalysis::FactSet TaintAnalysis::call_flow(const llvm::CallInst* call, const llvm::Function* callee, 
                     const TaintFact& fact) {
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

TaintAnalysis::FactSet TaintAnalysis::return_flow(const llvm::CallInst* call, const llvm::Function* callee,
                       const TaintFact& exit_fact, const TaintFact& call_fact) {
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

TaintAnalysis::FactSet TaintAnalysis::call_to_return_flow(const llvm::CallInst* call, const TaintFact& fact) {
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

TaintAnalysis::FactSet TaintAnalysis::initial_facts(const llvm::Function* main) {
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

bool TaintAnalysis::is_source(const llvm::Instruction* inst) const {
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        if (const llvm::Function* callee = call->getCalledFunction()) {
            return m_source_functions.count(callee->getName().str()) > 0;
        }
    }
    return false;
}

bool TaintAnalysis::is_sink(const llvm::Instruction* inst) const {
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        if (const llvm::Function* callee = call->getCalledFunction()) {
            return m_sink_functions.count(callee->getName().str()) > 0;
        }
    }
    return false;
}

void TaintAnalysis::add_source_function(const std::string& func_name) {
    m_source_functions.insert(func_name);
}

void TaintAnalysis::add_sink_function(const std::string& func_name) {
    m_sink_functions.insert(func_name);
}

bool TaintAnalysis::kills_fact(const llvm::CallInst* call, const TaintFact& fact) const {
    // Determine if a call kills a particular fact
    // For taint analysis, most calls don't kill existing facts
    
    const llvm::Function* callee = call->getCalledFunction();
    if (!callee) return false;
    
    std::string func_name = callee->getName().str();
    
    // Some functions might "sanitize" taint
    // Note: Sanitizer functions are not yet supported in the unified config
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

} // namespace ifds
} // namespace sparta

// Hash function implementation
namespace std {
size_t hash<sparta::ifds::TaintFact>::operator()(const sparta::ifds::TaintFact& fact) const {
    size_t h1 = std::hash<int>{}(static_cast<int>(fact.get_type()));
    size_t h2 = 0;
    if (fact.get_value()) {
        h2 = std::hash<const llvm::Value*>{}(fact.get_value());
    } else if (fact.get_memory_location()) {
        h2 = std::hash<const llvm::Value*>{}(fact.get_memory_location());
    }
    return h1 ^ (h2 << 1);
}
}
