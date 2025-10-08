/*
 * Taint Analysis Implementation
 */

#include <Analysis/IFDS/Clients/IFDSTaintAnalysis.h>
#include <Annotation/Taint/TaintConfigManager.h>

#include <iostream>
#include <llvm/Support/raw_ostream.h>

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
    
    // Always propagate zero fact
    if (fact.is_zero()) {
        result.insert(fact);
    }
    
    const llvm::Function* callee = call->getCalledFunction();
    if (!callee) {
        if (!fact.is_zero() && !kills_fact(call, fact)) {
            result.insert(fact);
        }
        return result;
    }
    
    std::string func_name = taint_config::normalize_name(callee->getName().str());
    
    // Note: Source function handling is now done using config specifications below
    // instead of this simple check that only handles return values
    
    // Handle taint propagation through string formatting functions (sprintf, snprintf)
    // These functions take tainted input and write to an output buffer
    // Note: Function names are already normalized, so no need to include _chk variants
    static const std::unordered_set<std::string> format_functions = {
        "sprintf", "snprintf", "vsprintf", "vsnprintf"
    };
    
    if (format_functions.count(func_name)) {
        // Check if any of the format arguments (after the format string) are tainted
        bool has_tainted_input = false;
        
        // Determine where format arguments start based on function signature
        unsigned format_arg_start = 1;  // After destination buffer
        if (func_name == "snprintf" || func_name == "vsnprintf") {
            // snprintf has size parameter: dest, size, format, ...
            format_arg_start = 2;
        }
        // Note: sprintf/vsprintf are: dest, format, ...
        // The normalization already handles fortified versions
        
        // Check if format arguments are tainted
        for (unsigned i = format_arg_start + 1; i < call->getNumOperands() - 1; ++i) {
            const llvm::Value* arg = call->getOperand(i);
            if (fact.is_tainted_var() && fact.get_value() == arg) {
                has_tainted_input = true;
                break;
            }
            if (fact.is_tainted_memory() && arg->getType()->isPointerTy() && 
                may_alias(arg, fact.get_memory_location())) {
                has_tainted_input = true;
                break;
            }
        }
        
        // If input is tainted, taint the output buffer
        if (has_tainted_input) {
            const llvm::Value* dest = call->getOperand(0);  // First arg is always destination
            if (dest->getType()->isPointerTy()) {
                result.insert(TaintFact::tainted_memory(dest));
                
                // Also handle aliasing
                if (m_alias_analysis) {
                    auto alias_set = get_alias_set(dest);
                    for (const llvm::Value* alias : alias_set) {
                        if (alias != dest && alias->getType()->isPointerTy()) {
                            result.insert(TaintFact::tainted_memory(alias));
                        }
                    }
                }
            }
        }
    }
    
    // Handle source functions using config specifications
    const FunctionTaintConfig* func_config = taint_config::get_function_config(func_name);
    
    if (func_config && func_config->has_source_specs()) {
        // Use detailed specifications from config file
        for (const auto& spec : func_config->source_specs) {
            if (spec.location == TaintSpec::RET && spec.access_mode == TaintSpec::VALUE) {
                // Return value is tainted
                result.insert(TaintFact::tainted_var(call));
            } else if (spec.location == TaintSpec::ARG && spec.access_mode == TaintSpec::DEREF) {
                // Specific argument's memory is tainted
                if (spec.arg_index >= 0 && spec.arg_index < (int)(call->getNumOperands() - 1)) {
                    const llvm::Value* arg = call->getOperand(spec.arg_index);
                    if (arg->getType()->isPointerTy()) {
                        result.insert(TaintFact::tainted_memory(arg));
                        
                        // Also handle aliasing
                        if (m_alias_analysis) {
                            auto alias_set = get_alias_set(arg);
                            for (const llvm::Value* alias : alias_set) {
                                if (alias != arg && alias->getType()->isPointerTy()) {
                                    result.insert(TaintFact::tainted_memory(alias));
                                }
                            }
                        }
                    }
                }
            } else if (spec.location == TaintSpec::AFTER_ARG && spec.access_mode == TaintSpec::DEREF) {
                // All arguments after a specific index are tainted
                unsigned start_arg = spec.arg_index + 1;
                for (unsigned i = start_arg; i < call->getNumOperands() - 1; ++i) {
                    const llvm::Value* arg = call->getOperand(i);
                    if (arg->getType()->isPointerTy()) {
                        result.insert(TaintFact::tainted_memory(arg));
                        
                        // Also handle aliasing
                        if (m_alias_analysis) {
                            auto alias_set = get_alias_set(arg);
                            for (const llvm::Value* alias : alias_set) {
                                if (alias != arg && alias->getType()->isPointerTy()) {
                                    result.insert(TaintFact::tainted_memory(alias));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // Handle PIPE specifications for taint propagation
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
                                
                                if (m_alias_analysis) {
                                    auto alias_set = get_alias_set(to_arg);
                                    for (const llvm::Value* alias : alias_set) {
                                        if (alias != to_arg && alias->getType()->isPointerTy()) {
                                            result.insert(TaintFact::tainted_memory(alias));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
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

// Helper to find source instructions by backward traversal
// TODO: is this an elegant (or correct) way? A more natural (but potentially more compelx) way is to "log" the analysis process (e.g. track "value flows")
TaintAnalysis::TaintPath TaintAnalysis::find_sources_for_sink(
    const IFDSSolver<TaintAnalysis>& solver,
    const llvm::CallInst* sink_call,
    const TaintFact& tainted_fact) const {
    
    TaintPath result;
    std::unordered_set<const llvm::Instruction*> visited;
    std::unordered_set<const llvm::Function*> visited_functions;
    std::vector<const llvm::Instruction*> worklist;
    worklist.push_back(sink_call);
    
    const auto& path_edges = solver.get_path_edges();
    
    // Track intermediate functions for path visualization
    visited_functions.insert(sink_call->getFunction());
    
    // Backward search with depth limit to avoid performance issues
    int depth = 0;
    const int max_depth = 1000;
    
    while (!worklist.empty() && result.sources.size() < 5 && depth < max_depth) {
        const llvm::Instruction* current = worklist.back();
        worklist.pop_back();
        depth++;
        
        if (visited.count(current)) continue;
        visited.insert(current);
        
        // Track functions in the path (intermediate functions between source and sink)
        const llvm::Function* current_func = current->getFunction();
        if (!visited_functions.count(current_func) && result.intermediate_functions.size() < 8) {
            visited_functions.insert(current_func);
            result.intermediate_functions.push_back(current_func);
        }
        
        // Check if this instruction is a source function
        if (is_source(current)) {
            result.sources.push_back(current);
            continue; // Don't traverse beyond sources
        }
        
        // Check if this is a function entry with tainted arguments (initial taint)
        if (current == &current->getFunction()->getEntryBlock().front()) {
            // This could be entry point with tainted arguments
            result.sources.push_back(current);
            continue;
        }
        
        // Find predecessor path edges
        bool found_predecessor = false;
        for (const auto& edge : path_edges) {
            if (edge.target_node == current && !visited.count(edge.start_node)) {
                worklist.push_back(edge.start_node);
                found_predecessor = true;
            }
        }
        
        // If no predecessors found, this might be a source
        if (!found_predecessor && current != sink_call) {
            result.sources.push_back(current);
        }
    }
    
    // Reverse intermediate functions so they go from source to sink
    std::reverse(result.intermediate_functions.begin(), result.intermediate_functions.end());
    
    return result;
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
        
        std::string func_name = taint_config::normalize_name(call->getCalledFunction()->getName().str());
        
        std::string tainted_args;
        std::vector<const llvm::Instruction*> all_sources;
        std::vector<const llvm::Function*> propagation_path;
        
        for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
            const llvm::Value* arg = call->getOperand(i);
            
            for (const auto& fact : facts) {
                bool found_tainted = false;
                // Check if the argument itself is tainted
                if (fact.is_tainted_var() && fact.get_value() == arg) {
                    if (!tainted_args.empty()) tainted_args += ", ";
                    tainted_args += "arg" + std::to_string(i);
                    found_tainted = true;
                }
                // Check if the argument points to tainted memory (direct match)
                else if (fact.is_tainted_memory() && arg->getType()->isPointerTy()) {
                    if (fact.get_memory_location() == arg) {
                        if (!tainted_args.empty()) tainted_args += ", ";
                        tainted_args += "arg" + std::to_string(i) + "(mem)";
                        found_tainted = true;
                    }
                    // Check if the argument may alias with tainted memory
                    else if (may_alias(arg, fact.get_memory_location())) {
                        if (!tainted_args.empty()) tainted_args += ", ";
                        tainted_args += "arg" + std::to_string(i) + "(alias)";
                        found_tainted = true;
                    }
                }
                
                // Find sources for this tainted fact
                if (found_tainted) {
                    auto path = find_sources_for_sink(solver, call, fact);
                    all_sources.insert(all_sources.end(), path.sources.begin(), path.sources.end());
                    if (propagation_path.empty()) {
                        propagation_path = path.intermediate_functions;
                    }
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
                
                // Display sources
                if (!all_sources.empty()) {
                    OS << "  \n  📍 Taint Sources:\n";
                    std::unordered_set<const llvm::Instruction*> unique_sources(all_sources.begin(), all_sources.end());
                    int source_num = 1;
                    for (const auto* source : unique_sources) {
                        if (auto* source_call = llvm::dyn_cast<llvm::CallInst>(source)) {
                            if (source_call->getCalledFunction()) {
                                std::string source_func = taint_config::normalize_name(
                                    source_call->getCalledFunction()->getName().str());
                                OS << "    " << source_num++ << ". " << source_func 
                                   << " in function " << source->getFunction()->getName()
                                   << " at " << source_call->getDebugLoc() << "\n";
                            }
                        } else {
                            // Function entry (tainted arguments)
                            if (source == &source->getFunction()->getEntryBlock().front()) {
                                OS << "    " << source_num++ << ". [Program Entry/Tainted Argument]"
                                   << " in function " << source->getFunction()->getName() << "\n";
                            } else {
                                OS << "    " << source_num++ << ". [Instruction]"
                                   << " in function " << source->getFunction()->getName()
                                   << " at " << source->getDebugLoc() << "\n";
                            }
                        }
                    }
                } else {
                    OS << "  \n  📍 Taint Sources: [Unable to determine - complex flow]\n";
                }
                
                // Display propagation path (intermediate functions)
                if (!propagation_path.empty() && propagation_path.size() > 1) {
                    OS << "  \n  🔗 Propagation Path:\n";
                    OS << "     ";
                    for (size_t i = 0; i < propagation_path.size() && i < 6; ++i) {
                        if (i > 0) OS << " → ";
                        OS << propagation_path[i]->getName().str();
                    }
                    if (propagation_path.size() > 6) {
                        OS << " → ... (" << (propagation_path.size() - 6) << " more)";
                    }
                    OS << " → " << call->getFunction()->getName().str();
                    OS << "\n";
                } else if (!all_sources.empty()) {
                    // Show simple path when in same function
                    OS << "  \n  🔗 Flow: Source and sink in same function (" 
                       << call->getFunction()->getName().str() << ")\n";
                }
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
