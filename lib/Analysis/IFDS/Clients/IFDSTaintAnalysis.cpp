/*
 * Taint Analysis Implementation
 */

#include <Analysis/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Annotation/Taint/TaintConfigManager.h>

#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <algorithm>

namespace ifds {

// ============================================================================
// TaintFact Implementation
// ============================================================================

TaintFact::TaintFact() : m_type(ZERO), m_value(nullptr), m_memory_location(nullptr), m_source_inst(nullptr) {}

TaintFact TaintFact::zero() { 
    return TaintFact(); 
}

TaintFact TaintFact::tainted_var(const llvm::Value* v, const llvm::Instruction* source) {
    TaintFact fact;
    fact.m_type = TAINTED_VAR;
    fact.m_value = v;
    fact.m_source_inst = source;
    return fact;
}

TaintFact TaintFact::tainted_memory(const llvm::Value* loc, const llvm::Instruction* source) {
    TaintFact fact;
    fact.m_type = TAINTED_MEMORY;
    fact.m_memory_location = loc;
    fact.m_source_inst = source;
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

const llvm::Instruction* TaintFact::get_source() const {
    return m_source_inst;
}

TaintFact TaintFact::with_source(const llvm::Instruction* source) const {
    TaintFact fact = *this;
    if (!fact.m_source_inst) { // Only set source if not already set
        fact.m_source_inst = source;
    }
    return fact;
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

    // Always propagate zero fact
    if (fact.is_zero()) {
        result.insert(fact);
        return result;
    }

    const llvm::Function* callee = call->getCalledFunction();
    if (!callee) {
        if (!kills_fact(call, fact)) {
            result.insert(fact);
        }
        return result;
    }

    // Handle source functions using config specifications
    handle_source_function_specs(call, result);

    // Handle PIPE specifications for taint propagation
    handle_pipe_specifications(call, fact, result);

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
    if (!call || !call->getCalledFunction()) return false;
    
    std::string func_name = taint_config::normalize_name(call->getCalledFunction()->getName().str());
    return m_source_functions.count(func_name) > 0;
}

bool TaintAnalysis::is_sink(const llvm::Instruction* inst) const {
    auto* call = llvm::dyn_cast<llvm::CallInst>(inst);
    if (!call || !call->getCalledFunction()) return false;
    
    std::string func_name = taint_config::normalize_name(call->getCalledFunction()->getName().str());
    return m_sink_functions.count(func_name) > 0;
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


// ============================================================================
// Summary Edge-Based Trace Reconstruction
// ============================================================================

// Helper to find sources using function summary reconstruction
TaintAnalysis::TaintPath TaintAnalysis::trace_taint_sources_summary_based(
    const IFDSSolver<TaintAnalysis>& solver,
    const llvm::CallInst* sink_call,
    const TaintFact& tainted_fact) const {
    (void)tainted_fact; // Suppress unused parameter warning

    TaintAnalysis::TaintPath result;

    // Get all summary edges from the solver
    std::vector<SummaryEdge<TaintFact>> summary_edges;
    solver.get_summary_edges(summary_edges);

    // Build maps for efficient lookup
    std::unordered_map<const llvm::CallInst*, std::vector<SummaryEdge<TaintFact>>> call_to_summaries;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> function_to_calls;

    for (const auto& edge : summary_edges) {
        call_to_summaries[edge.call_site].push_back(edge);

        // Build call graph
        const llvm::Function* caller_func = edge.call_site->getFunction();
        function_to_calls[caller_func].push_back(edge.call_site);
    }

    // Use a more sophisticated approach: trace back through the summary edges
    // starting from the sink call and following the taint flow backwards
    std::unordered_set<const llvm::CallInst*> visited_calls;
    std::unordered_set<const llvm::Function*> visited_functions;
    std::vector<const llvm::CallInst*> worklist;
    worklist.push_back(sink_call);

    while (!worklist.empty() && result.sources.size() < 10) {
        const llvm::CallInst* current_call = worklist.back();
        worklist.pop_back();

        if (visited_calls.count(current_call)) continue;
        visited_calls.insert(current_call);

        const llvm::Function* current_func = current_call->getFunction();

        // Check if this is a source function
        if (is_source(current_call)) {
            result.sources.push_back(current_call);
            continue;
        }

        // Track intermediate functions
        if (visited_functions.insert(current_func).second && result.intermediate_functions.size() < 10) {
            result.intermediate_functions.push_back(current_func);
        }

        // Check for initial taint from function entry
        if (current_call == &current_func->getEntryBlock().front()) {
            result.sources.push_back(current_call);
            continue;
        }

        // Find all summary edges for this call site
                    auto summaries = call_to_summaries.find(current_call);
                    if (summaries != call_to_summaries.end()) {
                        for (const auto& summary : summaries->second) {
                            (void)summary; // Suppress unused variable warning
                            // Check if this summary edge could contribute to our tainted fact
                // This is where we'd need more sophisticated taint flow analysis
                // For now, we'll use a simplified approach

                // Look for calls in the same function that could flow taint to this call
                auto calls_in_func = function_to_calls.find(current_func);
                if (calls_in_func != function_to_calls.end()) {
                    for (const llvm::CallInst* other_call : calls_in_func->second) {
                        if (other_call != current_call && comes_before(other_call, current_call)) {
                            // Check if this other call's summary edges could flow to our call
                            auto other_summaries = call_to_summaries.find(other_call);
                            if (other_summaries != call_to_summaries.end()) {
                                for (const auto& other_summary : other_summaries->second) {
                                    (void)other_summary; // Suppress unused variable warning
                                    // Simplified check: if the return fact could flow to our call fact
                                    if (!visited_calls.count(other_call)) {
                                        worklist.push_back(other_call);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Also look for inter-procedural flows (calls from other functions)
        // This requires more sophisticated call graph analysis
        for (const auto& edge : summary_edges) {
            if (edge.call_site->getCalledFunction() == current_func) {
                // This call invokes the current function - trace back to the caller
                if (!visited_calls.count(edge.call_site)) {
                    worklist.push_back(edge.call_site);
                }
            }
        }
    }

    // Reverse intermediate functions so they go from source to sink
    std::reverse(result.intermediate_functions.begin(), result.intermediate_functions.end());

    return result;
}

// Overload for ParallelIFDSSolver (uses same implementation as sequential version)
TaintAnalysis::TaintPath TaintAnalysis::trace_taint_sources_summary_based(
    const ParallelIFDSSolver<TaintAnalysis>& solver,
    const llvm::CallInst* sink_call,
    const TaintFact& tainted_fact) const {
    (void)tainted_fact; // Suppress unused parameter warning

    TaintAnalysis::TaintPath result;

    // Get all summary edges from the solver
    std::vector<SummaryEdge<TaintFact>> summary_edges;
    solver.get_summary_edges(summary_edges);

    // Build maps for efficient lookup
    std::unordered_map<const llvm::CallInst*, std::vector<SummaryEdge<TaintFact>>> call_to_summaries;
    std::unordered_map<const llvm::Function*, std::vector<const llvm::CallInst*>> function_to_calls;

    for (const auto& edge : summary_edges) {
        call_to_summaries[edge.call_site].push_back(edge);

        // Build call graph
        const llvm::Function* caller_func = edge.call_site->getFunction();
        function_to_calls[caller_func].push_back(edge.call_site);
    }

    // Use a more sophisticated approach: trace back through the summary edges
    // starting from the sink call and following the taint flow backwards
    std::unordered_set<const llvm::CallInst*> visited_calls;
    std::unordered_set<const llvm::Function*> visited_functions;
    std::vector<const llvm::CallInst*> worklist;
    worklist.push_back(sink_call);

    while (!worklist.empty() && result.sources.size() < 10) {
        const llvm::CallInst* current_call = worklist.back();
        worklist.pop_back();

        if (visited_calls.count(current_call)) continue;
        visited_calls.insert(current_call);

        const llvm::Function* current_func = current_call->getFunction();

        // Check if this is a source function
        if (is_source(current_call)) {
            result.sources.push_back(current_call);
            continue;
        }

        // Track intermediate functions
        if (visited_functions.insert(current_func).second && result.intermediate_functions.size() < 10) {
            result.intermediate_functions.push_back(current_func);
        }

        // Check for initial taint from function entry
        if (current_call == &current_func->getEntryBlock().front()) {
            result.sources.push_back(current_call);
            continue;
        }

        // Find all summary edges for this call site
                    auto summaries = call_to_summaries.find(current_call);
                    if (summaries != call_to_summaries.end()) {
                        for (const auto& summary : summaries->second) {
                            (void)summary; // Suppress unused variable warning
                            // Check if this summary edge could contribute to our tainted fact
                // This is where we'd need more sophisticated taint flow analysis
                // For now, we'll use a simplified approach

                // Look for calls in the same function that could flow taint to this call
                auto calls_in_func = function_to_calls.find(current_func);
                if (calls_in_func != function_to_calls.end()) {
                    for (const llvm::CallInst* other_call : calls_in_func->second) {
                        if (other_call != current_call && comes_before(other_call, current_call)) {
                            // Check if this other call's summary edges could flow to our call
                            auto other_summaries = call_to_summaries.find(other_call);
                            if (other_summaries != call_to_summaries.end()) {
                                for (const auto& other_summary : other_summaries->second) {
                                    (void)other_summary; // Suppress unused variable warning
                                    // Simplified check: if the return fact could flow to our call fact
                                    if (!visited_calls.count(other_call)) {
                                        worklist.push_back(other_call);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Also look for inter-procedural flows (calls from other functions)
        // This requires more sophisticated call graph analysis
        for (const auto& edge : summary_edges) {
            if (edge.call_site->getCalledFunction() == current_func) {
                // This call invokes the current function - trace back to the caller
                if (!visited_calls.count(edge.call_site)) {
                    worklist.push_back(edge.call_site);
                }
            }
        }
    }

    // Reverse intermediate functions so they go from source to sink
    std::reverse(result.intermediate_functions.begin(), result.intermediate_functions.end());

    return result;
}

// Helper to check if one instruction comes before another in the same function
bool TaintAnalysis::comes_before(const llvm::Instruction* first, const llvm::Instruction* second) const {
    if (first->getFunction() != second->getFunction()) return false;

    const llvm::BasicBlock* first_bb = first->getParent();
    const llvm::BasicBlock* second_bb = second->getParent();

    // Simple check: if they're in different basic blocks, check block order
    if (first_bb != second_bb) {
        for (const llvm::BasicBlock& bb : *first->getFunction()) {
            if (&bb == first_bb) return true;
            if (&bb == second_bb) return false;
        }
        return false;
    }

    // Same basic block - check instruction order
    for (const llvm::Instruction& inst : *first_bb) {
        if (&inst == first) return true;
        if (&inst == second) return false;
    }
    return false;
}



// Helper function to handle source function specifications from config
void TaintAnalysis::handle_source_function_specs(const llvm::CallInst* call, FactSet& result) const {
    std::string func_name = taint_config::normalize_name(call->getCalledFunction()->getName().str());
    const FunctionTaintConfig* func_config = taint_config::get_function_config(func_name);

    if (func_config && func_config->has_source_specs()) {
        for (const auto& spec : func_config->source_specs) {
            if (spec.location == TaintSpec::RET && spec.access_mode == TaintSpec::VALUE) {
                result.insert(TaintFact::tainted_var(call));
            } else if (spec.location == TaintSpec::ARG && spec.access_mode == TaintSpec::DEREF) {
                if (spec.arg_index >= 0 && spec.arg_index < (int)(call->getNumOperands() - 1)) {
                    const llvm::Value* arg = call->getOperand(spec.arg_index);
                    if (arg->getType()->isPointerTy()) {
                        result.insert(TaintFact::tainted_memory(arg));
                    }
                }
            } else if (spec.location == TaintSpec::AFTER_ARG && spec.access_mode == TaintSpec::DEREF) {
                unsigned start_arg = spec.arg_index + 1;
                for (unsigned i = start_arg; i < call->getNumOperands() - 1; ++i) {
                    const llvm::Value* arg = call->getOperand(i);
                    if (arg->getType()->isPointerTy()) {
                        result.insert(TaintFact::tainted_memory(arg));
                    }
                }
            }
        }
    }
}

// Helper function to handle PIPE specifications for taint propagation
void TaintAnalysis::handle_pipe_specifications(const llvm::CallInst* call, const TaintFact& fact, FactSet& result) const {
    std::string func_name = taint_config::normalize_name(call->getCalledFunction()->getName().str());
    const FunctionTaintConfig* func_config = taint_config::get_function_config(func_name);

    if (func_config && func_config->has_pipe_specs()) {
        for (const auto& pipe_spec : func_config->pipe_specs) {
            bool matches_from = false;

            // Check if current fact matches the 'from' spec
            if (pipe_spec.from.location == TaintSpec::ARG) {
                int from_arg_idx = pipe_spec.from.arg_index;
                if (from_arg_idx >= 0 && from_arg_idx < (int)(call->getNumOperands() - 1)) {
                    const llvm::Value* from_arg = call->getOperand(from_arg_idx);

                    if (pipe_spec.from.access_mode == TaintSpec::VALUE) {
                        if (fact.is_tainted_var() && fact.get_value() == from_arg) {
                            matches_from = true;
                        }
                    } else {
                        if (fact.is_tainted_memory() && from_arg->getType()->isPointerTy()) {
                            if (may_alias(from_arg, fact.get_memory_location())) {
                                matches_from = true;
                            }
                        }
                    }
                }
            }

            // If from matches, propagate to 'to'
            if (matches_from) {
                if (pipe_spec.to.location == TaintSpec::RET) {
                    if (pipe_spec.to.access_mode == TaintSpec::VALUE) {
                        result.insert(TaintFact::tainted_var(call));
                    } else {
                        if (call->getType()->isPointerTy()) {
                            result.insert(TaintFact::tainted_memory(call));
                        }
                    }
                } else if (pipe_spec.to.location == TaintSpec::ARG) {
                    int to_arg_idx = pipe_spec.to.arg_index;
                    if (to_arg_idx >= 0 && to_arg_idx < (int)(call->getNumOperands() - 1)) {
                        const llvm::Value* to_arg = call->getOperand(to_arg_idx);

                        if (pipe_spec.to.access_mode == TaintSpec::VALUE) {
                            result.insert(TaintFact::tainted_var(to_arg));
                        } else {
                            if (to_arg->getType()->isPointerTy()) {
                            result.insert(TaintFact::tainted_memory(to_arg));
                            }
                        }
                    }
                }
            }
        }
    }
}

// Helper function to check if an argument is tainted by a fact
bool TaintAnalysis::is_argument_tainted(const llvm::Value* arg, const TaintFact& fact) const {
    return (fact.is_tainted_var() && fact.get_value() == arg) ||
           (fact.is_tainted_memory() && arg->getType()->isPointerTy() &&
            (fact.get_memory_location() == arg || may_alias(arg, fact.get_memory_location())));
}

// Helper function to format tainted argument description
std::string TaintAnalysis::format_tainted_arg(unsigned arg_index, const TaintFact& fact, const llvm::CallInst* call) const {
    if (fact.is_tainted_var()) {
        return "arg" + std::to_string(arg_index);
    } else if (fact.is_tainted_memory()) {
        return (fact.get_memory_location() == call->getOperand(arg_index)) ?
            "arg" + std::to_string(arg_index) + "(mem)" :
            "arg" + std::to_string(arg_index) + "(alias)";
    }
    return "";
}

// Helper function to analyze tainted arguments for a call
void TaintAnalysis::analyze_tainted_arguments(const llvm::CallInst* call, const TaintAnalysis::FactSet& facts,
                              std::string& tainted_args) const {
    std::set<std::string> unique_tainted_args;

    for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
        const llvm::Value* arg = call->getOperand(i);

        for (const auto& fact : facts) {
            if (is_argument_tainted(arg, fact)) {
                std::string arg_desc = format_tainted_arg(i, fact, call);
                if (!arg_desc.empty()) {
                    unique_tainted_args.insert(arg_desc);
                }
                break; // Found taint for this argument, move to next
            }
        }
    }

    // Join unique tainted arguments
    for (const auto& arg_desc : unique_tainted_args) {
        if (!tainted_args.empty()) tainted_args += ", ";
        tainted_args += arg_desc;
    }
}

// Helper function to output vulnerability report
void TaintAnalysis::output_vulnerability_report(llvm::raw_ostream& OS, size_t vuln_num,
                               const std::string& func_name, const llvm::CallInst* call,
                               const std::string& tainted_args,
                               const std::vector<const llvm::Instruction*>& all_sources,
                               const std::vector<const llvm::Function*>& propagation_path,
                               size_t max_vulnerabilities) const {
    if (vuln_num > max_vulnerabilities) return;

    OS << "\nVULNERABILITY #" << vuln_num << ":\n";
    OS << "  Sink: " << func_name << " (" << call->getDebugLoc() << ")\n";
    OS << "  Tainted args: " << tainted_args << "\n";

    // Display sources
    if (!all_sources.empty()) {
        OS << "  Sources:\n";
        std::unordered_set<const llvm::Instruction*> unique_sources(all_sources.begin(), all_sources.end());
        int source_num = 1;
        for (const auto* source : unique_sources) {
            if (auto* source_call = llvm::dyn_cast<llvm::CallInst>(source)) {
                if (source_call->getCalledFunction()) {
                    std::string source_func = taint_config::normalize_name(
                        source_call->getCalledFunction()->getName().str());
                    OS << "    " << source_num++ << ". " << source_func
                       << " (" << source->getFunction()->getName() << ":" << source_call->getDebugLoc() << ")\n";
                }
            } else {
                if (source == &source->getFunction()->getEntryBlock().front()) {
                    OS << "    " << source_num++ << ". [Entry: " << source->getFunction()->getName() << "]\n";
                } else {
                    OS << "    " << source_num++ << ". [Instr: " << source->getFunction()->getName()
                       << ":" << source->getDebugLoc() << "]\n";
                }
            }
        }
    } else {
        OS << "  Sources: [Complex flow]\n";
    }

    // Display propagation path (intermediate functions)
    if (!propagation_path.empty() && propagation_path.size() > 1) {
        OS << "  Path: ";
        for (size_t i = 0; i < propagation_path.size() && i < 6; ++i) {
            if (i > 0) OS << " → ";
            OS << propagation_path[i]->getName().str();
        }
        if (propagation_path.size() > 6) {
            OS << " → ... (+" << (propagation_path.size() - 6) << ")";
        }
        OS << " → " << call->getFunction()->getName().str() << "\n";
    } else if (!all_sources.empty()) {
        OS << "  Path: Same function (" << call->getFunction()->getName().str() << ")\n";
    }
}

// Template function for vulnerability reporting with different solver types
template<typename SolverType>
void report_vulnerabilities_impl(const TaintAnalysis& self, const SolverType& solver, llvm::raw_ostream& OS,
                                size_t max_vulnerabilities, const std::string& report_type) {
    OS << "\nTaint Analysis (" << report_type << "):\n";
    OS << std::string(20 + report_type.length(), '=') << "\n";

    const auto& results = solver.get_all_results();
    size_t vulnerability_count = 0;

    for (const auto& result : results) {
        const auto& node = result.first;
        const auto& facts = result.second;

        if (facts.empty() || !node.instruction) continue;

        auto* call = llvm::dyn_cast<llvm::CallInst>(node.instruction);
        if (!call || !self.is_sink(call)) continue;

        std::string func_name = taint_config::normalize_name(call->getCalledFunction()->getName().str());

        std::string tainted_args;
        std::vector<const llvm::Instruction*> all_sources;
        std::vector<const llvm::Function*> propagation_path;

        self.analyze_tainted_arguments(call, facts, tainted_args);

        if (!tainted_args.empty()) {
            vulnerability_count++;

            // Find sources using the appropriate solver method
            for (const auto& fact : facts) {
                bool found_tainted = false;

                // Check if any argument is tainted by this fact
                for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
                    if (self.is_argument_tainted(call->getOperand(i), fact)) {
                        found_tainted = true;
                        break;
                    }
                }

                if (found_tainted) {
                    auto path = self.trace_taint_sources_summary_based(solver, call, fact);
                    all_sources.insert(all_sources.end(), path.sources.begin(), path.sources.end());
                    if (propagation_path.empty()) {
                        propagation_path = path.intermediate_functions;
                    }
                    break;
                }
            }

            self.output_vulnerability_report(OS, vulnerability_count, func_name, call,
                                      tainted_args, all_sources, propagation_path, max_vulnerabilities);
        }
    }

    if (vulnerability_count == 0) {
        OS << "No vulnerabilities detected.\n";
    } else {
        OS << "\nSummary: " << vulnerability_count << " vulnerabilities";
        if (vulnerability_count > max_vulnerabilities) {
            OS << " (showing first " << max_vulnerabilities << ")";
        }
        OS << "\n";
    }
}

void TaintAnalysis::report_vulnerabilities(const IFDSSolver<TaintAnalysis>& solver,
                                          llvm::raw_ostream& OS,
                                          size_t max_vulnerabilities) const {
    report_vulnerabilities_impl(*this, solver, OS, max_vulnerabilities, "Sequential");
}

void TaintAnalysis::report_vulnerabilities(const ParallelIFDSSolver<TaintAnalysis>& solver,
                                          llvm::raw_ostream& OS,
                                          size_t max_vulnerabilities) const {
    report_vulnerabilities_impl(*this, solver, OS, max_vulnerabilities, "Parallel");
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
