#include "Dataflow/IFDS/Clients/IDETypeState.h"
#include <llvm/IR/Instructions.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GlobalVariable.h>
#include <regex>

namespace ifds {

// ============================================================================
// TypeStateProperty Implementation
// ============================================================================

std::vector<TypeStateProperty::Transition> 
TypeStateProperty::get_transitions(const llvm::Instruction* inst) const {
    std::vector<Transition> result;
    
    for (const auto& pair : m_operation_transitions) {
        if (matcher_matches(pair.first, inst)) {
            result.insert(result.end(), pair.second.begin(), pair.second.end());
        }
    }
    
    return result;
}

bool TypeStateProperty::matcher_matches(const OperationMatcher& matcher, 
                                        const llvm::Instruction* inst) const {
    switch (matcher.type) {
        case OperationMatcher::FUNCTION_NAME: {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
                if (const llvm::Function* callee = call->getCalledFunction()) {
                    return callee->getName() == matcher.pattern;
                }
            }
            return false;
        }
        
        case OperationMatcher::FUNCTION_REGEX: {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
                if (const llvm::Function* callee = call->getCalledFunction()) {
                    try {
                        std::regex re(matcher.pattern);
                        return std::regex_match(callee->getName().str(), re);
                    } catch (...) {
                        return false;
                    }
                }
            }
            return false;
        }
        
        case OperationMatcher::INSTRUCTION_OPCODE: {
            return std::string(inst->getOpcodeName()) == matcher.pattern;
        }
        
        case OperationMatcher::CUSTOM_PREDICATE: {
            return matcher.predicate && matcher.predicate(inst);
        }
    }
    
    return false;
}

// ============================================================================
// IDETypeState Implementation
// ============================================================================

IDETypeState::IDETypeState(std::shared_ptr<TypeStateProperty> property)
    : m_property(std::move(property)), m_track_globals(true), m_track_heap(true) {
    if (!m_property) {
        throw std::runtime_error("IDETypeState: property cannot be null");
    }
}

IDETypeState::FactSet IDETypeState::initial_facts(const llvm::Function* main) {
    FactSet seeds;
    
    // Track function arguments
    for (const llvm::Argument& arg : main->args()) {
        if (should_track(&arg)) {
            seeds.insert(&arg);
        }
    }
    
    // Track global variables if enabled
    if (m_track_globals) {
        const llvm::Module* module = main->getParent();
        for (const llvm::GlobalVariable& gv : module->globals()) {
            if (should_track(&gv)) {
                seeds.insert(&gv);
            }
        }
    }
    
    return seeds;
}

IDETypeState::Value IDETypeState::join(const Value& v1, const Value& v2) const {
    // Bottom is identity
    if (v1.is_bottom()) return v2;
    if (v2.is_bottom()) return v1;
    
    // Top absorbs everything
    if (v1.is_top() || v2.is_top()) return Value(TypeStateValue::TOP);
    
    // Same state
    if (v1 == v2) return v1;
    
    // Conflicting states -> Top (unknown)
    return Value(TypeStateValue::TOP);
}

IDETypeState::FactSet IDETypeState::normal_flow(const llvm::Instruction* stmt, const Fact& fact) {
    FactSet out;
    
    // Keep existing fact (unless killed by an aliasing store)
    bool fact_killed = false;
    
    // For stores: kill facts that may alias with the stored pointer
    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
        const llvm::Value* stored_val = store->getValueOperand();
        const llvm::Value* ptr = store->getPointerOperand();
        
        // If the fact aliases with the pointer being stored to, it may be killed
        if (fact && may_alias(fact, ptr)) {
            fact_killed = true;
        }
        
        // Propagate taint from stored value to pointer
        if (fact == stored_val && should_track(ptr)) {
            out.insert(ptr);
        }
        
        // Track the pointer itself
        if (should_track(ptr)) {
            out.insert(ptr);
        }
    }
    // For loads: propagate state from memory to register
    else if (auto* load = llvm::dyn_cast<llvm::LoadInst>(stmt)) {
        const llvm::Value* ptr = load->getPointerOperand();
        
        // If fact aliases with loaded pointer, propagate to load result
        if (fact && may_alias(fact, ptr) && should_track(load)) {
            out.insert(load);
        }
        
        if (should_track(load)) {
            out.insert(load);
        }
    }
    // For PHI nodes: propagate from incoming values
    else if (auto* phi = llvm::dyn_cast<llvm::PHINode>(stmt)) {
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            if (fact == phi->getIncomingValue(i)) {
                out.insert(phi);
            }
        }
    }
    // For casts: propagate aliases
    else if (auto* cast = llvm::dyn_cast<llvm::CastInst>(stmt)) {
        if (fact == cast->getOperand(0) && should_track(cast)) {
            out.insert(cast);
        }
    }
    // For GEP: track field accesses
    else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(stmt)) {
        if (fact == gep->getPointerOperand() && should_track(gep)) {
            out.insert(gep);
        }
    }
    
    // Keep existing fact unless killed
    if (fact && should_track(fact) && !fact_killed) {
        out.insert(fact);
    }
    
    // Generate new fact for instruction result
    if (!stmt->getType()->isVoidTy() && should_track(stmt)) {
        out.insert(stmt);
    }
    
    return out;
}

