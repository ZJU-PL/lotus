/*
 * Reaching Definitions Analysis Implementation
 */

#include <Analysis/IFDS/Clients/IFDSReachingDefinitions.h>

#include <iostream>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// DefinitionFact Implementation
// ============================================================================

DefinitionFact::DefinitionFact() : m_type(ZERO), m_variable(nullptr), m_definition_site(nullptr) {}

DefinitionFact DefinitionFact::zero() { 
    return DefinitionFact(); 
}

DefinitionFact DefinitionFact::definition(const llvm::Value* var, const llvm::Instruction* def_site) {
    DefinitionFact fact;
    fact.m_type = DEFINITION;
    fact.m_variable = var;
    fact.m_definition_site = def_site;
    return fact;
}

bool DefinitionFact::operator==(const DefinitionFact& other) const {
    if (m_type != other.m_type) return false;
    if (m_type == ZERO) return true;
    return m_variable == other.m_variable && m_definition_site == other.m_definition_site;
}

bool DefinitionFact::operator<(const DefinitionFact& other) const {
    if (m_type != other.m_type) return m_type < other.m_type;
    if (m_type == ZERO) return false;
    if (m_variable != other.m_variable) return m_variable < other.m_variable;
    return m_definition_site < other.m_definition_site;
}

bool DefinitionFact::operator!=(const DefinitionFact& other) const {
    return !(*this == other);
}

DefinitionFact::Type DefinitionFact::get_type() const { 
    return m_type; 
}

const llvm::Value* DefinitionFact::get_variable() const { 
    return m_variable; 
}

const llvm::Instruction* DefinitionFact::get_definition_site() const { 
    return m_definition_site; 
}

bool DefinitionFact::is_zero() const { 
    return m_type == ZERO; 
}

bool DefinitionFact::is_definition() const { 
    return m_type == DEFINITION; 
}

std::ostream& operator<<(std::ostream& os, const DefinitionFact& fact) {
    if (fact.is_zero()) {
        os << "⊥";
    } else {
        os << "Def(" << fact.m_variable->getName().str() << " @ ";
        if (fact.m_definition_site->getParent()) {
            os << fact.m_definition_site->getParent()->getName().str() << ")";
        } else {
            os << "?)";
        }
    }
    return os;
}

// ============================================================================
// ReachingDefinitionsAnalysis Implementation
// ============================================================================

DefinitionFact ReachingDefinitionsAnalysis::zero_fact() const {
    return DefinitionFact::zero();
}

ReachingDefinitionsAnalysis::FactSet ReachingDefinitionsAnalysis::normal_flow(const llvm::Instruction* stmt, const DefinitionFact& fact) {
    FactSet result;
    
    // Always propagate zero fact
    if (fact.is_zero()) {
        result.insert(fact);
    }
    
    // Check if this instruction defines a variable
    if (defines_variable(stmt)) {
        const llvm::Value* defined_var = get_defined_variable(stmt);
        
        // Kill existing definitions of the same variable
        if (fact.is_definition() && fact.get_variable() == defined_var) {
            // This fact is killed by the new definition
            // Don't propagate it
        } else {
            // Propagate existing facts that are not killed
            if (!fact.is_zero()) {
                result.insert(fact);
            }
        }
        
        // Generate new definition fact
        if (fact.is_zero()) {
            result.insert(DefinitionFact::definition(defined_var, stmt));
        }
        
    } else {
        // No definition - just propagate existing facts
        if (!fact.is_zero()) {
            result.insert(fact);
        }
    }
    
    return result;
}

ReachingDefinitionsAnalysis::FactSet ReachingDefinitionsAnalysis::call_flow(const llvm::CallInst* call, const llvm::Function* callee,
                     const DefinitionFact& fact) {
    FactSet result;
    
    // Always propagate zero fact
    if (fact.is_zero()) {
        result.insert(fact);
    }
    
    if (!callee || callee->isDeclaration()) {
        return result;
    }
    
    // For reaching definitions, we need to map definitions of arguments
    if (fact.is_definition()) {
        // Check if the defined variable is passed as an argument
        for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
            if (call->getOperand(i) == fact.get_variable()) {
                // Map to corresponding parameter in callee
                auto arg_it = callee->arg_begin();
                std::advance(arg_it, i);
                
                // Create a new definition fact for the parameter
                // The definition site is the function entry
                const llvm::Instruction* entry_inst = &callee->getEntryBlock().front();
                result.insert(DefinitionFact::definition(&*arg_it, entry_inst));
            }
        }
    }
    
    return result;
}

