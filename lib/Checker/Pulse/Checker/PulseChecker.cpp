#include "Checker/Pulse/Checker/PulseChecker.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Domain/PulseInvalidation.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Interproc/PulseModels.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Pulse/Report/PulseReport.h"

#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

namespace pulse {

namespace {

static std::string llvmTypeToString(const llvm::Type *ty) {
  if (!ty)
    return "unknown";
  std::string str;
  llvm::raw_string_ostream os(str);
  ty->print(os);
  return os.str();
}

static bool isCheapToCopyStructName(llvm::StringRef name) {
  return name.contains("string_view") || name.contains("basic_string_view") ||
         name.contains("span") || name.contains("iterator") ||
         name.contains("unique_ptr") || name.contains("weak_ptr") ||
         name.contains("reference_wrapper") || name.contains("function_ref");
}

static bool isNonTrivialConstRefCandidate(const llvm::Argument &arg) {
  auto isCheapTy = [](const llvm::Type *ty) {
    if (const auto *ST = llvm::dyn_cast_or_null<llvm::StructType>(ty)) {
      if (ST->hasName() && isCheapToCopyStructName(ST->getName())) {
        return true;
      }
    }
    return false;
  };
  if (arg.hasByValAttr()) {
    if (const auto *pty = llvm::dyn_cast<llvm::PointerType>(arg.getType())) {
      if (isCheapTy(pty->getPointerElementType()))
        return false;
    }
    return true;
  }
  const llvm::Type *ty = arg.getType();
  if (ty->isStructTy() || ty->isArrayTy()) {
    return !isCheapTy(ty);
  }
  if (const auto *pty = llvm::dyn_cast<llvm::PointerType>(ty)) {
    if (const llvm::Type *elem = pty->getPointerElementType()) {
      if (elem->isStructTy() && !arg.onlyReadsMemory()) {
        return arg.hasByValAttr() && !isCheapTy(elem);
      }
    }
  }
  return false;
}

static size_t computeDisjunctPathStamp(const AbductiveDomain &astate) {
  size_t h = 0;
  auto mix = [&h](size_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  };
  mix(astate.getPostHeap().getEdges().size());
  for (const auto &kv : astate.getPostHeap().getEdges()) {
    mix(kv.second.size());
    for (const auto &e : kv.second) {
      mix(static_cast<size_t>(e.first.kind));
      mix(e.first.field_idx);
    }
  }
  mix(astate.getPostAttrs().getAttrs().size());
  for (const auto &kv : astate.getPostAttrs().getAttrs()) {
    for (Attribute a : kv.second) {
      mix(static_cast<size_t>(a));
    }
  }
  mix(astate.getPathFormula().getConditions().size());
  for (const auto &kv : astate.getPathFormula().getConditions()) {
    mix(static_cast<size_t>(kv.first.kind));
    mix(kv.second);
  }
  mix(astate.getPostStack().getMap().size());
  mix(astate.getPostWrittenTo().size());
  mix(astate.getPostAllocationInfo().size());
  return h;
}

} // namespace

constexpr unsigned PulseChecker::kMaxDisjuncts;
constexpr unsigned PulseChecker::kMaxCallDepth;

PulseChecker::PulseChecker(llvm::Module *M, lotus::AliasAnalysisWrapper *AA)
    : module_(M), aa_(AA), ops_(&factory_),
      models_(std::make_unique<PulseModels>(*this)) {
  registerBugTypes();
  if (aa_ && aa_->isInitialized()) {
    factory_.setMustAliasFn(
        [this](const llvm::Value *v1, const llvm::Value *v2) {
          return aa_->mustAlias(v1, v2);
        });
  }
}

PulseChecker::~PulseChecker() { DiagnosticManager::getInstance().flush(); }

const PulseSummary *PulseChecker::resolveSummaryForCall(
    const llvm::Function *callee, const ExecutionDomain &caller_state,
    const llvm::CallBase *CI, const llvm::BasicBlock *pred) {
  const PulseSummary *base_summary = summary_manager_.getSummary(callee);
  if (!base_summary || !base_summary->isValid()) {
    return base_summary;
  }

  const AbductiveDomain *caller_astate = caller_state.getAstate();
  if (!caller_astate) {
    return base_summary;
  }

  const auto *call_inst = llvm::dyn_cast<llvm::CallInst>(CI);
  if (!call_inst) {
    return base_summary;
  }

  if (caller_astate->getDynamicTypes().empty() && CI->arg_size() < 2) {
    return base_summary;
  }

  std::vector<AbstractValue> actual_args;
  actual_args.reserve(CI->arg_size());
  AbductiveDomain eval_state = caller_astate->clone();
  for (unsigned i = 0; i < CI->arg_size(); ++i) {
    auto arg_opt = ops_.eval(eval_state, CI->getArgOperand(i), CI, pred);
    if (!arg_opt) {
      actual_args.push_back(AbstractValue());
      continue;
    }
    actual_args.push_back(eval_state.getCanonical(arg_opt->addr));
  }

  SpecializationKey key = SpecializationManager::computeSpecializationKey(
      *caller_astate, call_inst, actual_args);
  if (key.isBottom()) {
    return base_summary;
  }

  if (const PulseSummary *specialized =
          analyzeFunctionSpecialized(callee, key)) {
    return specialized;
  }

  return base_summary;
}

void PulseChecker::materializeSpecializedSummary(
    const llvm::Function *callee, const PulseSummary &base_summary,
    const SpecializationKey &key) {
  (void)base_summary;
  (void)analyzeFunctionSpecialized(callee, key);
}

const PulseSummary *
PulseChecker::analyzeFunctionSpecialized(const llvm::Function *F,
                                         const SpecializationKey &raw_key) {
  if (!F || F->isDeclaration()) {
    return nullptr;
  }
  SpecializationKey key = raw_key;
  key.normalize();
  if (key.isBottom()) {
    return summary_manager_.getSummary(F);
  }
  if (const PulseSummary *cached =
          specialization_manager_.getSpecializedSummary(F, key)) {
    return cached;
  }

  auto guard_key = std::make_pair(F, key);
  if (active_specializations_.count(guard_key) > 0) {
    return nullptr;
  }
  active_specializations_.insert(guard_key);

  ExecutionDomain base_init = initializeFunction(F);
  const AbductiveDomain *init_astate = base_init.getAstate();
  if (!init_astate) {
    active_specializations_.erase(guard_key);
    return nullptr;
  }

  auto spec_entry_opt = SpecializationManager::applySpecializationToEntry(
      *init_astate, F, key, factory_);
  if (!spec_entry_opt) {
    active_specializations_.erase(guard_key);
    return nullptr;
  }

  ExecutionDomain spec_init = ExecutionDomain::continueProgram(
      std::make_unique<AbductiveDomain>(std::move(*spec_entry_opt)));
  analyzeFunctionInternal(F, std::move(spec_init), &key);

  active_specializations_.erase(guard_key);
  return specialization_manager_.getSpecializedSummary(F, key);
}

void PulseChecker::registerBugTypes() {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  auto &diagMgr = DiagnosticManager::getInstance();

  useAfterFreeTypeId_ =
      mgr.register_bug_type(IssueType::UseAfterFree, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-416");
  diagMgr.registerBugType(IssueType::UseAfterFree, useAfterFreeTypeId_);

  nullDerefTypeId_ =
      mgr.register_bug_type(IssueType::NullDereference, BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "CWE-476");
  diagMgr.registerBugType(IssueType::NullDereference, nullDerefTypeId_);

  uninitializedReadTypeId_ = mgr.register_bug_type(
      IssueType::UninitializedRead, BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "CWE-457");
  diagMgr.registerBugType(IssueType::UninitializedRead,
                          uninitializedReadTypeId_);

  unnecessaryCopyTypeId_ =
      mgr.register_bug_type(IssueType::UnnecessaryCopy, BugDescription::BI_LOW,
                            BugDescription::BC_PERFORMANCE, "");
  diagMgr.registerBugType(IssueType::UnnecessaryCopy, unnecessaryCopyTypeId_);

  constRefableParamTypeId_ = mgr.register_bug_type(
      IssueType::ConstRefableParam, BugDescription::BI_LOW,
      BugDescription::BC_PERFORMANCE, "");
  diagMgr.registerBugType(IssueType::ConstRefableParam,
                          constRefableParamTypeId_);

  taintErrorTypeId_ =
      mgr.register_bug_type(IssueType::TaintError, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-20");
  diagMgr.registerBugType(IssueType::TaintError, taintErrorTypeId_);

  stackAddressEscapeTypeId_ = mgr.register_bug_type(
      IssueType::StackVariableAddressEscape, BugDescription::BI_HIGH,
      BugDescription::BC_SECURITY, "CWE-562");
  diagMgr.registerBugType(IssueType::StackVariableAddressEscape,
                          stackAddressEscapeTypeId_);

  invalidFreeTypeId_ =
      mgr.register_bug_type(IssueType::InvalidFree, BugDescription::BI_HIGH,
                            BugDescription::BC_SECURITY, "CWE-590");
  diagMgr.registerBugType(IssueType::InvalidFree, invalidFreeTypeId_);

  outOfBoundsTypeId_ =
      mgr.register_bug_type(IssueType::OutOfBounds, BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "CWE-119");
  diagMgr.registerBugType(IssueType::OutOfBounds, outOfBoundsTypeId_);

  memoryLeakTypeId_ =
      mgr.register_bug_type(IssueType::MemoryLeak, BugDescription::BI_MEDIUM,
                            BugDescription::BC_PERFORMANCE, "CWE-401");
  diagMgr.registerBugType(IssueType::MemoryLeak, memoryLeakTypeId_);

  resourceLeakTypeId_ =
      mgr.register_bug_type(IssueType::ResourceLeak, BugDescription::BI_HIGH,
                            BugDescription::BC_ERROR, "CWE-775");
  diagMgr.registerBugType(IssueType::ResourceLeak, resourceLeakTypeId_);

  mutualRecursionTypeId_ = mgr.register_bug_type(
      IssueType::MutualRecursionCycle, BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "CWE-674");
  diagMgr.registerBugType(IssueType::MutualRecursionCycle,
                          mutualRecursionTypeId_);

  retainCycleTypeId_ = mgr.register_bug_type(
      IssueType::RetainCycle, BugDescription::BI_MEDIUM,
      BugDescription::BC_PERFORMANCE, "CWE-401");
  diagMgr.registerBugType(IssueType::RetainCycle, retainCycleTypeId_);

  infiniteLoopTypeId_ = mgr.register_bug_type(
      IssueType::InfiniteLoop, BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "CWE-835");
  diagMgr.registerBugType(IssueType::InfiniteLoop, infiniteLoopTypeId_);

  transitiveAccessTypeId_ = mgr.register_bug_type(
      IssueType::TransitiveAccess, BugDescription::BI_MEDIUM,
      BugDescription::BC_ERROR, "");
  diagMgr.registerBugType(IssueType::TransitiveAccess, transitiveAccessTypeId_);
}

void PulseChecker::extractTypeHierarchyFromModule() {
  if (!module_) {
    return;
  }
  // Parse Itanium C++ ABI RTTI globals (`@_ZTI*` using __si_class_type_info /
  // __vmi_class_type_info) to populate PulseFormula's subtype hierarchy so
  // `Derived <: Base` works for InstanceOf and catch(Base&) matching.
  for (const llvm::GlobalVariable &GV : module_->globals()) {
    if (!GV.hasName() || !GV.getName().startswith("_ZTI") ||
        !GV.hasInitializer()) {
      continue;
    }
    const auto *CS = llvm::dyn_cast<llvm::ConstantStruct>(GV.getInitializer());
    if (!CS || CS->getNumOperands() < 3) {
      continue;
    }
    std::string derived_name = detail::normalizeRttiTypeName(&GV);
    if (derived_name.empty()) {
      continue;
    }
    // Check element 0 for __si_class_type_info (single inheritance: op 2 is
    // base _ZTI) or __vmi_class_type_info (multiple inheritance: ops >= 4).
    const llvm::Value *meta = CS->getOperand(0)->stripPointerCasts();
    std::string meta_name;
    if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(meta)) {
      if (CE->getNumOperands() > 0) {
        if (const auto *MGV = llvm::dyn_cast<llvm::GlobalValue>(
                CE->getOperand(0)->stripPointerCasts())) {
          meta_name = MGV->getName().str();
        }
      }
    } else if (const auto *MGV = llvm::dyn_cast<llvm::GlobalValue>(meta)) {
      meta_name = MGV->getName().str();
    }

    auto registerBaseFromOp = [&](const llvm::Constant *op) {
      std::string base_name = detail::normalizeRttiTypeName(op);
      if (!base_name.empty() && base_name != derived_name) {
        PulseFormula::registerSubtype(derived_name, base_name);
      }
    };

    if (meta_name.find("__si_class_type_info") != std::string::npos) {
      registerBaseFromOp(CS->getOperand(2));
    } else if (meta_name.find("__vmi_class_type_info") != std::string::npos) {
      for (unsigned i = 4; i < CS->getNumOperands(); ++i) {
        if (CS->getOperand(i)->getType()->isPointerTy()) {
          registerBaseFromOp(CS->getOperand(i));
        }
      }
    } else if (CS->getNumOperands() == 3 &&
               CS->getOperand(2)->getType()->isPointerTy()) {
      // Fallback for hand-written single-inheritance RTTI structs in IR.
      registerBaseFromOp(CS->getOperand(2));
    }
  }
}

void PulseChecker::checkTransitiveAccessPolicies(const llvm::Function *F,
                                                 const TransitiveInfo &info) {
  if (!F || transitive_access_policies_.empty()) {
    return;
  }
  const std::string fname = F->getName().str();
  auto matchesPatternList = [](const std::string &name,
                               const std::vector<std::string> &patterns) {
    for (const auto &pat : patterns) {
      if (pat == name)
        return true;
      try {
        if (std::regex_search(name, std::regex(pat)))
          return true;
      } catch (const std::regex_error &) {
        if (name.find(pat) != std::string::npos)
          return true;
      }
    }
    return false;
  };

  for (const auto &policy : transitive_access_policies_) {
    if (!matchesPatternList(fname, policy.source_functions)) {
      continue;
    }
    for (const auto &call_info : info.getCallResolutions()) {
      for (const llvm::Function *target : call_info.targets) {
        if (!target)
          continue;
        std::string tname = target->getName().str();
        if (!matchesPatternList(tname, policy.forbidden_callees)) {
          continue;
        }
        const llvm::Instruction *loc =
            call_info.call_site
                ? call_info.call_site
                : (F->empty() ? nullptr : &F->getEntryBlock().front());
        if (!loc)
          continue;
        Trace trace;
        if (!F->empty()) {
          trace.addEvent(&F->getEntryBlock().front(),
                         "Restricted entry function: " + fname);
        }
        if (call_info.call_site) {
          trace.addEvent(call_info.call_site,
                         "Transitively reaches forbidden callee: " + tname);
        }
        std::string detail = policy.description + " (" + fname +
                             " transitively calls " + tname + ")";
        auto diag = std::make_unique<TransitiveAccess>(loc, policy.name, detail,
                                                       std::move(trace));
        DiagnosticManager::getInstance().report(std::move(diag));
      }
    }
  }
}

void PulseChecker::analyze() {
  PulseLogger::info("Starting module analysis");
  PulseLogger::incrementCounter("modules.analyzed");
  extractTypeHierarchyFromModule();

  std::vector<const llvm::Function *> functions;
  functions.reserve(module_->size());
  for (auto &F : *module_) {
    if (!F.isDeclaration())
      functions.push_back(&F);
  }

  std::unordered_set<const llvm::Function *> function_set(functions.begin(),
                                                          functions.end());

  // Build direct call graph from both CallInst and InvokeInst (CallBase).
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      edges;
  edges.reserve(functions.size());
  for (const llvm::Function *F : functions) {
    std::vector<const llvm::Function *> callees;
    for (const auto &BB : *F) {
      for (const auto &I : BB) {
        const auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!CB)
          continue;
        const llvm::Function *Callee = CB->getCalledFunction();
        if (!Callee || Callee->isDeclaration())
          continue;
        if (function_set.count(Callee) == 0)
          continue;
        callees.push_back(Callee);
      }
    }
    edges.emplace(F, std::move(callees));
  }

  // Reverse edges for Kosaraju SCC computation.
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      rev_edges;
  rev_edges.reserve(functions.size());
  for (const llvm::Function *F : functions) {
    (void)rev_edges[F];
  }
  for (const auto &kv : edges) {
    const llvm::Function *caller = kv.first;
    for (const llvm::Function *callee : kv.second) {
      rev_edges[callee].push_back(caller);
    }
  }

  std::vector<const llvm::Function *> order;
  order.reserve(functions.size());
  std::unordered_set<const llvm::Function *> visited;
  visited.reserve(functions.size());

  std::function<void(const llvm::Function *)> dfs1 =
      [&](const llvm::Function *F) {
        if (visited.count(F))
          return;
        visited.insert(F);
        auto it = edges.find(F);
        if (it != edges.end()) {
          for (const llvm::Function *callee : it->second) {
            dfs1(callee);
          }
        }
        order.push_back(F);
      };

  for (const llvm::Function *F : functions)
    dfs1(F);

  std::unordered_set<const llvm::Function *> visited2;
  visited2.reserve(functions.size());
  std::vector<std::vector<const llvm::Function *>> sccs;
  sccs.reserve(functions.size());

  std::function<void(const llvm::Function *,
                     std::vector<const llvm::Function *> &)>
      dfs2 = [&](const llvm::Function *F,
                 std::vector<const llvm::Function *> &scc) {
        if (visited2.count(F))
          return;
        visited2.insert(F);
        scc.push_back(F);
        auto it = rev_edges.find(F);
        if (it != rev_edges.end()) {
          for (const llvm::Function *pred : it->second) {
            dfs2(pred, scc);
          }
        }
      };

  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const llvm::Function *F = *it;
    if (visited2.count(F))
      continue;
    std::vector<const llvm::Function *> scc;
    dfs2(F, scc);
    sccs.push_back(std::move(scc));
  }