IDETypeState::FactSet IDETypeState::call_flow(const llvm::CallInst* call, 
                                               const llvm::Function* callee, 
                                               const Fact& fact) {
    FactSet out;
    
    if (!callee || callee->isDeclaration()) {
        return out;
    }
    
    // Map actual arguments to formal parameters (with alias analysis)
    for (unsigned i = 0; i < call->arg_size() && i < callee->arg_size(); ++i) {
        const llvm::Value* arg = call->getArgOperand(i);
        if (!arg) continue;
        
        auto param_it = callee->arg_begin();
        std::advance(param_it, i);
        if (param_it == callee->arg_end()) break;
        
        const llvm::Argument* formal = &*param_it;
        
        // Direct match or may-alias
        if (fact == arg) {
            if (should_track(formal)) {
                out.insert(formal);
            }
        } else if (fact && arg->getType()->isPointerTy() && 
                   fact->getType()->isPointerTy() && may_alias(arg, fact)) {
            // Fact aliases with argument, propagate to formal
            if (should_track(formal)) {
                out.insert(formal);
            }
        }
    }
    
    return out;
}

IDETypeState::FactSet IDETypeState::return_flow(const llvm::CallInst* call, 
                                                 const llvm::Function* callee,
                                                 const Fact& exit_fact, 
                                                 const Fact& call_fact) {
    (void)callee;
    (void)exit_fact;
    
    FactSet out;
    
    // Pass through the call_fact
    if (call_fact && should_track(call_fact)) {
        out.insert(call_fact);
    }
    
    // Return value becomes a new fact
    if (!call->getType()->isVoidTy() && should_track(call)) {
        out.insert(call);
    }
    
    return out;
}

IDETypeState::FactSet IDETypeState::call_to_return_flow(const llvm::CallInst* call, 
                                                         const Fact& fact) {
    FactSet out;
    
    // Keep the fact (it's not killed by the call)
    if (fact && should_track(fact)) {
        out.insert(fact);
    }
    
    // Call result is a new fact
    if (!call->getType()->isVoidTy() && should_track(call)) {
        out.insert(call);
    }
    
    return out;
}

IDETypeState::EdgeFunction 
IDETypeState::normal_edge_function(const llvm::Instruction* stmt, 
                                   const Fact& src_fact, 
                                   const Fact& tgt_fact) {
    (void)src_fact;
    (void)tgt_fact;
    
    // Get transitions for this instruction
    auto transitions = m_property->get_transitions(stmt);
    
    if (transitions.empty()) {
        // Identity function
        return [](const Value& v) { return v; };
    }
    
    return make_transition_function(transitions);
}

IDETypeState::EdgeFunction 
IDETypeState::call_edge_function(const llvm::CallInst* call, 
                                 const Fact& src_fact, 
                                 const Fact& tgt_fact) {
    (void)src_fact;
    (void)tgt_fact;
    
    // Get transitions for this call
    auto transitions = m_property->get_transitions(call);
    
    if (transitions.empty()) {
        // Identity function
        return [](const Value& v) { return v; };
    }
    
    return make_transition_function(transitions);
}

IDETypeState::EdgeFunction 
IDETypeState::return_edge_function(const llvm::CallInst* call, 
                                   const Fact& exit_fact, 
                                   const Fact& ret_fact) {
    (void)call;
    (void)exit_fact;
    (void)ret_fact;
    
    // Identity function (transitions happen at call site)
    return [](const Value& v) { return v; };
}

IDETypeState::EdgeFunction 
IDETypeState::call_to_return_edge_function(const llvm::CallInst* call, 
                                           const Fact& src_fact, 
                                           const Fact& tgt_fact) {
    (void)call;
    (void)src_fact;
    (void)tgt_fact;
    
    // Identity function (local facts are preserved)
    return [](const Value& v) { return v; };
}

bool IDETypeState::should_track(const llvm::Value* val) const {
    if (!val) return false;
    
    // Check if it's a global and we're tracking globals
    if (llvm::isa<llvm::GlobalVariable>(val)) {
        return m_track_globals;
    }
    
    // Check if it's heap-allocated (result of malloc, new, etc.) and we're tracking heap
    if (m_track_heap) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(val)) {
            if (const llvm::Function* callee = call->getCalledFunction()) {
                llvm::StringRef name = callee->getName();
                if (name == "malloc" || name == "calloc" || name == "realloc" ||
                    name == "_Znwm" || name == "_Znam") { // new, new[]
                    return true;
                }
            }
        }
    }
    
    // Check if the type matches tracked types
    if (!m_tracked_types.empty() && val->getType()) {
        llvm::Type* ty = val->getType();
        if (auto* ptr_ty = llvm::dyn_cast<llvm::PointerType>(ty)) {
            llvm::Type* elem_ty = ptr_ty->getPointerElementType();
            if (auto* struct_ty = llvm::dyn_cast<llvm::StructType>(elem_ty)) {
                if (struct_ty->hasName()) {
                    std::string type_name = struct_ty->getName().str();
                    if (m_tracked_types.count(type_name) > 0) {
                        return true;
                    }
                }
            }
        }
    }
    
    // By default, track all pointer types
    return val->getType() && val->getType()->isPointerTy();
}

