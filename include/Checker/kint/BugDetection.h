#pragma once

#include <llvm/IR/Instructions.h>
#include <llvm/ADT/DenseMap.h>
#include <z3++.h>

#include <map>
#include <set>

using namespace llvm;

// Forward declaration
namespace kint {
    struct crange;
}

namespace kint {

enum class interr { 
    INT_OVERFLOW, 
    DIV_BY_ZERO, 
    BAD_SHIFT, 
    ARRAY_OOB, 
    DEAD_TRUE_BR, 
    DEAD_FALSE_BR 
};

class BugDetection {
public:
    BugDetection() = default;
    ~BugDetection() = default;

    // Error marking
    template <interr err_t, typename I> 
    static typename std::enable_if<std::is_pointer<I>::value>::type mark_err(I inst);
    
    template <interr err_t, typename I> 
    static typename std::enable_if<!std::is_pointer<I>::value>::type mark_err(I& inst);

    // SMT-based bug detection
    void binary_check(BinaryOperator* op, 
                     z3::solver& solver,
                     const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym,
                     std::set<Instruction*>& overflow_insts,
                     std::set<Instruction*>& bad_shift_insts,
                     std::set<Instruction*>& div_zero_insts);

    // SMT expression generation
    z3::expr binary_op_propagate(BinaryOperator* op, const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym, z3::solver& solver);
    z3::expr cast_op_propagate(CastInst* op, const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym, z3::solver& solver);
    z3::expr v2sym(const Value* v, const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym_map, z3::solver& solver);

    // Range constraint generation
    bool add_range_cons(const crange& rng, const z3::expr& bv, z3::solver& solver);

    // Error reporting
    void mark_errors(const std::map<ICmpInst*, bool>& impossible_branches,
                    const std::set<GetElementPtrInst*>& gep_oob,
                    const std::set<Instruction*>& overflow_insts,
                    const std::set<Instruction*>& bad_shift_insts,
                    const std::set<Instruction*>& div_zero_insts);

};

} // namespace kint