ReachingDefinitionsAnalysis::FactSet ReachingDefinitionsAnalysis::return_flow(const llvm::CallInst* call, const llvm::Function* callee,
                       const DefinitionFact& exit_fact, const DefinitionFact& call_fact) {
    FactSet result;
    
    // Always propagate zero fact
    if (exit_fact.is_zero()) {
        result.insert(exit_fact);
    }
    
    // For reaching definitions, we need to handle return values
    if (exit_fact.is_definition()) {
        // Check if the definition is of a return value
        for (const llvm::BasicBlock& bb : *callee) {
            for (const llvm::Instruction& inst : bb) {
                if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                    if (ret->getReturnValue() == exit_fact.get_variable()) {
                        // Map return value definition to call result
                        result.insert(DefinitionFact::definition(call, exit_fact.get_definition_site()));
                    }
                }
            }
        }
    }
    
    // Propagate call site facts that represent local variables
    if (call_fact.is_definition() && is_local_to_caller(call_fact, callee)) {
        result.insert(call_fact);
    }
    
    return result;
}

ReachingDefinitionsAnalysis::FactSet ReachingDefinitionsAnalysis::call_to_return_flow(const llvm::CallInst* call, const DefinitionFact& fact) {
    FactSet result;
    
    // Always propagate zero fact
    if (fact.is_zero()) {
        result.insert(fact);
    }
    
    const llvm::Function* callee = call->getCalledFunction();
    
    // For external functions, model their effects
    if (!callee || callee->isDeclaration()) {
        // External function call
        std::string func_name = callee ? callee->getName().str() : "";
        
        // Handle functions that might modify global state or return values
        if (func_name == "malloc" || func_name == "calloc") {
            // Memory allocation - creates a new definition
            if (fact.is_zero()) {
                result.insert(DefinitionFact::definition(call, call));
            }
        }
        
        // Propagate facts that are not killed by external calls
        if (fact.is_definition() && !is_killed_by_external_call(fact, call)) {
            result.insert(fact);
        }
    } else {
        // Internal function - propagate local facts
        if (fact.is_definition() && is_local_to_caller(fact, callee)) {
            result.insert(fact);
        }
    }
    
    return result;
}

ReachingDefinitionsAnalysis::FactSet ReachingDefinitionsAnalysis::initial_facts(const llvm::Function* main) {
    FactSet result;
    result.insert(zero_fact());
    
    // Add initial definitions for function parameters
    for (const llvm::Argument& arg : main->args()) {
        const llvm::Instruction* entry_inst = &main->getEntryBlock().front();
        result.insert(DefinitionFact::definition(&arg, entry_inst));
    }
    
    return result;
}

std::vector<const llvm::Instruction*> ReachingDefinitionsAnalysis::get_reaching_definitions(
    const llvm::Instruction* use_site, const llvm::Value* variable) const {
    
    std::vector<const llvm::Instruction*> definitions;
    
    // This would query the analysis results
    // Implementation depends on how results are stored
    
    return definitions;
}

bool ReachingDefinitionsAnalysis::defines_variable(const llvm::Instruction* inst) const {
    // Check if instruction defines a variable
    return !inst->getType()->isVoidTy() || 
           llvm::isa<llvm::StoreInst>(inst) ||
           llvm::isa<llvm::AllocaInst>(inst);
}

const llvm::Value* ReachingDefinitionsAnalysis::get_defined_variable(const llvm::Instruction* inst) const {
    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
        return store->getPointerOperand();
    } else if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(inst)) {
        return alloca;
    } else if (!inst->getType()->isVoidTy()) {
        return inst;
    }
    return nullptr;
}

bool ReachingDefinitionsAnalysis::is_local_to_caller(const DefinitionFact& fact, const llvm::Function* callee) const {
    if (!fact.is_definition()) return false;
    
    const llvm::Value* var = fact.get_variable();
    
    // Check if the variable is local to the caller (not a parameter or global)
    if (llvm::isa<llvm::GlobalValue>(var)) {
        return false; // Global variable
    }
    
    if (llvm::isa<llvm::Argument>(var)) {
        // Check if it's a parameter of the callee
        for (const llvm::Argument& arg : callee->args()) {
            if (&arg == var) {
                return false; // Parameter of callee
            }
        }
    }
    
    return true; // Local to caller
}

bool ReachingDefinitionsAnalysis::is_killed_by_external_call(const DefinitionFact& fact, const llvm::CallInst* call) const {
    if (!fact.is_definition()) return false;
    
    const llvm::Value* var = fact.get_variable();
    
    // Conservative: external calls might modify global variables and 
    // memory locations passed as pointer arguments
    if (llvm::isa<llvm::GlobalValue>(var)) {
        return true; // Assume external calls can modify globals
    }
    
    // Check if the variable is passed as a pointer argument
    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
        const llvm::Value* arg = call->getOperand(i);
        if (arg->getType()->isPointerTy() && may_alias(arg, var)) {
            return true; // Might be modified through pointer
        }
    }
    
    return false;
}

} // namespace ifds

// Hash function implementation
namespace std {
size_t hash<ifds::DefinitionFact>::operator()(const ifds::DefinitionFact& fact) const {
    if (fact.is_zero()) return 0;
    
    size_t h1 = std::hash<const llvm::Value*>{}(fact.get_variable());
    size_t h2 = std::hash<const llvm::Instruction*>{}(fact.get_definition_site());
    return h1 ^ (h2 << 1);
}
}
