#include "Apps/Checker/kint/KINTTaintAnalysis.h"
#include "Apps/Checker/kint/Utils.h"
#include "Apps/Checker/kint/Log.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/InstIterator.h>

#include <cxxabi.h>
#include <algorithm>

using namespace llvm;

namespace kint {

constexpr const char* MKINT_IR_TAINT = "mkint.taint";
constexpr const char* MKINT_IR_SINK = "mkint.sink";
constexpr const char* MKINT_TAINT_SRC_SUFFX = ".mkint.arg";

constexpr auto MKINT_SINKS = std::array<std::pair<const char*, size_t>, 19> {{
    std::pair<const char*, size_t>("malloc", 0),
    std::pair<const char*, size_t>("__mkint_sink0", 0), 
    std::pair<const char*, size_t>("__mkint_sink1", 1), 
    std::pair<const char*, size_t>("__mkint_sink2", 2),
    std::pair<const char*, size_t>("xmalloc", 0), 
    std::pair<const char*, size_t>("kmalloc", 0), 
    std::pair<const char*, size_t>("kzalloc", 0), 
    std::pair<const char*, size_t>("vmalloc", 0),
    std::pair<const char*, size_t>("mem_alloc", 0), 
    std::pair<const char*, size_t>("__page_empty", 0), 
    std::pair<const char*, size_t>("agp_alloc_page_array", 0),
    std::pair<const char*, size_t>("copy_from_user", 2), 
    std::pair<const char*, size_t>("__writel", 0), 
    std::pair<const char*, size_t>("access_ok", 2),
    std::pair<const char*, size_t>("btrfs_lookup_first_ordered_extent", 1), 
    std::pair<const char*, size_t>("sys_cfg80211_find_ie", 2),
    std::pair<const char*, size_t>("gdth_ioctl_alloc", 1), 
    std::pair<const char*, size_t>("sock_alloc_send_skb", 1), 
    std::pair<const char*, size_t>("memcpy", 2)
}};

std::string TaintAnalysis::demangle(const char* name) {
    int status = -1;
    std::unique_ptr<char, void (*)(void*)> res { abi::__cxa_demangle(name, NULL, NULL, &status), std::free };
    return (status == 0) ? res.get() : std::string(name);
}

void TaintAnalysis::mark_taint(Instruction& inst, const std::string& taint_name) {
    auto& ctx = inst.getContext();
    auto md = MDNode::get(ctx, MDString::get(ctx, taint_name));
    inst.setMetadata(MKINT_IR_TAINT, md);
}

bool TaintAnalysis::is_taint_src(StringRef sv) {
    const auto demangled_name = demangle(sv.str().c_str());
    const auto name = StringRef(demangled_name);
    return name.startswith("sys_") || name.startswith("__mkint_ann_");
}

std::vector<CallInst*> TaintAnalysis::get_taint_source(Function& F) {
    std::vector<CallInst*> ret;
    // judge if this function is the taint source.
    const auto name = F.getName();
    if (is_taint_src(name) && !F.isDeclaration()) {
        // mark all this function as a taint source.
        // Unfortunately arguments cannot be marked with metadata...
        // We need to rewrite the arguments -> unary callers and mark the callers.
        for (auto& arg : F.args()) {
            auto call_name = name.str() + MKINT_TAINT_SRC_SUFFX + std::to_string(arg.getArgNo());
            MKINT_LOG() << "Taint Analysis -> taint src arg -> call inst: " << call_name;
            FunctionType *FT = FunctionType::get(arg.getType(), /*isVarArg=*/false);
            auto Callee = F.getParent()->getOrInsertFunction(call_name, FT);
            auto call_inst = CallInst::Create(Callee, arg.getName(), &*F.getEntryBlock().getFirstInsertionPt());
            ret.push_back(call_inst);
            arg.replaceAllUsesWith(call_inst);
        }
    }
    return ret;
}

bool TaintAnalysis::is_taint_src_arg_call(StringRef s) { 
    return s.contains(MKINT_TAINT_SRC_SUFFX); 
}

SmallVector<Function*, 2> TaintAnalysis::get_sink_fns(Instruction* inst) {
    SmallVector<Function*, 2> ret;
    for (auto user : inst->users()) {
        if (auto call = dyn_cast<CallInst>(user)) {
            auto dname = demangle(call->getCalledFunction()->getName().data());
            if (std::find_if(
                    MKINT_SINKS.begin(), MKINT_SINKS.end(), [&dname](const auto& s) { return dname == s.first; })
                != MKINT_SINKS.end()) {
                ret.push_back(call->getCalledFunction());
            }
        }
    }
    return ret;
}

bool TaintAnalysis::is_sink_reachable(Instruction* inst, SetVector<Function*>& taint_funcs) {
    // we want to only mark sink-reachable taints; and
    // find out if the return value is tainted.
    if (nullptr == inst) {
        return false;
    } else if (inst->getMetadata(MKINT_IR_SINK)) {
        for (auto f : get_sink_fns(inst)) {
            taint_funcs.insert(f); // sink are tainted.
        }
        return true;
    }

    bool you_see_sink /* ? */ = false;

    // if store
    if (auto store = dyn_cast<StoreInst>(inst)) {
        auto ptr = store->getPointerOperand();
        if (auto gv = dyn_cast<GlobalVariable>(ptr)) {
            for (auto user : gv->users()) {
                if (auto user_inst = dyn_cast<Instruction>(user)) {
                    if (user != store) // no self-loop.
                        you_see_sink |= is_sink_reachable(user_inst, taint_funcs);
                }
            }

            if (you_see_sink) {
                mark_taint(*inst);
                gv->setMetadata(MKINT_IR_TAINT, inst->getMetadata(MKINT_IR_TAINT));
                return true;
            }
        }
    } else {
        if (auto call = dyn_cast<CallInst>(inst)) {
            if (auto f = call->getCalledFunction()) {
                // How to do taint analysis for call func?
                // if func's impl is unknow we simply assume it is related.
                // if func's impl is known, we analyze which arg determines the return value.
                // if unrelated -> cut off the connection.
                // FIXME: But we simply assume it is related and people won't wrote stupid code that results are not
                // related to inputs.
                if (!f->isDeclaration() && taint_bcast_sink(f->args(), taint_funcs)) {
                    you_see_sink = true;
                    taint_funcs.insert(f);
                }
            }
        }

        for (auto user : inst->users()) {
            if (auto user_inst = dyn_cast<Instruction>(user)) {
                // if used by phi whose id is even smaller than you. -> loop
                if (auto phi = dyn_cast<PHINode>(user_inst)) {
                    if (get_id(phi) < get_id(inst)) {
                        continue;
                    }
                }
                you_see_sink |= is_sink_reachable(user_inst, taint_funcs);
            }
        }

        if (you_see_sink) {
            mark_taint(*inst);
            if (auto call = dyn_cast<CallInst>(inst)) {
                if (auto f = call->getCalledFunction()) {
                    if (!f->getReturnType()->isVoidTy()) {
                        taint_funcs.insert(f);
                    }
                }
            }
            return true;
        }
    }

    return false;
}

bool TaintAnalysis::taint_bcast_sink(Function* f, const std::vector<CallInst*>& taint_source, SetVector<Function*>& taint_funcs) {
    // ? Note we currently assume that sub-func-calls do not have sinks...
    // ? otherwise we need a use-def tree to do the job (but too complicated).
    // Propogation: This pass should only consider single-function-level tainting.
    //              In `out = call(..., taint, ...)`, `out` is tainted. But let's
    //              refine that in cross-function-level tainting.

    // Algo: should do depth-first search until we find a sink. If we find a sink,
    //       we backtrack and mark taints.

    bool ret = false;

    // try to broadcast all allocas.
    for (auto& bb : *f) {
        for (auto& inst : bb) {
            if (auto alloc = dyn_cast<AllocaInst>(&inst)) {
                if (is_sink_reachable(alloc, taint_funcs)) {
                    mark_taint(*alloc, "source");
                    ret = true;
                }
            }
        }
    }

    for (auto ts : taint_source) {
        if (is_sink_reachable(ts, taint_funcs)) {
            mark_taint(*ts, "source");
            ret = true;
        }
    }

    return ret;
}

template <typename Iter> 
bool TaintAnalysis::taint_bcast_sink(Iter taint_source, SetVector<Function*>& taint_funcs) {
    bool ret = false;

    for (auto& ts : taint_source) {
        for (auto user : ts.users()) {
            if (auto user_inst = dyn_cast<Instruction>(user)) {
                if (is_sink_reachable(user_inst, taint_funcs)) {
                    mark_taint(*user_inst);
                    ret = true;
                }
            }
        }
    }

    return ret;
}

void TaintAnalysis::mark_func_sinks(Function& F, SetVector<StringRef>& callback_tsrc_fn) {
    static auto mark_sink = [](Instruction& inst, const std::string& sink_name) {
        auto& ctx = inst.getContext();
        auto md = MDNode::get(ctx, MDString::get(ctx, sink_name));
        inst.setMetadata(MKINT_IR_SINK, md);
    };

    for (auto& inst : instructions(F)) {
        if (auto* call = dyn_cast<CallInst>(&inst)) {
            // call in MKINT_SINKS
            for (const auto& sink_pair : MKINT_SINKS) {
                const char* name = sink_pair.first;
                size_t idx = sink_pair.second;
                if (auto called_fn = call->getCalledFunction()) {
                    const auto demangled_func_name = demangle(called_fn->getName().str().c_str());
                    if (demangled_func_name == name) {
                        if (auto arg = dyn_cast_or_null<Instruction>(call->getArgOperand(idx))) {
                            MKINT_LOG()
                                << "Taint Analysis -> sink: argument [" << idx << "] of " << demangled_func_name;
                            mark_sink(*arg, name);
                        }
                        break;
                    } else if (StringRef(demangled_func_name).startswith(name)) {
                        MKINT_WARN() << "Are you missing the sink? [demangled_func_name]: " << demangled_func_name
                                     << "; [name]: " << name;
                    }
                }
            }
        }
    }

    // if this function is taint source and has a return value used non-taint-source functions, we mark its return
    // statement as sink. this is because its return value can be used by, say kernel functions.
    if (is_taint_src(F.getName()) && F.getReturnType()->isIntegerTy() && !F.use_empty()) {

        // if there is any users.
        bool valid_use = false;
        for (auto user : F.users()) {
            if (auto user_inst = dyn_cast<Instruction>(user)) {
                if (!is_taint_src(user_inst->getParent()->getParent()->getName())) {
                    valid_use = true;
                    break;
                }
            }
        }
        if (!valid_use)
            return;

        for (auto& inst : instructions(F)) {
            if (dyn_cast<ReturnInst>(&inst)) {
                MKINT_LOG() << "Taint Analysis -> sink: return inst of " << F.getName();
                mark_sink(inst, "return");
                callback_tsrc_fn.insert(F.getName());
            }
        }
    }
}

void TaintAnalysis::propagate_taint_across_functions(Module& /*M*/,
                                                    MapVector<Function*, std::vector<CallInst*>>& func2tsrc,
                                                    SetVector<Function*>& taint_funcs) {
    for (auto& func_tsrc_pair : func2tsrc) {
        auto fp = func_tsrc_pair.first;
        auto& tsrc = func_tsrc_pair.second;
        if (taint_bcast_sink(fp, tsrc, taint_funcs)) {
            taint_funcs.insert(fp);
        }
    }

    size_t n_tfunc_before = 0;
    do {
        n_tfunc_before = taint_funcs.size();
        for (auto f : taint_funcs) {
            if (!is_taint_src(f->getName())) {
                taint_bcast_sink(f->args(), taint_funcs);
            }
        }
    } while (n_tfunc_before != taint_funcs.size());
}

} // namespace kint