  std::unordered_map<const llvm::Function *, size_t> scc_id;
  scc_id.reserve(functions.size());
  for (size_t i = 0; i < sccs.size(); ++i) {
    for (const llvm::Function *F : sccs[i])
      scc_id[F] = i;
  }

  std::vector<std::unordered_set<size_t>> scc_edges(sccs.size());
  std::vector<size_t> indeg(sccs.size(), 0);
  for (const auto &kv : edges) {
    const llvm::Function *caller = kv.first;
    size_t c_id = scc_id[caller];
    for (const llvm::Function *callee : kv.second) {
      size_t d_id = scc_id[callee];
      if (c_id == d_id)
        continue;
      if (scc_edges[c_id].insert(d_id).second) {
        indeg[d_id]++;
      }
    }
  }

  std::queue<size_t> q;
  for (size_t i = 0; i < indeg.size(); ++i) {
    if (indeg[i] == 0)
      q.push(i);
  }
  std::vector<size_t> topo;
  topo.reserve(sccs.size());
  while (!q.empty()) {
    size_t id = q.front();
    q.pop();
    topo.push_back(id);
    for (size_t succ : scc_edges[id]) {
      if (--indeg[succ] == 0)
        q.push(succ);
    }
  }

  // Analyze SCCs bottom-up (callees before callers).
  // For cyclic SCCs (mutual or direct recursion), run 2 bottom-up passes:
  // Pass 1 computes base-case summaries while breaking cycles via current_scc_;
  // Pass 2 refines summaries by applying Pass 1's summaries at recursive calls.
  for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
    size_t id = *it;
    bool is_cyclic = sccs[id].size() > 1;
    if (!is_cyclic && sccs[id].size() == 1) {
      const llvm::Function *F = sccs[id][0];
      auto eit = edges.find(F);
      if (eit != edges.end()) {
        for (const llvm::Function *c : eit->second) {
          if (c == F) {
            is_cyclic = true;
            break;
          }
        }
      }
    }

