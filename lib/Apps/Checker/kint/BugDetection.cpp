#include "Apps/Checker/kint/BugDetection.h"
#include "Apps/Checker/kint/RangeAnalysis.h"
#include "Apps/Checker/kint/Options.h"
#include "Apps/Checker/kint/Log.h"
#include "Apps/Checker/Report/SARIF.h"
#include "Utils/General/range.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Operator.h>

#include <z3++.h>


using namespace llvm;

namespace kint {

template <interr err, typename StrRet = const char*> constexpr StrRet mkstr() {
    if (err == interr::NONE) {
        return "none";
    } else if (err == interr::INT_OVERFLOW) {
        return "integer overflow";
    } else if (err == interr::DIV_BY_ZERO) {
        return "divide by zero";
    } else if (err == interr::BAD_SHIFT) {
        return "bad shift";
    } else if (err == interr::ARRAY_OOB) {
        return "array index out of bound";
    } else if (err == interr::DEAD_TRUE_BR) {
        return "impossible true branch";
    } else if (err == interr::DEAD_FALSE_BR) {
        return "impossible false branch";
    } else {
        static_assert(err == interr::NONE || err == interr::INT_OVERFLOW || err == interr::DIV_BY_ZERO || err == interr::BAD_SHIFT
                || err == interr::ARRAY_OOB || err == interr::DEAD_TRUE_BR || err == interr::DEAD_FALSE_BR,
            "unknown error type");
        return "";
    }
}

inline const char* mkstr(interr err) {
    switch (err) {
    case interr::NONE: return mkstr<interr::NONE>();
    case interr::INT_OVERFLOW: return mkstr<interr::INT_OVERFLOW>();
    case interr::DIV_BY_ZERO: return mkstr<interr::DIV_BY_ZERO>();
    case interr::BAD_SHIFT: return mkstr<interr::BAD_SHIFT>();
    case interr::ARRAY_OOB: return mkstr<interr::ARRAY_OOB>();
    case interr::DEAD_TRUE_BR: return mkstr<interr::DEAD_TRUE_BR>();
    case interr::DEAD_FALSE_BR: return mkstr<interr::DEAD_FALSE_BR>();
    default: MKINT_CHECK_ABORT(false) << "unknown error type" << static_cast<int>(err);
    }
    return "";
}

constexpr const char* MKINT_IR_ERR = "mkint.err";



template <interr err_t, typename I> 
typename std::enable_if<std::is_pointer<I>::value>::type BugDetection::mark_err(I inst) {
    auto& ctx = inst->getContext();
    std::string prefix = "";
    if (MDNode* omd = inst->getMetadata(MKINT_IR_ERR)) {
        prefix = cast<MDString>(omd->getOperand(0))->getString().str() + " + ";
    }
    auto md = MDNode::get(ctx, MDString::get(ctx, prefix + mkstr<err_t>()));
    inst->setMetadata(MKINT_IR_ERR, md);
}

template <interr err_t, typename I> 
typename std::enable_if<!std::is_pointer<I>::value>::type BugDetection::mark_err(I& inst) {
    mark_err<err_t>(&inst);
}

bool BugDetection::add_range_cons(const crange& rng, const z3::expr& bv, z3::solver& solver) {
    if (rng.isFullSet() || bv.is_const())
        return true;

    if (rng.isEmptySet()) {
        MKINT_CHECK_RELAX(false) << "lhs is empty set";
        return false;
    }

    solver.add(
        z3::ule(bv, solver.ctx().bv_val(rng.getUnsignedMax().getZExtValue(), rng.getBitWidth())));
    solver.add(
        z3::uge(bv, solver.ctx().bv_val(rng.getUnsignedMin().getZExtValue(), rng.getBitWidth())));
    return true;
}

void BugDetection::binary_check(BinaryOperator* op, 
                               z3::solver& solver,
                               const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym,
                               std::set<Instruction*>& overflow_insts,
                               std::set<Instruction*>& bad_shift_insts,
                               std::set<Instruction*>& div_zero_insts) {
    // Skip checks if all checkers are disabled
    if (!CheckIntOverflow && !CheckDivByZero && !CheckBadShift)
        return;
    const auto& lhs_bv = this->v2sym(op->getOperand(0), v2sym, solver);
    const auto& rhs_bv = this->v2sym(op->getOperand(1), v2sym, solver);
    const auto rhs_bits = rhs_bv.get_sort().bv_size();

    auto is_nsw_is_nuw = [op] {
        if (const auto ofop = dyn_cast<OverflowingBinaryOperator>(op)) {
            return std::make_pair(ofop->hasNoSignedWrap(), ofop->hasNoUnsignedWrap());
        }
        return std::make_pair(false, false);
    }();
    const auto is_nsw = is_nsw_is_nuw.first;

    const auto check = [&](interr et, bool is_signed) {
        if (solver.check() == z3::sat) { // counter example
            z3::model m = solver.get_model();
            MKINT_WARN() << rang::fg::yellow << rang::style::bold << mkstr(et) << rang::style::reset << " at "
                         << rang::bg::black << rang::fg::red << op->getParent()->getParent()->getName()
                         << "::" << *op << rang::style::reset;
            auto lhs_bin = m.eval(lhs_bv, true);
            auto rhs_bin = m.eval(rhs_bv, true);
            MKINT_WARN() << "Counter example: " << rang::bg::black << rang::fg::red << op->getOpcodeName()
                         << '(' << lhs_bin << ", " << rhs_bin << ") -> " << op->getOpcodeName() << '('
                         << (is_signed ? lhs_bin.as_int64() : lhs_bin.as_uint64()) << ", "
                         << (is_signed ? rhs_bin.as_int64() : rhs_bin.as_uint64()) << ')' << rang::style::reset;

            // Record bug with its path
            this->recordBugWithPath(op, et);

            switch (et) {
            case interr::INT_OVERFLOW:
                overflow_insts.insert(op);
                break;
            case interr::BAD_SHIFT:
                bad_shift_insts.insert(op);
                break;
            case interr::DIV_BY_ZERO:
                div_zero_insts.insert(op);
                break;
            default:
                break;
            }
        }
    };

    solver.push();
    switch (op->getOpcode()) {
    case Instruction::Add:
        if (!CheckIntOverflow)
            break;
            
        if (!is_nsw) { // unsigned
            solver.add(!z3::bvadd_no_overflow(lhs_bv, rhs_bv, false));
            check(interr::INT_OVERFLOW, false);
        } else {
            solver.add(!z3::bvadd_no_overflow(lhs_bv, rhs_bv, true));
            solver.add(!z3::bvadd_no_underflow(lhs_bv, rhs_bv));
            check(interr::INT_OVERFLOW, true);
        }
        break;
        
    case Instruction::Sub:
        if (!CheckIntOverflow)
            break;
            
        if (!is_nsw) {
            solver.add(!z3::bvsub_no_underflow(lhs_bv, rhs_bv, false));
            check(interr::INT_OVERFLOW, false);
        } else {
            solver.add(!z3::bvsub_no_underflow(lhs_bv, rhs_bv, true));
            solver.add(!z3::bvsub_no_overflow(lhs_bv, rhs_bv));
            check(interr::INT_OVERFLOW, true);
        }
        break;
        
    case Instruction::Mul:
        if (!CheckIntOverflow)
            break;
            
        if (!is_nsw) {
            solver.add(!z3::bvmul_no_overflow(lhs_bv, rhs_bv, false));
            check(interr::INT_OVERFLOW, false);
        } else {
            solver.add(!z3::bvmul_no_overflow(lhs_bv, rhs_bv, true));
            solver.add(!z3::bvmul_no_underflow(lhs_bv, rhs_bv)); // INTMAX * -1
            check(interr::INT_OVERFLOW, true);
        }
        break;
        
    case Instruction::URem:
    case Instruction::UDiv:
        if (!CheckDivByZero)
            break;
            
        solver.add(rhs_bv == solver.ctx().bv_val(0, rhs_bits));
        check(interr::DIV_BY_ZERO, false);
        break;
        
    case Instruction::SRem:
    case Instruction::SDiv: // can be overflow or divisor == 0
        if (CheckDivByZero) {
            solver.push();
            solver.add(rhs_bv == solver.ctx().bv_val(0, rhs_bits)); // may 0?
            check(interr::DIV_BY_ZERO, true);
            solver.pop();
        }
        
        if (CheckIntOverflow) {
            solver.add(z3::bvsdiv_no_overflow(lhs_bv, rhs_bv));
            check(interr::INT_OVERFLOW, true);
        }
        break;
        
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
        if (!CheckBadShift)
            break;
            
        solver.add(rhs_bv >= solver.ctx().bv_val(rhs_bits, rhs_bits)); // sat means bug
        check(interr::BAD_SHIFT, false);
        break;
        
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
        break;
        
    default:
        break;
    }
    solver.pop();
}

z3::expr BugDetection::binary_op_propagate(BinaryOperator* op, const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym, z3::solver& solver) {
    auto lhs = this->v2sym(op->getOperand(0), v2sym, solver);
    auto rhs = this->v2sym(op->getOperand(1), v2sym, solver);
    switch (op->getOpcode()) {
    case Instruction::Add:
        return lhs + rhs;
    case Instruction::Sub:
        return lhs - rhs;
    case Instruction::Mul:
        return lhs * rhs;
    case Instruction::URem:
        return z3::urem(lhs, rhs);
    case Instruction::UDiv:
        return z3::udiv(lhs, rhs);
    case Instruction::SRem:
        return z3::srem(lhs, rhs);
    case Instruction::SDiv: // can be overflow or divisor == 0
        return lhs / rhs;
    case Instruction::Shl:
        return z3::shl(lhs, rhs);
    case Instruction::LShr:
        return z3::lshr(lhs, rhs);
    case Instruction::AShr:
        return z3::ashr(lhs, rhs);
    case Instruction::And:
        return lhs & rhs;
    case Instruction::Or:
        return lhs | rhs;
    case Instruction::Xor:
        return lhs ^ rhs;
    default:
        break;
    }

    MKINT_CHECK_ABORT(false) << "unsupported binary op: " << *op;
    return lhs; // dummy
}

z3::expr BugDetection::v2sym(const Value* v, 
                             const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym_map,
                             z3::solver& solver) {
    auto it = v2sym_map.find(v);
    if (it != v2sym_map.end())
        return it->second.getValue();

    auto lconst = dyn_cast<ConstantInt>(v);
    MKINT_CHECK_ABORT(nullptr != lconst) << "unsupported value -> symbol mapping: " << *v;
    return solver.ctx().bv_val(lconst->getZExtValue(), lconst->getType()->getIntegerBitWidth());
}

void BugDetection::recordBugWithPath(const Instruction* inst, interr type) {
    if (!inst) return;
    
    // Create a new BugPath for this bug
    BugPath bugPath(inst, type);
    bugPath.path = m_current_path;
    
    // Store it in the map
    m_bug_paths[inst] = bugPath;
}

z3::expr BugDetection::cast_op_propagate(CastInst* op, const DenseMap<const Value*, llvm::Optional<z3::expr>>& v2sym, z3::solver& solver) {
    const auto src = this->v2sym(op->getOperand(0), v2sym, solver);
    const uint32_t bits = op->getType()->getIntegerBitWidth();
    switch (op->getOpcode()) {
    case CastInst::Trunc:
        return src.extract(bits - 1, 0);
    case CastInst::ZExt:
        return z3::zext(src, bits - op->getOperand(0)->getType()->getIntegerBitWidth());
    case CastInst::SExt:
        return z3::sext(src, bits - op->getOperand(0)->getType()->getIntegerBitWidth());
    default:
        MKINT_WARN() << "Unhandled Cast Instruction " << op->getOpcodeName() << ". Using original range.";
    }

    const std::string new_sym_str = "\%cast" + std::to_string(op->getValueID());
    return v2sym.begin()->second.getValue().ctx().bv_const(new_sym_str.c_str(), bits); // new expr
}


void BugDetection::mark_errors(const std::map<ICmpInst*, bool>& impossible_branches,
                              const std::set<GetElementPtrInst*>& gep_oob,
                              const std::set<Instruction*>& overflow_insts,
                              const std::set<Instruction*>& bad_shift_insts,
                              const std::set<Instruction*>& div_zero_insts) {
    if (CheckDeadBranch) {
        for (auto& cmp_istbr_pair : impossible_branches) {
            auto cmp = cmp_istbr_pair.first;
            auto is_tbr = cmp_istbr_pair.second;
            if (is_tbr)
                mark_err<interr::DEAD_TRUE_BR>(cmp);
            else
                mark_err<interr::DEAD_FALSE_BR>(cmp);
        }
    }

    if (CheckArrayOOB) {
        for (auto gep : gep_oob) {
            mark_err<interr::ARRAY_OOB>(gep);
        }
    }

    if (CheckIntOverflow) {
        for (auto inst : overflow_insts) {
            mark_err<interr::INT_OVERFLOW>(inst);
        }
    }

    if (CheckBadShift) {
        for (auto inst : bad_shift_insts) {
            mark_err<interr::BAD_SHIFT>(inst);
        }
    }

    if (CheckDivByZero) {
        for (auto inst : div_zero_insts) {
            mark_err<interr::DIV_BY_ZERO>(inst);
        }
    }
}

void BugDetection::generateSarifReport(const std::string& filename,
                                      const std::map<ICmpInst*, bool>& impossible_branches,
                                      const std::set<GetElementPtrInst*>& gep_oob,
                                      const std::set<Instruction*>& overflow_insts,
                                      const std::set<Instruction*>& bad_shift_insts,
                                      const std::set<Instruction*>& div_zero_insts) {
    sarif::SarifLog sarifLog("Kint", "1.0.0");

    // Define rules for each bug type
    sarifLog.addRule(sarif::Rule("INT_OVERFLOW", "Integer Overflow",
                                 "Integer arithmetic operation may overflow"));
    sarifLog.addRule(sarif::Rule("DIV_BY_ZERO", "Division by Zero",
                                 "Division or modulo operation may have zero divisor"));
    sarifLog.addRule(sarif::Rule("BAD_SHIFT", "Bad Shift",
                                 "Shift operation may have shift amount >= bit width"));
    sarifLog.addRule(sarif::Rule("ARRAY_OOB", "Array Out of Bounds",
                                 "Array index may be out of bounds"));
    sarifLog.addRule(sarif::Rule("DEAD_TRUE_BR", "Impossible True Branch",
                                 "Branch condition can never be true"));
    sarifLog.addRule(sarif::Rule("DEAD_FALSE_BR", "Impossible False Branch",
                                 "Branch condition can never be false"));

    // Helper lambda to add a result for an instruction
    auto addBugResult = [&sarifLog, this](const Instruction* inst, interr bugType) {
        if (!inst) return;
        
        sarif::Result result(
            [bugType]() {
                switch (bugType) {
                    case interr::NONE: return "NONE";
                    case interr::INT_OVERFLOW: return "INT_OVERFLOW";
                    case interr::DIV_BY_ZERO: return "DIV_BY_ZERO";
                    case interr::BAD_SHIFT: return "BAD_SHIFT";
                    case interr::ARRAY_OOB: return "ARRAY_OOB";
                    case interr::DEAD_TRUE_BR: return "DEAD_TRUE_BR";
                    case interr::DEAD_FALSE_BR: return "DEAD_FALSE_BR";
                    default: return "UNKNOWN";
                }
            }(),
            mkstr(bugType)
        );
        
        result.level = sarif::Level::Error;
        
        // Get location from debug info
        sarif::Location loc = sarif::utils::createLocationFromInstruction(inst);
        if (!loc.file.empty() && loc.line > 0) {
            // Add instruction text as snippet
            std::string instStr;
            llvm::raw_string_ostream instOS(instStr);
            instOS << *inst;
            loc.snippet = instOS.str();
            
            result.locations.push_back(loc);
        }
        
        // Add execution path as code flow if available
        auto pathIt = m_bug_paths.find(inst);
        if (pathIt != m_bug_paths.end() && !pathIt->second.path.empty()) {
            sarif::CodeFlow codeFlow;
            codeFlow.message = "Execution path leading to " + std::string(mkstr(bugType));
            
            int order = 1;
            for (const auto& pathPoint : pathIt->second.path) {
                // Create location for this path point
                sarif::Location pathLoc;
                
                // Try to get location from the instruction if available
                const Instruction* pathInst = pathPoint.inst ? pathPoint.inst : 
                                              (pathPoint.bb ? &pathPoint.bb->front() : nullptr);
                
                if (pathInst && pathInst->getDebugLoc()) {
                    pathLoc = sarif::utils::createLocationFromInstruction(pathInst);
                    
                    // Add instruction as snippet if we have a specific instruction
                    if (pathPoint.inst) {
                        std::string pathInstStr;
                        llvm::raw_string_ostream pathInstOS(pathInstStr);
                        pathInstOS << *pathPoint.inst;
                        pathLoc.snippet = pathInstOS.str();
                    }
                }
                
                // Create thread flow location
                std::string message = pathPoint.description;
                if (message.empty() && pathPoint.bb) {
                    message = "Execution reaches basic block in " + 
                             (pathPoint.bb->getParent() ? pathPoint.bb->getParent()->getName().str() : "unknown");
                }
                
                sarif::ThreadFlowLocation tfl(pathLoc, message, order++);
                codeFlow.threadFlowLocations.push_back(tfl);
            }
            
            // Add the bug location as the final step in the path
            sarif::ThreadFlowLocation bugTfl(loc, "Bug detected: " + std::string(mkstr(bugType)), order);
            codeFlow.threadFlowLocations.push_back(bugTfl);
            
            result.codeFlows.push_back(codeFlow);
        }
        
        sarifLog.addResult(result);
    };

    // Add results for integer overflow bugs
    if (CheckIntOverflow) {
        for (auto inst : overflow_insts) {
            addBugResult(inst, interr::INT_OVERFLOW);
        }
    }

    // Add results for division by zero bugs
    if (CheckDivByZero) {
        for (auto inst : div_zero_insts) {
            addBugResult(inst, interr::DIV_BY_ZERO);
        }
    }

    // Add results for bad shift bugs
    if (CheckBadShift) {
        for (auto inst : bad_shift_insts) {
            addBugResult(inst, interr::BAD_SHIFT);
        }
    }

    // Add results for array out of bounds bugs
    if (CheckArrayOOB) {
        for (auto inst : gep_oob) {
            addBugResult(inst, interr::ARRAY_OOB);
        }
    }

    // Add results for dead branch bugs
    if (CheckDeadBranch) {
        for (const auto& pair : impossible_branches) {
            addBugResult(pair.first, pair.second ? interr::DEAD_TRUE_BR : interr::DEAD_FALSE_BR);
        }
    }

    // Write SARIF output to file
    sarifLog.writeToFile(filename, true);
    
    MKINT_LOG() << "SARIF report written to: " << filename;
}

} // namespace kint
