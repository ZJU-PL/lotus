#include "Checker/KINT/MKintAnalysisSupport.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

using namespace llvm;

namespace kint {

bool MKintPass::canSummarizeFunction(const Function &F) const {
  if (F.isDeclaration() || F.isVarArg())
    return false;
  if (!F.getReturnType()->isVoidTy() && !F.getReturnType()->isIntegerTy() &&
      !F.getReturnType()->isPointerTy())
    return false;
  for (const auto &bb : F) {
    for (const auto &inst : bb) {
      if (isa<InvokeInst>(inst) || isa<CallBrInst>(inst))
        return false;
      if (const auto *call = dyn_cast<CallInst>(&inst)) {
        if (call->isInlineAsm())
          return false;
        Function *callee = call->getCalledFunction();
        if (!callee && !isa<IntrinsicInst>(call))
          return false;
        if (callee && callee->isDeclaration() && !isAllocatorLike(callee) &&
            !isKnownReadOnlyLibcCall(callee->getName()) &&
            callee->getName() != "memset" && callee->getName() != "__memset" &&
            callee->getName() != "memcpy" && callee->getName() != "__memcpy" &&
            callee->getName() != "memmove" &&
            callee->getName() != "__memmove" && !call->doesNotAccessMemory() &&
            !call->onlyReadsMemory()) {
          return false;
        }
      }
    }
  }
  return true;
}

bool MKintPass::isAllocatorLike(const Function *callee) const {
  if (!callee)
    return false;
  const auto name = callee->getName();
  return name == "malloc" || name == "calloc" || name == "realloc" ||
         name == "kmalloc" || name == "kzalloc" || name == "vmalloc" ||
         name == "free";
}

bool MKintPass::classifyPointerReturn(const Function &F, const Value *&root,
                                      std::string &reason) const {
  root = nullptr;
  if (!F.getReturnType()->isPointerTy())
    return true;

  SmallPtrSet<const Value *, 16> visiting;
  auto resolveRoot = [&](const Value *value, const auto &self) -> const Value * {
    if (!value)
      return nullptr;
    if (!visiting.insert(value).second)
      return nullptr;
    auto cleanup = [&]() { visiting.erase(value); };
    const Value *result = nullptr;
    if (const auto *arg = dyn_cast<Argument>(value)) {
      result = arg;
    } else if (const auto *gv = dyn_cast<GlobalVariable>(value)) {
      result = gv;
    } else if (const auto *call = dyn_cast<CallInst>(value)) {
      if (isAllocatorLike(call->getCalledFunction()))
        result = call;
    } else if (const auto *fi = dyn_cast<FreezeInst>(value)) {
      result = self(fi->getOperand(0), self);
    } else if (const auto *bc = dyn_cast<BitCastOperator>(value)) {
      result = self(bc->getOperand(0), self);
    } else if (const auto *gep = dyn_cast<GEPOperator>(value)) {
      result = self(gep->getPointerOperand(), self);
    } else if (const auto *sel = dyn_cast<SelectInst>(value)) {
      const Value *t = self(sel->getTrueValue(), self);
      const Value *f = self(sel->getFalseValue(), self);
      if (t && t == f)
        result = t;
    } else if (const auto *phi = dyn_cast<PHINode>(value)) {
      const Value *common = nullptr;
      bool seen = false;
      bool ok = true;
      for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
        const Value *incomingRoot = self(phi->getIncomingValue(i), self);
        if (!incomingRoot) {
          ok = false;
          break;
        }
        if (!seen) {
          common = incomingRoot;
          seen = true;
        } else if (common != incomingRoot) {
          ok = false;
          break;
        }
      }
      if (ok)
        result = common;
    } else if (const auto *itp = dyn_cast<IntToPtrInst>(value)) {
      if (const auto *pti = dyn_cast<PtrToIntInst>(itp->getOperand(0)))
        result = self(pti->getOperand(0), self);
    }
    cleanup();
    return result;
  };

  for (const auto &bb : F) {
    const auto *ret = dyn_cast<ReturnInst>(bb.getTerminator());
    if (!ret)
      continue;
    const Value *retValue = ret->getReturnValue();
    if (!retValue || !retValue->getType()->isPointerTy()) {
      reason = "missing pointer return value";
      return false;
    }
    const Value *underlying = resolveRoot(retValue, resolveRoot);
    if (!underlying) {
      reason = "unresolved pointer return root";
      return false;
    }
    const bool supported =
        isa<Argument>(underlying) || isa<GlobalVariable>(underlying) ||
        (isa<CallInst>(underlying) &&
         isAllocatorLike(cast<CallInst>(underlying)->getCalledFunction()));
    if (!supported) {
      reason = "unsupported pointer return root";
      return false;
    }
    if (root && root != underlying) {
      reason = "multiple pointer return roots";
      return false;
    }
    root = underlying;
  }

  if (!root) {
    reason = "missing pointer return root";
    return false;
  }
  return true;
}

bool MKintPass::isBoundaryVisiblePointerArg(const Argument &arg,
                                            const Function &F) const {
  for (const auto *user : arg.users()) {
    const auto *inst = dyn_cast<Instruction>(user);
    if (!inst || inst->getFunction() != &F)
      continue;
    return true;
  }
  return false;
}

std::vector<const GlobalVariable *>
MKintPass::collectReferencedGlobals(const Function &F) const {
  SetVector<const GlobalVariable *> globals;
  for (const auto &bb : F) {
    for (const auto &inst : bb) {
      for (const auto &operand : inst.operands()) {
        const Value *op = operand.get();
        if (!op)
          continue;
        if (const auto *gv = dyn_cast<GlobalVariable>(op->stripPointerCasts()))
          globals.insert(gv);
      }
    }
  }
  return {globals.begin(), globals.end()};
}