    current_scc_.clear();
    for (const llvm::Function *F : sccs[id])
      current_scc_.insert(F);
    for (const llvm::Function *F : sccs[id]) {
      analyzeFunction(F);
    }

    if (is_cyclic && sccs[id].size() <= 4) {
      size_t total_scc_bbs = 0;
      for (const llvm::Function *F : sccs[id]) {
        total_scc_bbs += F->size();
      }
      if (total_scc_bbs > 256) {
        continue;
      }
      current_scc_.clear();
      for (const llvm::Function *F : sccs[id]) {
        active_functions_.insert(F);
      }
      for (const llvm::Function *F : sccs[id]) {
        if (summary_manager_.hasSummary(F)) {
          ExecutionDomain init_state = initializeFunction(F);
          analyzeFunctionInternal(F, std::move(init_state), nullptr);
        }
      }
      for (const llvm::Function *F : sccs[id]) {
        active_functions_.erase(F);
      }
    }
  }
  current_scc_.clear();

  PulseLogger::info("Completed analysis of " +
                    std::to_string(functions.size()) + " functions");

  DiagnosticManager::getInstance().flush();
}

void PulseChecker::analyzeFunction(const llvm::Function *F) {
  if (!F || F->isDeclaration()) {
    return;
  }
  if (summary_manager_.hasSummary(F)) {
    PulseLogger::logFunction(F, "skipped (already has summary)");
    return;
  }

  ExecutionDomain init_state = initializeFunction(F);
  analyzeFunctionInternal(F, std::move(init_state), nullptr);
}

