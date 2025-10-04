/*
 * Taint Analysis Implementation
 */

#include <Analysis/IFDS/IFDSTaintAnalysis.h>
#include <Annotation/Taint/TaintConfigManager.h>

#include <iostream>
#include <llvm/Support/raw_ostream.h>

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

bool TaintFact::operator!=(const TaintFact& other) const {
    return !(*this == other);
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
    if (!taint_config::load_default_config()) {
        llvm::errs() << "Error: Could not load taint configuration\n";
        return;
    }
    
    auto& config = TaintConfigManager::getInstance();
    auto sources = config.get_all_source_functions();
    auto sinks = config.get_all_sink_functions();
    
    m_source_functions.insert(sources.begin(), sources.end());
    m_sink_functions.insert(sinks.begin(), sinks.end());
    
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
        return result;
    }
    
    // Helper to propagate existing facts
    auto propagate_fact = [&result, &fact]() { result.insert(fact); };
    
    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
        const llvm::Value* value = store->getValueOperand();
        const llvm::Value* ptr = store->getPointerOperand();
        
        if (fact.is_tainted_var() && fact.get_value() == value) {
            result.insert(TaintFact::tainted_memory(ptr));
            if (m_alias_analysis) {
                auto alias_set = get_alias_set(ptr);
                for (const llvm::Value* alias : alias_set) {
                    if (alias != ptr && alias->getType()->isPointerTy()) {
                        result.insert(TaintFact::tainted_memory(alias));
                    }
                }
            }
        }
        
        if (fact.is_tainted_memory() && may_alias(fact.get_memory_location(), ptr)) {
            result.insert(TaintFact::tainted_var(value));
        }
        
        propagate_fact();
        
    } else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
        const llvm::Value* ptr = load->getPointerOperand();
        
        if ((fact.is_tainted_memory() && may_alias(fact.get_memory_location(), ptr)) ||
            (fact.is_tainted_var() && fact.get_value() == ptr)) {
            result.insert(TaintFact::tainted_var(load));
        }
        
        propagate_fact();
        
    } else if (auto* binop = llvm::dyn_cast<llvm::BinaryOperator>(stmt)) {
        const llvm::Value* lhs = binop->getOperand(0);
        const llvm::Value* rhs = binop->getOperand(1);
        
        if (fact.is_tainted_var() && (fact.get_value() == lhs || fact.get_value() == rhs)) {
            result.insert(TaintFact::tainted_var(binop));
        }
        
        propagate_fact();
        
    } else if (auto* cast = llvm::dyn_cast<llvm::CastInst>(stmt)) {
        if (fact.is_tainted_var() && fact.get_value() == cast->getOperand(0)) {
            result.insert(TaintFact::tainted_var(cast));
        }
        
        propagate_fact();
        
    } else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
        if (fact.is_tainted_var() && fact.get_value() == gep->getPointerOperand()) {
            result.insert(TaintFact::tainted_var(gep));
        }
        
        propagate_fact();
        
    } else {
        propagate_fact();
    }
    
    return result;
}

TaintAnalysis::FactSet TaintAnalysis::call_flow(const llvm::CallInst* call, const llvm::Function* callee, 
                     const TaintFact& fact) {
    FactSet result;
    
    if (fact.is_zero()) {
        result.insert(fact);
        return result;
    }
    
    if (!callee || callee->isDeclaration()) {
        return result;
    }
    
    // Map caller facts to callee facts
    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
        const llvm::Value* arg = call->getOperand(i);
        auto param_it = callee->arg_begin();
        std::advance(param_it, i);
        
        if (fact.is_tainted_var() && (arg == fact.get_value() || may_alias(arg, fact.get_value()))) {
            result.insert(TaintFact::tainted_var(&*param_it));
        }
        
        if (fact.is_tainted_memory() && arg->getType()->isPointerTy() && 
            may_alias(arg, fact.get_memory_location())) {
            result.insert(TaintFact::tainted_memory(&*param_it));
        }
    }
    
    return result;
}