std::vector<const Value *>
MKintPass::collectSummaryObjects(const Function &F) const {
  SetVector<const Value *> objects;
  const Value *pointerReturnRoot = nullptr;
  std::string reason;
  (void)classifyPointerReturn(F, pointerReturnRoot, reason);
  for (const auto &arg : F.args()) {
    if (arg.getType()->isPointerTy() &&
        (isBoundaryVisiblePointerArg(arg, F) || pointerReturnRoot == &arg)) {
      objects.insert(&arg);
    }
  }
  for (const auto *gv : collectReferencedGlobals(F))
    objects.insert(gv);
  if (isa_and_nonnull<CallInst>(pointerReturnRoot))
    objects.insert(pointerReturnRoot);
  return {objects.begin(), objects.end()};
}

bool MKintPass::collectModifiedBoundaryObjects(Function &F,
                                               FunctionSummary &summary) {
  auto boundaryContains = [&](const Value *root) {
    for (const auto &binding : summary.boundary_objects) {
      if (binding.root == root)
        return true;
    }
    return false;
  };
  auto markRoot = [&](const Value *root) {
    if (!root)
      return false;
    root = resolveAliasedObject(root);
    if (boundaryContains(root)) {
      summary.modified_objects.insert(root);
      return true;
    }
    if (isa<AllocaInst>(root))
      return true;
    if (const auto *call = dyn_cast<CallInst>(root)) {
      if (root == summary.pointer_return_root) {
        summary.modified_objects.insert(root);
        return true;
      }
      if (isAllocatorLike(call->getCalledFunction()))
        return true;
    }
    return false;
  };
  auto markPointer = [&](const Value *ptr) {
    if (!ptr)
      return false;
    const Value *root = llvm::getUnderlyingObject(ptr->stripPointerCasts());
    return markRoot(root);
  };

  if (summary.pointer_return_root && isa<CallInst>(summary.pointer_return_root))
    summary.modified_objects.insert(summary.pointer_return_root);

  for (const auto &bb : F) {
    for (const auto &inst : bb) {
      if (const auto *store = dyn_cast<StoreInst>(&inst)) {
        if (!markPointer(store->getPointerOperand())) {
          m_summary_failure_reason = "unsupported boundary store target";
          return false;
        }
        continue;
      }
      if (const auto *memsetI = dyn_cast<MemSetInst>(&inst)) {
        if (!markPointer(memsetI->getRawDest())) {
          m_summary_failure_reason = "unsupported memset target";
          return false;
        }
        continue;
      }
      if (const auto *memcpyI = dyn_cast<MemCpyInst>(&inst)) {
        if (!markPointer(memcpyI->getRawDest())) {
          m_summary_failure_reason = "unsupported memcpy target";
          return false;
        }
        continue;
      }
      if (const auto *memmoveI = dyn_cast<MemMoveInst>(&inst)) {
        if (!markPointer(memmoveI->getRawDest())) {
          m_summary_failure_reason = "unsupported memmove target";
          return false;
        }
        continue;
      }
      if (const auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
        if (!markPointer(rmw->getPointerOperand())) {
          m_summary_failure_reason = "unsupported atomicrmw target";
          return false;
        }
        continue;
      }
      if (const auto *cx = dyn_cast<AtomicCmpXchgInst>(&inst)) {
        if (!markPointer(cx->getPointerOperand())) {
          m_summary_failure_reason = "unsupported cmpxchg target";
          return false;
        }
        continue;
      }
      const auto *call = dyn_cast<CallInst>(&inst);
      if (!call)
        continue;
      Function *callee = call->getCalledFunction();
      if (!callee)
        continue;
      const auto name = callee->getName();
      if (name == "memset" || name == "__memset" || name == "memcpy" ||
          name == "__memcpy" || name == "memmove" || name == "__memmove") {
        if (call->arg_size() < 1 || !markPointer(call->getArgOperand(0))) {
          m_summary_failure_reason = "unsupported libc memory op target";
          return false;
        }
        continue;
      }
      if (name == "free") {
        if (call->arg_size() < 1) {
          m_summary_failure_reason = "unsupported free target";
          return false;
        }
        (void)markPointer(call->getArgOperand(0));
        continue;
      }
      if (name == "realloc") {
        if (call->arg_size() < 1) {
          m_summary_failure_reason = "unsupported realloc target";
          return false;
        }
        (void)markPointer(call->getArgOperand(0));
        continue;
      }
      if (isAllocatorLike(callee))
        continue;
      if (callee->isDeclaration()) {
        if (!isKnownReadOnlyLibcCall(name) && !call->doesNotAccessMemory() &&
            !call->onlyReadsMemory()) {
          m_summary_failure_reason = "unsupported external side-effecting call";
          return false;
        }
        continue;
      }

      const FunctionSummary *nested = buildSummary(*callee);
      if (!nested || nested->availability != SummaryAvailability::Available) {
        m_summary_failure_reason = "unsupported nested call summary";
        return false;
      }
      for (const auto &binding : nested->boundary_objects) {
        if (!nested->modified_objects.count(binding.root))
          continue;
        switch (binding.kind) {
        case SummaryObjectKind::Argument:
          if (binding.arg_index >= call->arg_size() ||
              !markPointer(call->getArgOperand(binding.arg_index))) {
            m_summary_failure_reason = "unsupported nested modified argument";
            return false;
          }
          break;
        case SummaryObjectKind::Global:
          if (!markRoot(binding.root)) {
            m_summary_failure_reason = "unsupported nested modified global";
            return false;
          }
          break;
        case SummaryObjectKind::EscapedReturn:
          if (!call->getType()->isPointerTy() || !markRoot(call)) {
            m_summary_failure_reason =
                "unsupported nested modified escaped return";
            return false;
          }
          break;
        }
      }
    }
  }

  return true;
}