void PulseChecker::analyzeFunctionInternal(const llvm::Function *F,
                                           ExecutionDomain init_state,
                                           const SpecializationKey *spec_key) {
  PulseLogger::logFunction(F, spec_key ? "starting specialized analysis"
                                       : "starting analysis");
  PulseLogger::startTimer("function." + F->getName().str());
  PulseLogger::incrementCounter("functions.analyzed");

  // Save and isolate non-disjunctive domain when running a nested specialized
  // or on-demand callee analysis so we never clobber the caller's state.
  const bool inserted_active = active_functions_.insert(F).second;
  const bool isolate_non_disj =
      (spec_key != nullptr) || (active_functions_.size() > 1 && inserted_active);
  NonDisjunctiveDomain saved_non_disj;
  if (isolate_non_disj) {
    saved_non_disj = std::move(analysis_non_disj_);
  }
  analysis_non_disj_.clear();

  // Register formal parameters (for interprocedural NonDisjunctiveSummary
  // mutation propagation) and candidate const-refable parameters at entry.
  if (!spec_key && init_state.getAstate()) {
    unsigned arg_idx = 0;
    for (const auto &Arg : F->args()) {
      AbstractValue av = factory_.getOrCreate(&Arg);
      analysis_non_disj_.registerFormalParam(arg_idx, av);
      if (isNonTrivialConstRefCandidate(Arg)) {
        analysis_non_disj_.registerConstRefCandidate(&Arg, av);
      }
      ++arg_idx;
    }
  }

  LoopAbstraction loop_abs_ref;
  {
    llvm::Function &Fn = *const_cast<llvm::Function *>(F);
    llvm::DominatorTree DT(Fn);
    llvm::LoopInfo LI;
    LI.analyze(DT);
    loop_abs_ref.initialize(LI);
  }

  DisjunctiveDomain disj_domain;

  // Compute Reverse-Post-Order numbering for RPO worklist scheduling
  // (Infer AbstractInterpreter.ml).
  std::unordered_map<const llvm::BasicBlock *, unsigned> rpo_order;
  {
    llvm::ReversePostOrderTraversal<const llvm::Function *> rpot(F);
    unsigned idx = 0;
    for (const llvm::BasicBlock *BB : rpot) {
      rpo_order[BB] = idx++;
    }
  }

  struct WorkItem {
    unsigned rpo;
    unsigned seq;
    const llvm::BasicBlock *bb;
    ExecutionDomain state;
    const llvm::BasicBlock *pred;
  };
  struct WorkItemCmp {
    bool operator()(const WorkItem &a, const WorkItem &b) const {
      if (a.rpo != b.rpo)
        return a.rpo > b.rpo; // Smaller RPO has higher priority.
      return a.seq > b.seq;
    }
  };
  std::priority_queue<WorkItem, std::vector<WorkItem>, WorkItemCmp> worklist;
  unsigned next_seq = 0;

  auto pushWork = [&](const llvm::BasicBlock *bb, ExecutionDomain st,
                      const llvm::BasicBlock *pred) {
    if (!bb || bb->empty())
      return;
    unsigned rpo = rpo_order.count(bb) ? rpo_order[bb] : 0u;
    worklist.push(WorkItem{rpo, next_seq++, bb, std::move(st), pred});
  };

  const llvm::BasicBlock *entry_block = &F->getEntryBlock();
  pushWork(entry_block, std::move(init_state), nullptr);

  std::vector<ExecutionDomain> exit_states;
  std::vector<ExecutionDomain> latent_exit_states;
  std::map<const llvm::BasicBlock *, unsigned> block_visits;
  unsigned iter_limit = 0;
  const unsigned max_iter = 4000;

  PulseLogger::incrementCounter("paths.explored");

  const unsigned max_disjuncts = options::maxDisjuncts();
  const unsigned max_worklist = std::max(max_disjuncts * 2, 16u);
  const unsigned keep_worklist = std::max(max_disjuncts, 8u);
  const unsigned max_block_visits = std::max(max_disjuncts * 2, 16u);

  while (!worklist.empty() && iter_limit++ < max_iter) {
    if (worklist.size() > max_worklist) {
      PulseLogger::debug("Pruning oversized worklist for function " +
                         F->getName().str());
      std::vector<WorkItem> kept;
      kept.reserve(keep_worklist);
      while (!worklist.empty() && kept.size() < keep_worklist) {
        kept.push_back(std::move(const_cast<WorkItem &>(worklist.top())));
        worklist.pop();
      }
      while (!worklist.empty()) {
        const auto &dropped = worklist.top();
        if (!spec_key && dropped.state.getAstate()) {
          analysis_non_disj_.addState(*dropped.state.getAstate());
        }
        worklist.pop();
      }
      for (auto &k : kept) {
        worklist.push(std::move(k));
      }
    }

    WorkItem item = std::move(const_cast<WorkItem &>(worklist.top()));
    worklist.pop();
    const llvm::BasicBlock *BB = item.bb;
    ExecutionDomain current_state = std::move(item.state);
    const llvm::BasicBlock *pred_bb = item.pred;

    if (current_state.isExceptionRaised()) {
      exit_states.push_back(std::move(current_state));
      continue;
    }

    if (current_state.isStopped()) {
      if (current_state.isExitProgram()) {
        exit_states.push_back(std::move(current_state));
      } else if (current_state.isLatentAbortProgram() ||
                 current_state.isLatentInvalidAccess()) {
        latent_exit_states.push_back(std::move(current_state));
      }
      continue;
    }

    current_state.setEntryPred(pred_bb);

    block_visits[BB]++;
    if (block_visits[BB] > max_block_visits)
      continue;

    // Per-disjunct loop convergence (Infer's LoopHeaderInfo) + LoopAbstraction
    // + sound InfiniteLoop (PulseEternal) detection on exact repeating back-edges.
    if (loop_abs_ref.isLoopHeader(BB)) {
      const bool from_backedge =
          pred_bb && loop_abs_ref.isInLoopBody(BB, pred_bb);
      if (from_backedge && loop_abs_ref.hasExactRepeatingState(BB, current_state)) {
        if (const AbductiveDomain *a = current_state.getAstate()) {
          auto is_allocated = [a](AbstractValue v) { return a->isAllocated(v); };
          if (!a->hasUnknownValues() &&
              a->getPathFormula().isManifest(is_allocated)) {
            const llvm::Instruction *loop_loc = BB->getTerminator()
                                                    ? BB->getTerminator()
                                                    : &BB->front();
            Trace inf_trace;
            inf_trace.addEvent(loop_loc,
                               "Loop header repeats identical concrete state "
                               "on back-edge with no progress");
            auto diag = std::make_unique<InfiniteLoop>(
                loop_loc,
                "Manifest loop repeats identical state without termination "
                "progress",
                std::move(inf_trace));
            DiagnosticManager::getInstance().report(std::move(diag));
          }
        }
      }
      if (loop_abs_ref.hasConverged(BB, current_state)) {
        if (!spec_key && current_state.getAstate()) {
          analysis_non_disj_.addState(*current_state.getAstate());
        }
        continue;
      }
      unsigned disj_visits = 0;
      if (AbductiveDomain *a = current_state.getAstate()) {
        if (from_backedge) {
          size_t stamp = computeDisjunctPathStamp(*a);
          if (a->hasLoopPathStamp(BB, stamp)) {
            if (!spec_key) {
              analysis_non_disj_.addState(*a);
            }
            continue;
          }
          a->recordLoopPathStamp(BB, stamp);
        }
        disj_visits = a->getDisjunctLoopVisits(BB);
      }

      bool should_widen = loop_abs_ref.visitHeader(BB, current_state);
      if (from_backedge) {
        loop_abs_ref.recordBackedgeState(BB, current_state);
      }
      // Wait for at least one un-widened back-edge iteration beyond entry
      // before widening, so exact-repeating infinite loops are observed
      // without premature widening clobbering PHI-initialized loop locals.
      if (should_widen && (!from_backedge || disj_visits > options::widenThreshold())) {
        if (loop_abs_ref.isInferringInvariant(BB)) {
          auto invariant_opt = loop_abs_ref.inferInvariant(
              BB, loop_abs_ref.getEntryState(BB), current_state);
          if (invariant_opt && invariant_opt->getEntryPred() == pred_bb) {
            current_state = std::move(*invariant_opt);
          }
        } else {
          ExecutionDomain widened = loop_abs_ref.widen(BB, current_state);
          if (widened.getEntryPred() == pred_bb) {
            current_state = std::move(widened);
          }
        }
        if (current_state.isStopped()) {
          continue;
        }
      }
    }

    // Check block-visit cap BEFORE cloning/deduplicating states that will
    // immediately be dropped.
    if (disj_domain.hasReachedBlockVisitCap(BB)) {
      if (!spec_key && current_state.getAstate()) {
        analysis_non_disj_.addState(*current_state.getAstate());
      }
      continue;
    }

    // Deduplicate isomorphic disjuncts first (cloning only when novel, so
    // duplicate states never inflate visit counts or trigger premature joins).
    if (!disj_domain.addWithDedupRef(BB, current_state, pred_bb,
                                     spec_key ? nullptr
                                              : &analysis_non_disj_)) {
      continue;
    }
    disj_domain.widen(BB);
    if (disj_domain.hasReachedBlockVisitCap(BB)) {
      if (!spec_key && current_state.getAstate()) {
        analysis_non_disj_.addState(*current_state.getAstate());
      }
      continue;
    }
    disj_domain.recordEdgeVisit(BB, pred_bb);
    if (disj_domain.shouldWidenEdge(BB, pred_bb)) {
      if (disj_domain.hasWidenedEdge(BB, pred_bb)) {
        if (!spec_key && current_state.getAstate()) {
          analysis_non_disj_.addState(*current_state.getAstate());
        }
        continue;
      }
      ExecutionDomain joined = disj_domain.joinAtBlockForPred(BB, pred_bb);
      disj_domain.markWidenedEdge(BB, pred_bb);
      if (!joined.isStopped()) {
        pred_bb = joined.getEntryPred();
        current_state = std::move(joined);
      } else {
        continue;
      }
    }

    std::vector<ExecutionDomain> states;
    states.push_back(std::move(current_state));

    auto limitStates = [&](std::vector<ExecutionDomain> &vec) {
      if (vec.size() <= max_disjuncts) {
        return;
      }
      std::vector<ExecutionDomain> preferred;
      std::vector<ExecutionDomain> rest;
      preferred.reserve(vec.size());
      rest.reserve(vec.size());
      for (auto &st : vec) {
        auto *a = st.getAstate();
        if (a && !a->hasUnknownValues()) {
          preferred.push_back(std::move(st));
        } else {
          rest.push_back(std::move(st));
        }
      }
      vec.clear();
      for (auto &st : preferred) {
        if (vec.size() < max_disjuncts) {
          vec.push_back(std::move(st));
        } else if (!spec_key && st.getAstate()) {
          analysis_non_disj_.addState(*st.getAstate());
        }
      }
      for (auto &st : rest) {
        if (vec.size() < max_disjuncts) {
          vec.push_back(std::move(st));
        } else if (!spec_key && st.getAstate()) {
          analysis_non_disj_.addState(*st.getAstate());
        }
      }
    };

    const llvm::Instruction *term = BB->getTerminator();
    for (const llvm::Instruction &I : *BB) {
      if (states.empty())
        break;

      std::vector<ExecutionDomain> next_states;
      next_states.reserve(states.size());

      const bool is_call_inst = llvm::isa<llvm::CallBase>(&I);
      unsigned clean_next_count = 0;
      for (auto &st : states) {
        if (st.isStopped() || st.isExceptionRaised()) {
          next_states.push_back(std::move(st));
          continue;
        }
        if (is_call_inst &&
            (clean_next_count >= max_disjuncts ||
             next_states.size() >= max_disjuncts * 2)) {
          if (!spec_key && st.getAstate()) {
            analysis_non_disj_.addState(*st.getAstate());
          }
          continue;
        }

        auto new_states =
            executeInstruction(&I, std::move(st), pred_bb, 0u);
        for (auto &ns : new_states) {
          // A non-terminator call that raises an exception unwinds out of the
          // current function (only an InvokeInst terminator catches locally).
          if (&I != term && ns.isExceptionRaised()) {
            exit_states.push_back(std::move(ns));
            continue;
          }
          if (is_call_inst && ns.getAstate() &&
              !ns.getAstate()->hasUnknownValues()) {
            ++clean_next_count;
          }
          next_states.push_back(std::move(ns));
        }
      }

      states = std::move(next_states);
      limitStates(states);
    }

    bool any_continuing = false;
    const bool term_is_invoke = term && llvm::isa<llvm::InvokeInst>(term);
    for (auto &st : states) {
      if (st.isStopped()) {
        if (st.isExitProgram()) {
          if (!spec_key && st.getAstate()) {
            analysis_non_disj_.addState(*st.getAstate());
          }
          exit_states.push_back(std::move(st));
        } else if (st.isLatentAbortProgram() || st.isLatentInvalidAccess()) {
          latent_exit_states.push_back(std::move(st));
        }
        continue;
      }
      if (st.isExceptionRaised() && !term_is_invoke) {
        exit_states.push_back(std::move(st));
        continue;
      }
      any_continuing = true;
    }

    if (!any_continuing) {
      continue;
    }

    if (!term)
      continue;

    if (const auto *II = llvm::dyn_cast<llvm::InvokeInst>(term)) {
      for (auto &st : states) {
        if (st.isStopped())
          continue;
        if (st.isExceptionRaised()) {
          const llvm::BasicBlock *unwind = II->getUnwindDest();
          if (unwind && !unwind->empty()) {
            // Typed LandingPad catch-clause matching: if the thrown exception's
            // dynamic RTTI type is known and the landing pad has typed clauses
            // (without cleanup/catch-all) that do NOT match, propagate the
            // uncaught exception out of the function instead of entering unwind!
            bool handled_by_lpad = true;
            if (!st.getExceptionType().empty()) {
              if (const auto *LPI = unwind->getLandingPadInst()) {
                if (!LPI->isCleanup() && LPI->getNumClauses() > 0) {
                  handled_by_lpad = false;
                  for (unsigned ci = 0; ci < LPI->getNumClauses(); ++ci) {
                    if (!LPI->isCatch(ci))
                      continue;
                    std::string clause_ty =
                        detail::normalizeRttiTypeName(LPI->getClause(ci));
                    if (clause_ty.empty() ||
                        PulseFormula::isSubtypeOf(st.getExceptionType(),
                                                  clause_ty)) {
                      handled_by_lpad = true;
                      break;
                    }
                  }
                }
              }
            }
            if (!handled_by_lpad) {
              exit_states.push_back(std::move(st));
              continue;
            }
            ExecutionDomain unw = ExecutionDomain::continueProgram(
                std::make_unique<AbductiveDomain>(st.getAstate()->clone()));
            unw.setEntryPred(BB);
            unw.setExceptionInfo(st.getExceptionValue(), st.getExceptionType());
            pushWork(unwind, std::move(unw), BB);
          } else {
            exit_states.push_back(std::move(st));
          }
        } else {
          const llvm::BasicBlock *normal = II->getNormalDest();
          if (normal && !normal->empty()) {
            st.setEntryPred(BB);
            pushWork(normal, std::move(st), BB);
          }
        }
      }
      continue;
    }

    auto *BI = llvm::dyn_cast<llvm::BranchInst>(term);
    if (BI && BI->isConditional() && BI->getNumSuccessors() == 2) {
      for (auto &st : states) {
        if (st.isStopped() || st.isExceptionRaised())
          continue;
        for (unsigned i = 0; i < 2; i++) {
          std::optional<ExecutionDomain> fork_opt = applyBranchCondition(
              i == 0 ? st.clone() : std::move(st), BI, i, pred_bb);
          if (!fork_opt)
            continue;
          if (fork_opt->isLatentInvalidAccess() ||
              fork_opt->isLatentAbortProgram()) {
            latent_exit_states.push_back(std::move(*fork_opt));
            continue;
          }
          if (fork_opt->isStopped()) {
            continue;
          }
          const llvm::BasicBlock *succ = BI->getSuccessor(i);
          if (succ->empty())
            continue;
          fork_opt->setEntryPred(BB);
          pushWork(succ, std::move(*fork_opt), BB);
        }
      }
    } else if (auto *SI = llvm::dyn_cast<llvm::SwitchInst>(term)) {
      for (auto &st : states) {
        if (st.isStopped() || st.isExceptionRaised())
          continue;

        for (const auto &case_handle : SI->cases()) {
          ExecutionDomain case_state = st.clone();
          AbductiveDomain *case_astate = case_state.getAstate();
          if (!case_astate)
            continue;

          auto cond = ops_.eval(*case_astate, SI->getCondition(), SI, pred_bb);
          auto case_value =
              ops_.eval(*case_astate, case_handle.getCaseValue(), SI, pred_bb);
          if (!cond || !case_value)
            continue;
          AbstractValue cond_c = case_astate->getCanonical(cond->addr);
          AbstractValue val_c = case_astate->getCanonical(case_value->addr);
          if (!case_astate->getPathFormula().addEquality(cond_c, val_c)) {
            continue;
          }
          auto inc = case_astate->incorporatePendingNewEqs();
          if (inc.isUnsat()) {
            continue;
          }
          if (inc.isPotentialInvalidAccess()) {
            latent_exit_states.push_back(ExecutionDomain::latentInvalidAccess(
                std::make_unique<AbductiveDomain>(case_astate->clone()),
                inc.address,
                std::make_pair(inc.must_be_valid.trace.clone(),
                               inc.must_be_valid.reason),
                {}));
            continue;
          }
          PathConditionAtom atom = PathConditionAtom::makeRel(
              PathConditionAtom::Kind::Equal, cond_c, val_c);
          atom.is_ground =
              case_astate->getPathFormula().asConstantInt(cond_c).has_value();
          case_astate->getPathFormula().recordCondition(atom, 0);

          const llvm::BasicBlock *succ = case_handle.getCaseSuccessor();
          if (succ->empty())
            continue;
          case_state.setEntryPred(BB);
          pushWork(succ, std::move(case_state), BB);
        }

        ExecutionDomain default_state = std::move(st);
        AbductiveDomain *default_astate = default_state.getAstate();
        if (!default_astate)
          continue;
        auto cond =
            ops_.eval(*default_astate, SI->getCondition(), SI, pred_bb);
        if (!cond)
          continue;

        bool feasible = true;
        for (const auto &case_handle : SI->cases()) {
          auto case_value = ops_.eval(*default_astate,
                                      case_handle.getCaseValue(), SI, pred_bb);
          if (!case_value ||
              !default_astate->getPathFormula().addDisequality(
                  default_astate->getCanonical(cond->addr),
                  default_astate->getCanonical(case_value->addr))) {
            feasible = false;
            break;
          }
        }
        const llvm::BasicBlock *default_succ = SI->getDefaultDest();
        if (feasible && !default_succ->empty()) {
          default_state.setEntryPred(BB);
          pushWork(default_succ, std::move(default_state), BB);
        }
      }
    } else {
      const unsigned num_succs = term->getNumSuccessors();
      for (auto &st : states) {
        if (st.isStopped() || st.isExceptionRaised())
          continue;
        for (unsigned si = 0; si < num_succs; ++si) {
          const llvm::BasicBlock *succ = term->getSuccessor(si);
          if (!succ || succ->empty())
            continue;
          ExecutionDomain succ_state =
              (si + 1 == num_succs) ? std::move(st) : st.clone();
          succ_state.setEntryPred(BB);
          pushWork(succ, std::move(succ_state), BB);
        }
      }
    }
  }

  if (!exit_states.empty() || !latent_exit_states.empty() || !spec_key)
    createSummary(F, exit_states, latent_exit_states, spec_key);

  if (!spec_key) {
    reportUnnecessaryCopies(F);
    reportConstRefableParams(F);
    if (!transitive_access_policies_.empty()) {
      if (const PulseSummary *sum = summary_manager_.getSummary(F)) {
        checkTransitiveAccessPolicies(F, sum->getTransitiveInfo());
      }
    }
  }
  if (isolate_non_disj) {
    analysis_non_disj_ = std::move(saved_non_disj);
  }
  if (inserted_active) {
    active_functions_.erase(F);
  }

  PulseLogger::endTimer("function." + F->getName().str());
  PulseLogger::logFunction(F, "completed analysis");
}