IDETypeState::EdgeFunction 
IDETypeState::make_transition_function(
    const std::vector<TypeStateProperty::Transition>& transitions) const {
    
    // Capture transitions by value
    return [transitions](const Value& v) -> Value {
        // If value is top or bottom, no transition
        if (v.is_special()) {
            return v;
        }
        
        // Apply first matching transition
        int current_state = v.user_state();
        for (const auto& trans : transitions) {
            if (trans.from_state == current_state) {
                return Value(trans.to_state);
            }
        }
        
        // No matching transition, return unchanged
        return v;
    };
}

// ============================================================================
// Predefined Typestate Properties
// ============================================================================

namespace predefined {

std::shared_ptr<TypeStateProperty> create_file_property() {
    auto prop = std::make_shared<TypeStateProperty>("File");
    
    // Define states
    int closed = prop->define_state("Closed");
    int opened = prop->define_state("Opened");
    int error = prop->define_state("Error", true);
    
    prop->set_initial_state(closed);
    
    // Transitions
    prop->add_transition_for_function("fopen", closed, opened);
    prop->add_transition_for_function("open", closed, opened);
    prop->add_transition_for_function("fclose", opened, closed);
    prop->add_transition_for_function("close", opened, closed);
    
    // Error transitions (operating on closed file)
    prop->add_transition_for_function("fread", closed, error);
    prop->add_transition_for_function("fwrite", closed, error);
    prop->add_transition_for_function("read", closed, error);
    prop->add_transition_for_function("write", closed, error);
    
    // Double close
    prop->add_transition_for_function("fclose", closed, error);
    prop->add_transition_for_function("close", closed, error);
    
    return prop;
}

std::shared_ptr<TypeStateProperty> create_lock_property() {
    auto prop = std::make_shared<TypeStateProperty>("Lock");
    
    // Define states
    int unlocked = prop->define_state("Unlocked");
    int locked = prop->define_state("Locked");
    int error = prop->define_state("Error", true);
    
    prop->set_initial_state(unlocked);
    
    // Transitions
    prop->add_transition_for_function("pthread_mutex_lock", unlocked, locked);
    prop->add_transition_for_function("pthread_mutex_unlock", locked, unlocked);
    
    // Error transitions
    prop->add_transition_for_function("pthread_mutex_lock", locked, error); // double lock
    prop->add_transition_for_function("pthread_mutex_unlock", unlocked, error); // unlock unlocked
    
    return prop;
}

std::shared_ptr<TypeStateProperty> create_memory_property() {
    auto prop = std::make_shared<TypeStateProperty>("Memory");
    
    // Define states
    int unallocated = prop->define_state("Unallocated");
    int allocated = prop->define_state("Allocated");
    int freed = prop->define_state("Freed");
    int error = prop->define_state("Error", true);
    
    prop->set_initial_state(unallocated);
    
    // Allocation
    prop->add_transition_for_function("malloc", unallocated, allocated);
    prop->add_transition_for_function("calloc", unallocated, allocated);
    prop->add_transition_for_function("realloc", unallocated, allocated);
    
    // Free
    prop->add_transition_for_function("free", allocated, freed);
    
    // Error transitions
    prop->add_transition_for_function("free", freed, error); // double free
    prop->add_transition_for_function("free", unallocated, error); // free unallocated
    
    return prop;
}

std::shared_ptr<TypeStateProperty> create_socket_property() {
    auto prop = std::make_shared<TypeStateProperty>("Socket");
    
    // Define states
    int uninitialized = prop->define_state("Uninitialized");
    int created = prop->define_state("Created");
    int bound = prop->define_state("Bound");
    int listening = prop->define_state("Listening");
    int connected = prop->define_state("Connected");
    int closed = prop->define_state("Closed");
    int error = prop->define_state("Error", true);
    
    prop->set_initial_state(uninitialized);
    
    // Normal flow
    prop->add_transition_for_function("socket", uninitialized, created);
    prop->add_transition_for_function("bind", created, bound);
    prop->add_transition_for_function("listen", bound, listening);
    prop->add_transition_for_function("accept", listening, connected);
    prop->add_transition_for_function("connect", created, connected);
    prop->add_transition_for_function("close", connected, closed);
    prop->add_transition_for_function("close", listening, closed);
    prop->add_transition_for_function("close", bound, closed);
    
    // Error transitions (operations in wrong state)
    prop->add_transition_for_function("bind", uninitialized, error);
    prop->add_transition_for_function("listen", created, error);
    prop->add_transition_for_function("accept", created, error);
    prop->add_transition_for_function("send", created, error);
    prop->add_transition_for_function("recv", created, error);
    
    return prop;
}

} // namespace predefined

} // namespace ifds