z3::expr
MKintPass::conjunctSummaryExprs(const std::vector<z3::expr> &exprs) const {
  auto &ctx = m_solver.value().ctx();
  if (exprs.empty())
    return ctx.bool_val(true);
  z3::expr_vector clauses(ctx);
  for (const auto &e : exprs)
    clauses.push_back(e);
  return z3::mk_and(clauses);
}

z3::expr
MKintPass::disjunctSummaryExprs(const std::vector<z3::expr> &exprs) const {
  auto &ctx = m_solver.value().ctx();
  if (exprs.empty())
    return ctx.bool_val(false);
  z3::expr_vector clauses(ctx);
  for (const auto &e : exprs)
    clauses.push_back(e);
  return z3::mk_or(clauses);
}

z3::expr MKintPass::closeSummaryClause(const FunctionSummary &summary,
                                       const z3::expr &clause) const {
  auto &ctx = m_solver.value().ctx();
  llvm::DenseSet<Z3_ast> exported;
  auto markExported = [&](const std::optional<z3::expr> &sym) {
    if (sym.has_value())
      exported.insert(sym.value());
  };

  markExported(summary.integer_return_symbol);
  markExported(summary.pointer_return_symbol);
  for (const auto &[_, sym] : summary.arg_symbols)
    markExported(sym);
  for (const auto &binding : summary.boundary_objects) {
    markExported(binding.base_in_symbol);
    markExported(binding.size_in_symbol);
    markExported(binding.mem_in);
    markExported(binding.base_out_symbol);
    markExported(binding.size_out_symbol);
    markExported(binding.mem_out);
  }

  llvm::DenseSet<Z3_ast> visitedNodes;
  llvm::DenseSet<Z3_ast> visitedConsts;
  std::vector<z3::expr> hiddenSymbols;
  collectFreeConstants(clause, visitedNodes, visitedConsts, hiddenSymbols);

  z3::expr_vector quantified(ctx);
  for (const auto &sym : hiddenSymbols) {
    if (!exported.count(sym))
      quantified.push_back(sym);
  }
  if (quantified.empty())
    return clause.simplify();
  return z3::exists(quantified, clause).simplify();
}

void MKintPass::finalizeSummaryContract(FunctionSummary &summary) {
  auto &ctx = m_solver.value().ctx();

  for (auto &binding : summary.boundary_objects) {
    if (binding.kind == SummaryObjectKind::EscapedReturn) {
      if (binding.base_out_symbol.has_value()) {
        summary.allocation_constraints.push_back(
            binding.base_out_symbol.value() != ctx.bv_val(0, m_ptr_bits));
      }
      if (binding.base_out_symbol.has_value() &&
          binding.size_out_symbol.has_value()) {
        summary.allocation_constraints.push_back(z3::bvadd_no_overflow(
            binding.base_out_symbol.value(), binding.size_out_symbol.value(),
            /*is_signed=*/false));
      }
      for (const auto &other : summary.boundary_objects) {
        if (other.root == binding.root)
          continue;
        if (!binding.base_out_symbol.has_value() ||
            !binding.size_out_symbol.has_value() ||
            !other.base_in_symbol.has_value() ||
            !other.size_in_symbol.has_value()) {
          continue;
        }
        const auto endThis =
            binding.base_out_symbol.value() + binding.size_out_symbol.value();
        const auto endOther =
            other.base_in_symbol.value() + other.size_in_symbol.value();
        summary.allocation_constraints.push_back(
            z3::ule(endThis, other.base_in_symbol.value()) ||
            z3::ule(endOther, binding.base_out_symbol.value()));
      }
      continue;
    }

    if (binding.mem_in.has_value() && binding.mem_out.has_value() &&
        !summary.modified_objects.count(binding.root) &&
        binding.include_in_frame) {
      summary.frame_constraints.push_back(binding.mem_out.value() ==
                                          binding.mem_in.value());
    }
  }

  if (!summary.path_case_clauses.empty()) {
    summary.exit_constraints.push_back(
        disjunctSummaryExprs(summary.path_case_clauses).simplify());
  }
}