ExecutionDomain PulseChecker::initializeFunction(const llvm::Function *F) {
  ExecutionDomain exec_state;
  auto *astate = exec_state.getAstate();
  unsigned arg_idx = 0;
  for (auto &Arg : F->args()) {
    AbstractValue av = factory_.getOrCreate(&Arg);
    Address addr(av);
    addr.history.addEvent(ValueHistory::EventKind::Parameter, nullptr, F,
                          Arg.hasName() ? Arg.getName().str()
                                        : ("arg" + std::to_string(arg_idx)));
    // Bind ALL formal parameters (pointers and scalars) into BOTH post_stack_
    // and pre_stack_, and register them in pre_heap_ for sound conditional
    // bi-abduction (Infer PulseAbductiveDomain.mk_initial).
    astate->getPostStack().add(&Arg, addr);
    astate->getPreStack().add(&Arg, addr);
    astate->registerPreAddress(av);
    astate->setDecompilerName(av, Arg.hasName()
                                      ? Arg.getName().str()
                                      : ("arg" + std::to_string(arg_idx)));
    if (Arg.getType()->isIntegerTy()) {
      astate->getPathFormula().addIntegerConstraint(av);
    }
    arg_idx++;
  }
  return exec_state;
}

std::vector<std::pair<ExecutionDomain, std::optional<AbstractValue>>>
PulseChecker::runCallee(const llvm::Function *callee,
                        const ExecutionDomain &caller_state,
                        const llvm::CallInst *CI, const llvm::BasicBlock *pred,
                        unsigned call_depth) {
  std::vector<std::pair<ExecutionDomain, std::optional<AbstractValue>>> result;
  if (!callee || call_depth >= kMaxCallDepth) {
    return result;
  }
  if (!summary_manager_.hasSummary(callee) && current_scc_.count(callee) == 0 &&
      !callee->isDeclaration()) {
    analyzeFunction(callee);
  }
  auto states = applySummaryImproved(callee, caller_state, CI, pred, nullptr);
  for (auto &st : states) {
    std::optional<AbstractValue> ret_val;
    if (const auto *a = st.getAstate()) {
      if (const Address *ret_addr = a->getPostStack().find(CI)) {
        ret_val = a->getCanonical(ret_addr->addr);
      }
    }
    result.emplace_back(std::move(st), ret_val);
  }
  return result;
}