TaintAnalysis::FactSet TaintAnalysis::return_flow(const llvm::CallInst* call, const llvm::Function* callee,
                       const TaintFact& exit_fact, const TaintFact& call_fact) {
    FactSet result;
    
    if (exit_fact.is_zero()) {
        result.insert(exit_fact);
        return result;
    }
    
    // Map return values back to call site
    if (exit_fact.is_tainted_var()) {
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
    
    if (!call_fact.is_zero()) {
        result.insert(call_fact);
    }
    
    return result;
}

TaintAnalysis::FactSet TaintAnalysis::call_to_return_flow(const llvm::CallInst* call, const TaintFact& fact) {
    FactSet result;
    
    if (fact.is_zero()) {
        result.insert(fact);
        return result;
    }
    
    const llvm::Function* callee = call->getCalledFunction();
    if (!callee) {
        result.insert(fact);
        return result;
    }
    
    std::string func_name = callee->getName().str();
    
    // Handle source functions
    if (m_source_functions.count(func_name)) {
        result.insert(TaintFact::tainted_var(call));
    }
    
    // Propagate facts that are not killed by the call
    if (!kills_fact(call, fact)) {
        result.insert(fact);
    }
    
    return result;
}

TaintAnalysis::FactSet TaintAnalysis::initial_facts(const llvm::Function* main) {
    FactSet result;
    result.insert(zero_fact());
    
    // Taint command line arguments
    for (const llvm::Argument& arg : main->args()) {
        if (arg.getType()->isPointerTy()) {
            result.insert(TaintFact::tainted_var(&arg));
        }
    }
    
    return result;
}

bool TaintAnalysis::is_source(const llvm::Instruction* inst) const {
    auto* call = llvm::dyn_cast<llvm::CallInst>(inst);
    return call && call->getCalledFunction() && 
           m_source_functions.count(call->getCalledFunction()->getName().str()) > 0;
}

bool TaintAnalysis::is_sink(const llvm::Instruction* inst) const {
    auto* call = llvm::dyn_cast<llvm::CallInst>(inst);
    return call && call->getCalledFunction() && 
           m_sink_functions.count(call->getCalledFunction()->getName().str()) > 0;
}

void TaintAnalysis::add_source_function(const std::string& func_name) {
    m_source_functions.insert(func_name);
}

void TaintAnalysis::add_sink_function(const std::string& func_name) {
    m_sink_functions.insert(func_name);
}

bool TaintAnalysis::kills_fact(const llvm::CallInst* call, const TaintFact& fact) const {
    const llvm::Function* callee = call->getCalledFunction();
    if (!callee || !fact.is_tainted_var()) return false;
    
    static const std::unordered_set<std::string> sanitizers = {
        "strlen", "strcmp", "strncmp", "isdigit", "isalpha"
    };
    
    if (sanitizers.count(callee->getName().str())) {
        for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
            if (call->getOperand(i) == fact.get_value()) {
                return true;
            }
        }
    }
    
    return false;
}

void TaintAnalysis::report_vulnerabilities(const IFDSSolver<TaintAnalysis>& solver, 
                                          llvm::raw_ostream& OS, 
                                          size_t max_vulnerabilities) const {
    OS << "\nTaint Flow Vulnerability Analysis:\n";
    OS << "==================================\n";
    
    const auto& results = solver.get_all_results();
    size_t vulnerability_count = 0;
    
    for (const auto& result : results) {
        const auto& node = result.first;
        const auto& facts = result.second;
        
        if (facts.empty() || !node.instruction) continue;
        
        auto* call = llvm::dyn_cast<llvm::CallInst>(node.instruction);
        if (!call || !is_sink(call)) continue;
        
        std::string func_name = call->getCalledFunction()->getName().str();
        std::string tainted_args;
        
        for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
            const llvm::Value* arg = call->getOperand(i);
            
            for (const auto& fact : facts) {
                if (fact.is_tainted_var() && fact.get_value() == arg) {
                    if (!tainted_args.empty()) tainted_args += ", ";
                    tainted_args += "arg" + std::to_string(i);
                    break;
                }
            }
        }
        
        if (!tainted_args.empty()) {
            vulnerability_count++;
            if (vulnerability_count <= max_vulnerabilities) {
                OS << "\n🚨 VULNERABILITY #" << vulnerability_count << ":\n";
                OS << "  Sink: " << func_name << " at " << *call << "\n";
                OS << "  Tainted arguments: " << tainted_args << "\n";
                OS << "  Location: " << call->getDebugLoc() << "\n";
            }
        }
    }
    
    if (vulnerability_count == 0) {
        OS << "✅ No taint flow vulnerabilities detected.\n";
        OS << "   (This means no tainted data reached dangerous sink functions)\n";
    } else {
        OS << "\n📊 Summary:\n";
        OS << "  Total vulnerabilities found: " << vulnerability_count << "\n";
        if (vulnerability_count > max_vulnerabilities) {
            OS << "  (Showing first " << max_vulnerabilities << " vulnerabilities)\n";
        }
    }
}

} // namespace ifds

// Hash function implementation
namespace std {
size_t hash<ifds::TaintFact>::operator()(const ifds::TaintFact& fact) const {
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