const FunctionSummary *MKintPass::buildSummary(Function &F) {
  if (InterprocSummaryMode == SummaryMode::Off)
    return nullptr;

  auto &entryPtr = m_summary_cache[&F];
  if (!entryPtr) {
    entryPtr = std::make_unique<SummaryCacheEntry>(&F);
  }
  SummaryCacheEntry &entry = *entryPtr;
  if (entry.summary.availability == SummaryAvailability::Available)
    return &entry.summary;
  if (entry.summary.availability == SummaryAvailability::Unsupported &&
      !entry.building && !entry.summary.unsupported_reason.empty())
    return &entry.summary;
  if (entry.building) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.recursive = true;
    entry.summary.unsupported_reason = "recursive summary dependency";
    return &entry.summary;
  }
  if (F.empty() || !canSummarizeFunction(F)) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason = "unsupported function shape";
    return &entry.summary;
  }

  const Value *pointerReturnRoot = nullptr;
  std::string pointerReturnReason;
  if (!classifyPointerReturn(F, pointerReturnRoot, pointerReturnReason)) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason = pointerReturnReason;
    return &entry.summary;
  }

  ensureBackedges(F);

  auto &ctx = m_solver.value().ctx();
  struct StateGuard {
    MKintPass &self;
    SummaryCacheEntry &entry;
    std::optional<z3::solver> old_solver;
    std::unique_ptr<SmtMemory> old_smt_mem;
    DenseMap<const Value *, std::optional<z3::expr>> old_v2sym;
    std::map<const BasicBlock *, SmallVector<BasicBlock *, 2>> old_bbpaths;
    DenseMap<const Value *, std::optional<z3::expr>> old_obj_base;
    DenseMap<const Value *, std::optional<z3::expr>> old_obj_size;
    std::vector<const Value *> old_obj_list;
    DenseMap<const Value *, std::optional<z3::expr>> old_obj_mem;
    DenseMap<const Value *, const Value *> old_obj_alias;
    DenseMap<const Value *, const Value *> old_int_alias;
    DenseMap<const Value *, std::optional<z3::expr>> old_ptr_offset;
    DenseMap<const Value *, std::optional<z3::expr>> old_int_offset;
    DenseSet<const Value *> old_obj_freed;
    DenseSet<const Value *> old_obj_size_known;
    std::vector<ObjChange> old_obj_change_log;
    std::vector<size_t> old_obj_change_frames;
    std::vector<SymChange> old_sym_change_log;
    std::vector<size_t> old_sym_change_frames;
    std::vector<z3::expr> old_path_constraints;
    std::vector<size_t> old_constraint_frames;
    std::vector<z3::expr> old_universal_vars;
    std::unordered_set<Z3_ast> old_universal_ids;
    DenseMap<std::pair<const BasicBlock *, const BasicBlock *>, unsigned>
        old_backedge_counts;
    FunctionSummary *old_building_summary;
    unsigned old_timeout;
    unsigned old_path_limit;
    uint64_t old_paths_explored;
    bool old_path_limit_hit;
    bool old_timeout_hit;
    bool old_loop_cut_hit;
    bool old_in_main_entry;
    bool old_summary_backedge_hit;
    std::chrono::time_point<std::chrono::steady_clock> old_function_start_time;
    llvm::AAResults *old_aa;
    llvm::MemorySSA *old_mssa;
    bool old_summary_failed;
    std::string old_summary_failure_reason;

    StateGuard(MKintPass &s, SummaryCacheEntry &e)
        : self(s), entry(e), old_solver(std::move(s.m_solver)),
          old_smt_mem(std::move(s.m_smt_mem)), old_v2sym(std::move(s.m_v2sym)),
          old_bbpaths(std::move(s.m_bbpaths)),
          old_obj_base(std::move(s.m_obj_base)),
          old_obj_size(std::move(s.m_obj_size)),
          old_obj_list(std::move(s.m_obj_list)),
          old_obj_mem(std::move(s.m_obj_mem)),
          old_obj_alias(std::move(s.m_obj_alias)),
          old_int_alias(std::move(s.m_int_alias)),
          old_ptr_offset(std::move(s.m_ptr_offset)),
          old_int_offset(std::move(s.m_int_offset)),
          old_obj_freed(std::move(s.m_obj_freed)),
          old_obj_size_known(std::move(s.m_obj_size_known)),
          old_obj_change_log(std::move(s.m_obj_change_log)),
          old_obj_change_frames(std::move(s.m_obj_change_frames)),
          old_sym_change_log(std::move(s.m_sym_change_log)),
          old_sym_change_frames(std::move(s.m_sym_change_frames)),
          old_path_constraints(std::move(s.m_path_constraints)),
          old_constraint_frames(std::move(s.m_constraint_frames)),
          old_universal_vars(std::move(s.m_universal_vars)),
          old_universal_ids(std::move(s.m_universal_var_ids)),
          old_backedge_counts(std::move(s.m_backedge_counts)),
          old_building_summary(s.m_building_summary),
          old_timeout(s.m_function_timeout), old_path_limit(s.m_path_limit),
          old_paths_explored(s.m_paths_explored),
          old_path_limit_hit(s.m_path_limit_hit),
          old_timeout_hit(s.m_timeout_hit),
          old_loop_cut_hit(s.m_loop_cut_hit),
          old_in_main_entry(s.m_in_main_entry),
          old_summary_backedge_hit(s.m_summary_backedge_hit),
          old_function_start_time(s.m_function_start_time), old_aa(s.m_aa),
          old_mssa(s.m_mssa), old_summary_failed(s.m_summary_failed),
          old_summary_failure_reason(std::move(s.m_summary_failure_reason)) {}

    ~StateGuard() {
      entry.building = false;
      self.m_solver = std::move(old_solver);
      self.m_smt_mem = std::move(old_smt_mem);
      self.m_v2sym = std::move(old_v2sym);
      self.m_bbpaths = std::move(old_bbpaths);
      self.m_obj_base = std::move(old_obj_base);
      self.m_obj_size = std::move(old_obj_size);
      self.m_obj_list = std::move(old_obj_list);
      self.m_obj_mem = std::move(old_obj_mem);
      self.m_obj_alias = std::move(old_obj_alias);
      self.m_int_alias = std::move(old_int_alias);
      self.m_ptr_offset = std::move(old_ptr_offset);
      self.m_int_offset = std::move(old_int_offset);
      self.m_obj_freed = std::move(old_obj_freed);
      self.m_obj_size_known = std::move(old_obj_size_known);
      self.m_obj_change_log = std::move(old_obj_change_log);
      self.m_obj_change_frames = std::move(old_obj_change_frames);
      self.m_sym_change_log = std::move(old_sym_change_log);
      self.m_sym_change_frames = std::move(old_sym_change_frames);
      self.m_path_constraints = std::move(old_path_constraints);
      self.m_constraint_frames = std::move(old_constraint_frames);
      self.m_universal_vars = std::move(old_universal_vars);
      self.m_universal_var_ids = std::move(old_universal_ids);
      self.m_backedge_counts = std::move(old_backedge_counts);
      self.m_building_summary = old_building_summary;
      self.m_function_timeout = old_timeout;
      self.m_path_limit = old_path_limit;
      self.m_paths_explored = old_paths_explored;
      self.m_path_limit_hit = old_path_limit_hit;
      self.m_timeout_hit = old_timeout_hit;
      self.m_loop_cut_hit = old_loop_cut_hit;
      self.m_in_main_entry = old_in_main_entry;
      self.m_summary_backedge_hit = old_summary_backedge_hit;
      self.m_function_start_time = old_function_start_time;
      self.m_aa = old_aa;
      self.m_mssa = old_mssa;
      self.m_summary_failed = old_summary_failed;
      self.m_summary_failure_reason = std::move(old_summary_failure_reason);
    }
  };
  StateGuard guard(*this, entry);

  m_solver = z3::solver(ctx);
  m_smt_mem = std::make_unique<SmtMemory>(ctx, m_ptr_bits);
  m_v2sym.clear();
  m_bbpaths.clear();
  m_obj_base.clear();
  m_obj_size.clear();
  m_obj_list.clear();
  m_obj_mem.clear();
  m_obj_alias.clear();
  m_int_alias.clear();
  m_ptr_offset.clear();
  m_int_offset.clear();
  m_obj_freed.clear();
  m_obj_size_known.clear();
  m_obj_change_log.clear();
  m_obj_change_frames.clear();
  m_sym_change_log.clear();
  m_sym_change_frames.clear();
  m_path_constraints.clear();
  m_constraint_frames.clear();
  m_universal_vars.clear();
  m_universal_var_ids.clear();
  m_backedge_counts.clear();
  m_in_main_entry = false;
  m_loop_cut_hit = false;

  entry.building = true;
  entry.summary = FunctionSummary(&F);
  entry.summary.has_integer_return = F.getReturnType()->isIntegerTy();
  entry.summary.has_pointer_return = F.getReturnType()->isPointerTy();
  entry.summary.pointer_return_root = pointerReturnRoot;
  if (entry.summary.has_integer_return) {
    const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
    entry.summary.integer_return_symbol = ctx.bv_const(
        ("%summary.ret." + F.getName().str() + "." + std::to_string(id))
            .c_str(),
        F.getReturnType()->getIntegerBitWidth());
  }
  if (entry.summary.has_pointer_return) {
    const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
    entry.summary.pointer_return_symbol = ctx.bv_const(
        ("%summary.ret.ptr." + F.getName().str() + "." + std::to_string(id))
            .c_str(),
        m_ptr_bits);
  }

  m_building_summary = &entry.summary;
  m_summary_failed = false;
  m_summary_failure_reason.clear();
  m_function_timeout = m_summary_timeout;
  m_path_limit = m_summary_path_limit;
  m_paths_explored = 0;
  m_path_limit_hit = false;
  m_timeout_hit = false;
  m_summary_backedge_hit = false;
  m_function_start_time = std::chrono::steady_clock::now();

  if (m_fam && (m_aa = m_fam->getCachedResult<llvm::AAManager>(F))) {
    // cached
  } else if (m_fam) {
    m_aa = &m_fam->getResult<llvm::AAManager>(F);
  } else {
    m_aa = nullptr;
  }
  if (m_fam) {
    if (auto *MSSARes = m_fam->getCachedResult<llvm::MemorySSAAnalysis>(F)) {
      m_mssa = &MSSARes->getMSSA();
    } else {
      m_mssa = &m_fam->getResult<llvm::MemorySSAAnalysis>(F).getMSSA();
    }
  } else {
    m_mssa = nullptr;
  }

  for (auto &arg : F.args()) {
    const std::string argName = ("summary." + F.getName().str() + ".arg." +
                                 std::to_string(arg.getArgNo()));
    if (arg.getType()->isIntegerTy()) {
      z3::expr sym =
          ctx.bv_const(argName.c_str(), arg.getType()->getIntegerBitWidth());
      entry.summary.integer_args.push_back(&arg);
      entry.summary.arg_symbols[&arg] = sym;
      setSym(&arg, sym);
    } else if (arg.getType()->isPointerTy()) {
      entry.summary.pointer_args.push_back(&arg);
      auto sizeSym = ctx.bv_const((argName + ".size").c_str(), m_ptr_bits);
      ensureObject(&arg, argName + ".obj", sizeSym, /*sizeKnown=*/false,
                   /*hasRealSize=*/false);
      z3::expr sym = m_obj_base[&arg].value();
      entry.summary.arg_symbols[&arg] = sym;
      setSym(&arg, sym);
    }
  }

  for (const Value *obj : collectSummaryObjects(F)) {
    SummaryObjectKind kind = SummaryObjectKind::Argument;
    if (isa<GlobalVariable>(obj))
      kind = SummaryObjectKind::Global;
    else if (obj == pointerReturnRoot && isa<CallInst>(obj))
      kind = SummaryObjectKind::EscapedReturn;

    if (kind == SummaryObjectKind::Global && !m_obj_base.count(obj)) {
      const auto *gv = cast<GlobalVariable>(obj);
      ensureGlobalObject(gv);
    }
    unsigned argIndex = ~0U;
    if (const auto *arg = dyn_cast<Argument>(obj))
      argIndex = arg->getArgNo();
    std::optional<z3::expr> baseSym = std::nullopt;
    std::optional<z3::expr> sizeSym = std::nullopt;
    std::optional<z3::expr> memIn = std::nullopt;
    if (kind != SummaryObjectKind::EscapedReturn && m_obj_base.count(obj)) {
      baseSym = m_obj_base[obj];
      sizeSym = m_obj_size[obj];
      if (m_obj_mem.count(obj))
        memIn = m_obj_mem[obj];
    }
    entry.summary.boundary_objects.emplace_back(
        kind, obj, argIndex, /*include_in_frame=*/true, baseSym, sizeSym,
        memIn, std::nullopt, std::nullopt, std::nullopt);
  }

  if (!collectModifiedBoundaryObjects(F, entry.summary)) {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason =
        m_summary_failure_reason.empty() ? "summary modifies collection failed"
                                         : m_summary_failure_reason;
    return &entry.summary;
  }

  for (auto &binding : entry.summary.boundary_objects) {
    if (binding.mem_in.has_value() ||
        entry.summary.modified_objects.count(binding.root)) {
      const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
      binding.mem_out = ctx.constant(
          ("%summary.memout." + F.getName().str() + "." + std::to_string(id))
              .c_str(),
          ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
    }
  }

  entry.summary.entry_constraints = m_path_constraints;

  for (auto &bb : F) {
    for (const auto *pred : predecessors(&bb)) {
      const bool isBackedge = m_backedges[&bb].contains(pred) || &bb == pred;
      if (isBackedge && m_max_loop_unroll == 0)
        continue;
      auto &succs = m_bbpaths[pred];
      if (std::find(succs.begin(), succs.end(), &bb) == succs.end())
        succs.push_back(&bb);
    }
  }

  m_solver.value().push();
  pushSymFrame();
  pushObjectFrame();
  m_smt_mem->push();
  pushConstraintFrame();
  path_solving(&F.getEntryBlock(), nullptr);
  for (auto &binding : entry.summary.boundary_objects) {
    if (binding.kind != SummaryObjectKind::EscapedReturn)
      continue;
    if (!m_obj_base.count(binding.root) || !m_obj_mem.count(binding.root) ||
        !m_obj_mem[binding.root].has_value()) {
      m_summary_failed = true;
      if (m_summary_failure_reason.empty()) {
        m_summary_failure_reason =
            "missing escaped return object state in summary";
      }
      break;
    }
    binding.base_out_symbol = m_obj_base[binding.root];
    binding.size_out_symbol = m_obj_size[binding.root];
    if (!binding.mem_out.has_value()) {
      const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
      binding.mem_out = ctx.constant(
          ("%summary.memout." + F.getName().str() + "." + std::to_string(id))
              .c_str(),
          ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
    }
  }
  m_smt_mem->pop();
  popSymFrame();
  popObjectFrame();
  popConstraintFrame();
  m_solver.value().pop();

  if (!m_summary_failed && (m_path_limit_hit || m_timeout_hit)) {
    m_summary_failed = true;
    if (m_summary_failure_reason.empty()) {
      m_summary_failure_reason = "summary exploration incomplete";
    }
  }
  if (!m_summary_failed && m_summary_backedge_hit) {
    m_summary_failed = true;
    if (m_summary_failure_reason.empty()) {
      m_summary_failure_reason = "loop backedge encountered in summary";
    }
  }

  if (!m_summary_failed && !entry.summary.path_case_clauses.empty()) {
    finalizeSummaryContract(entry.summary);
    entry.summary.availability = SummaryAvailability::Available;
  } else {
    entry.summary.availability = SummaryAvailability::Unsupported;
    entry.summary.unsupported_reason = m_summary_failure_reason.empty()
                                           ? "summary construction failed"
                                           : m_summary_failure_reason;
  }

  return &entry.summary;
}

SummaryAvailability MKintPass::applySummary(CallInst *call, BasicBlock *cur,
                                            BasicBlock *pred) {
  if (InterprocSummaryMode == SummaryMode::Off || !call)
    return SummaryAvailability::Disabled;
  Function *callee = call->getCalledFunction();
  if (!callee || callee->isDeclaration())
    return SummaryAvailability::Unsupported;

  const FunctionSummary *summary = buildSummary(*callee);
  if (!summary || summary->availability != SummaryAvailability::Available)
    return SummaryAvailability::Unsupported;

  auto &ctx = m_solver.value().ctx();
  z3::expr_vector from(ctx);
  z3::expr_vector to(ctx);

  for (const auto &arg : summary->integer_args) {
    if (arg->getArgNo() >= call->arg_size())
      return SummaryAvailability::Unsupported;
    Value *actual = call->getArgOperand(arg->getArgNo());
    if (!actual || !actual->getType()->isIntegerTy())
      return SummaryAvailability::Unsupported;
    auto it = summary->arg_symbols.find(arg);
    if (it == summary->arg_symbols.end() || !it->second.has_value())
      return SummaryAvailability::Unsupported;
    if (actual->getType()->getIntegerBitWidth() !=
        it->second.value().get_sort().bv_size())
      return SummaryAvailability::Unsupported;
    from.push_back(it->second.value());
    to.push_back(getIntExpr(actual, cur, pred));
  }
  for (const auto &arg : summary->pointer_args) {
    if (arg->getArgNo() >= call->arg_size())
      return SummaryAvailability::Unsupported;
    Value *actual = call->getArgOperand(arg->getArgNo());
    if (!actual || !actual->getType()->isPointerTy())
      return SummaryAvailability::Unsupported;
    auto it = summary->arg_symbols.find(arg);
    if (it == summary->arg_symbols.end() || !it->second.has_value())
      return SummaryAvailability::Unsupported;
    from.push_back(it->second.value());
    to.push_back(getPtrExpr(actual, cur, pred));
  }
  if (summary->integer_return_symbol.has_value()) {
    if (!call->getType()->isIntegerTy() ||
        call->getType()->getIntegerBitWidth() !=
            summary->integer_return_symbol.value().get_sort().bv_size())
      return SummaryAvailability::Unsupported;
  }
  if (summary->pointer_return_symbol.has_value()) {
    if (!call->getType()->isPointerTy())
      return SummaryAvailability::Unsupported;
    if (const auto *arg =
            dyn_cast_or_null<Argument>(summary->pointer_return_root)) {
      if (arg->getArgNo() >= call->arg_size() ||
          !call->getArgOperand(arg->getArgNo())->getType()->isPointerTy())
        return SummaryAvailability::Unsupported;
    }
  }

  const uint64_t callId = getStableValueId(call);
  auto mapBindingObject =
      [&](const SummaryObjectBinding &binding) -> const Value * {
    switch (binding.kind) {
    case SummaryObjectKind::Argument: {
      if (binding.arg_index >= call->arg_size())
        return nullptr;
      Value *actual = call->getArgOperand(binding.arg_index);
      if (!actual || !actual->getType()->isPointerTy())
        return nullptr;
      const Value *obj = getObjectForPtr(actual);
      if (!obj) {
        const Value *under =
            llvm::getUnderlyingObject(actual->stripPointerCasts());
        if (const auto *gv = dyn_cast_or_null<GlobalVariable>(under)) {
          ensureGlobalObject(gv);
          obj = gv;
        } else if (const auto *ai = dyn_cast_or_null<AllocaInst>(under)) {
          ensureAllocaObject(ai, cur, pred);
          obj = ai;
        }
      }
      return obj;
    }
    case SummaryObjectKind::Global: {
      const Value *resolved = resolveAliasedObject(binding.root);
      if (const auto *gv = dyn_cast_or_null<GlobalVariable>(resolved)) {
        ensureGlobalObject(gv);
      }
      return resolved;
    }
    case SummaryObjectKind::EscapedReturn: {
      if (!call->getType()->isPointerTy())
        return nullptr;
      if (!m_obj_base.count(call)) {
        const auto id =
            g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
        z3::expr sizeExpr = ctx.bv_const(
            ("%summary.call.size." + std::to_string(callId) + "." +
             std::to_string(id))
                .c_str(),
            m_ptr_bits);
        ensureObject(call, "summary.escape." + std::to_string(callId), sizeExpr,
                     /*sizeKnown=*/false,
                     /*hasRealSize=*/binding.size_out_symbol.has_value());
      }
      return call;
    }
    }
    return nullptr;
  };

  DenseMap<const Value *, const Value *> mappedBoundaryRoots;
  SmallVector<std::pair<const Value *, z3::expr>, 8> memUpdates;
  for (const auto &objBinding : summary->boundary_objects) {
    const Value *mappedObject = mapBindingObject(objBinding);
    if (!mappedObject)
      return SummaryAvailability::Unsupported;
    if (objBinding.kind == SummaryObjectKind::Argument) {
      if (m_obj_base.count(mappedObject) &&
          m_obj_base[mappedObject].has_value()) {
        z3::expr actualPtr =
            getPtrExpr(call->getArgOperand(objBinding.arg_index), cur, pred);
        z3::expr offsetExpr = actualPtr - m_obj_base[mappedObject].value();
        z3::expr simplified = offsetExpr.simplify();
        uint64_t offValue = 0;
        if (simplified.is_numeral() &&
            Z3_get_numeral_uint64(simplified.ctx(), simplified, &offValue) &&
            offValue != 0) {
          return SummaryAvailability::Unsupported;
        }
      }
    }
    auto mappedIt = mappedBoundaryRoots.find(mappedObject);
    if (mappedIt != mappedBoundaryRoots.end() &&
        mappedIt->second != objBinding.root) {
      // Summary construction assumes distinct visible roots denote distinct
      // boundary objects. If actuals alias, fall back instead of treating the
      // call as infeasible in the caller.
      return SummaryAvailability::Unsupported;
    }
    mappedBoundaryRoots[mappedObject] = objBinding.root;
    if (objBinding.base_in_symbol.has_value()) {
      if (!m_obj_base.count(mappedObject) ||
          !m_obj_base[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.base_in_symbol.value());
      to.push_back(m_obj_base[mappedObject].value());
    }
    if (objBinding.size_in_symbol.has_value()) {
      if (!m_obj_size.count(mappedObject) ||
          !m_obj_size[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.size_in_symbol.value());
      to.push_back(m_obj_size[mappedObject].value());
    }
    if (objBinding.mem_in.has_value()) {
      if (!m_obj_mem.count(mappedObject) ||
          !m_obj_mem[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.mem_in.value());
      to.push_back(m_obj_mem[mappedObject].value());
    }
    if (objBinding.base_out_symbol.has_value()) {
      if (!m_obj_base.count(mappedObject) ||
          !m_obj_base[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.base_out_symbol.value());
      to.push_back(m_obj_base[mappedObject].value());
    }
    if (objBinding.size_out_symbol.has_value()) {
      if (!m_obj_size.count(mappedObject) ||
          !m_obj_size[mappedObject].has_value())
        return SummaryAvailability::Unsupported;
      from.push_back(objBinding.size_out_symbol.value());
      to.push_back(m_obj_size[mappedObject].value());
    }
    if (objBinding.mem_out.has_value()) {
      const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
      z3::expr postMem =
          ctx.constant(("%summary.call.mem." + std::to_string(callId) + "." +
                        std::to_string(id))
                           .c_str(),
                       ctx.array_sort(ctx.bv_sort(m_ptr_bits), ctx.bv_sort(8)));
      from.push_back(objBinding.mem_out.value());
      to.push_back(postMem);
      memUpdates.emplace_back(mappedObject, postMem);
    }
  }

  if (summary->integer_return_symbol.has_value()) {
    const auto id = g_summary_expr_id.fetch_add(1, std::memory_order_relaxed);
    z3::expr retSym =
        ctx.bv_const(("%summary.call.ret." + std::to_string(callId) + "." +
                      std::to_string(id))
                         .c_str(),
                     call->getType()->getIntegerBitWidth());
    from.push_back(summary->integer_return_symbol.value());
    to.push_back(retSym);
    setSym(call, retSym);
  }

  if (summary->pointer_return_symbol.has_value()) {
    z3::expr retPtr = ctx.bv_const(
        ("%summary.call.ptr." + std::to_string(callId)).c_str(), m_ptr_bits);
    if (summary->pointer_return_root) {
      if (isa<CallInst>(summary->pointer_return_root)) {
        if (!m_obj_base.count(call) || !m_obj_base[call].has_value())
          return SummaryAvailability::Unsupported;
        retPtr = m_obj_base[call].value();
        setPtrOffset(call, ctx.bv_val(0, m_ptr_bits));
      } else if (const auto *arg =
                     dyn_cast<Argument>(summary->pointer_return_root)) {
        const Value *mappedRoot =
            getObjectForPtr(call->getArgOperand(arg->getArgNo()));
        if (mappedRoot)
          setObjAlias(call, mappedRoot);
        if (auto off = getPointerOffset(call->getArgOperand(arg->getArgNo())))
          setPtrOffset(call, off);
      } else if (const auto *gv =
                     dyn_cast<GlobalVariable>(summary->pointer_return_root)) {
        ensureGlobalObject(gv);
        setObjAlias(call, gv);
        setPtrOffset(call, ctx.bv_val(0, m_ptr_bits));
      }
    }
    from.push_back(summary->pointer_return_symbol.value());
    to.push_back(retPtr);
    setSym(call, retPtr);
  }

  addConstraint(
      conjunctSummaryExprs(summary->entry_constraints).substitute(from, to));
  if (m_solver.value().check() == z3::unsat)
    return SummaryAvailability::Available;

  // Boogie-style call step: after preconditions are checked against the
  // incoming state, expose fresh post-state symbols for modified visible
  // memory before constraining them with summary postconditions.
  for (const auto &update : memUpdates)
    setObjMem(update.first, update.second);

  addConstraint(
      conjunctSummaryExprs(summary->exit_constraints).substitute(from, to));
  addConstraint(conjunctSummaryExprs(summary->allocation_constraints)
                    .substitute(from, to));
  addConstraint(
      conjunctSummaryExprs(summary->frame_constraints).substitute(from, to));
  return SummaryAvailability::Available;
}

bool MKintPass::recordSummaryCase(ReturnInst *ret, BasicBlock *cur,
                                  BasicBlock *pred) {
  (void)pred;
  if (!m_building_summary || !ret)
    return true;

  z3::expr clause = buildPathConstraintConjunction();
  if (m_building_summary->integer_return_symbol.has_value()) {
    Value *retValue = ret->getReturnValue();
    if (!retValue || !retValue->getType()->isIntegerTy() ||
        retValue->getType()->getIntegerBitWidth() !=
            m_building_summary->integer_return_symbol.value()
                .get_sort()
                .bv_size()) {
      m_summary_failure_reason = "non-integer return in integer summary";
      return false;
    }
    clause = clause && (m_building_summary->integer_return_symbol.value() ==
                        getIntExpr(retValue, cur, nullptr));
  }
  if (m_building_summary->pointer_return_symbol.has_value()) {
    Value *retValue = ret->getReturnValue();
    if (!retValue || !retValue->getType()->isPointerTy()) {
      m_summary_failure_reason = "non-pointer return in pointer summary";
      return false;
    }
    clause = clause && (m_building_summary->pointer_return_symbol.value() ==
                        getPtrExpr(retValue, cur, nullptr));
  }
  for (const auto &objBinding : m_building_summary->boundary_objects) {
    if (!objBinding.mem_out.has_value())
      continue;
    auto it = m_obj_mem.find(resolveAliasedObject(objBinding.root));
    if (it == m_obj_mem.end() || !it->second.has_value()) {
      m_summary_failure_reason = "missing summary object memory state";
      return false;
    }
    clause = clause && (objBinding.mem_out.value() == it->second.value());
  }
  m_building_summary->path_case_clauses.push_back(
      closeSummaryClause(*m_building_summary, clause));
  return true;
}

} // namespace kint