void PulseChecker::reportBug(OperationResult kind, const llvm::Instruction *loc,
                             AbstractValue addr, const Trace &trace,
                             const AbductiveDomain *astate) {
  PulseLogger::logBug(kind, loc);
  PulseLogger::incrementCounter("bugs.total");

  std::unique_ptr<Diagnostic> diagnostic;
  std::optional<std::string> var_name;
  if (astate) {
    var_name = astate->getDecompilerName(astate->getCanonical(addr));
  }
  const std::string var_suffix =
      var_name && !var_name->empty() ? (" (`" + *var_name + "`)") : "";

  switch (kind) {
  case OperationResult::UseAfterFree: {
    InvalidationKind invKind = InvalidationKind::Other;
    if (astate) {
      AbstractValue canon = astate->getCanonical(addr);
      auto inv = astate->getInvalidationInfo(canon);
      if (inv)
        invKind = inv->first;
    }
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Use after free detected",
        "Access to freed memory" + var_suffix,
        "Ensure the memory is not freed before access", IssueType::UseAfterFree,
        trace.clone(), invKind);
    break;
  }
  case OperationResult::NullDereference: {
    Trace filtered_trace;
    for (const auto &event : trace.getEvents()) {
      if (!event.location)
        continue;
      if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(event.location)) {
        const llvm::Value *stored_value = SI->getValueOperand();
        if (detail::isNullPointerConstantValue(stored_value)) {
          filtered_trace.addEvent(event.location, event.function,
                                  "Null constant stored");
        } else {
          filtered_trace.addEvent(event.location, event.function,
                                  event.description);
        }
      } else if (llvm::isa<llvm::CallBase>(event.location) ||
                 llvm::isa<llvm::BranchInst>(event.location) ||
                 llvm::isa<llvm::PHINode>(event.location) ||
                 llvm::isa<llvm::SelectInst>(event.location) ||
                 llvm::isa<llvm::LoadInst>(event.location)) {
        filtered_trace.addEvent(event.location, event.function,
                                event.description);
      }
    }
    filtered_trace.addEvent(loc, loc ? loc->getFunction() : nullptr,
                            "Null pointer dereference" + var_suffix);
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Null pointer dereference", "Pointer is null" + var_suffix,
        "Check for null before dereferencing", IssueType::NullDereference,
        std::move(filtered_trace));
    break;
  }
  case OperationResult::UninitializedRead:
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Uninitialized read",
        "Reading uninitialized memory" + var_suffix,
        "Initialize variable before use", IssueType::UninitializedRead,
        trace.clone());
    break;
  case OperationResult::OutOfBounds:
    diagnostic = std::make_unique<AccessToInvalidAddress>(
        loc, "Out of bounds access",
        "Access beyond allocated bounds" + var_suffix,
        "Ensure indices and lengths stay within the allocated object",
        IssueType::OutOfBounds, trace.clone());
    break;
  case OperationResult::TaintError:
    diagnostic = std::make_unique<TaintFlow>(loc, "Unknown Source",
                                             "Unknown Sink", trace.clone());
    break;
  default:
    return;
  }

  if (diagnostic) {
    DiagnosticManager::getInstance().report(std::move(diagnostic));
  }
}

void PulseChecker::reportUnnecessaryCopies(const llvm::Function *F) {
  (void)F;
  // Report only aggregate/struct copies proven unmodified across every
  // explored execution path (Infer's Copied vs Modified state machine).
  for (const auto &rec : analysis_non_disj_.getUnmodifiedCopies()) {
    if (!rec.location)
      continue;
    auto diag = std::make_unique<UnnecessaryCopy>(rec.location, rec.var_name,
                                                  rec.type_name);
    DiagnosticManager::getInstance().report(std::move(diag));
  }
}

void PulseChecker::reportConstRefableParams(const llvm::Function *F) {
  if (!F || F->empty())
    return;
  const llvm::Instruction *entry_loc = F->getEntryBlock().getFirstNonPHI();
  if (!entry_loc)
    return;
  for (const llvm::Argument *A : analysis_non_disj_.getConstRefableParams()) {
    std::string param_name =
        A->hasName() ? A->getName().str()
                     : ("arg" + std::to_string(A->getArgNo()));
    std::string type_name = llvmTypeToString(A->getType());
    auto diag = std::make_unique<ConstRefableParam>(entry_loc, param_name,
                                                    type_name);
    DiagnosticManager::getInstance().report(std::move(diag));
  }
}

void PulseChecker::reportDiagnostic(const llvm::Instruction *loc,
                                    const std::string &message,
                                    const std::string &type, int confidence) {
  (void)confidence;
  if (type == "TaintSink") {
    Trace trace;
    trace.addEvent(loc, message);
    auto diag = std::make_unique<TaintFlow>(loc, "Taint Source", "Taint Sink",
                                            std::move(trace));
    DiagnosticManager::getInstance().report(std::move(diag));
  } else {
    llvm::errs() << "[Pulse] " << type << ": " << message << " at " << *loc
                 << "\n";
  }
}

void PulseChecker::reportDiagnostic(const Diagnostic &diagnostic) {
  (void)diagnostic;
}

} // namespace pulse
