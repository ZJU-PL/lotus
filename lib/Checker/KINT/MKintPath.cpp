#include "Checker/KINT/MKintAnalysisSupport.h"

#include "Checker/KINT/Log.h"
#include "Checker/KINT/Options.h"

using namespace llvm;

namespace kint {

void MKintPass::path_solving(BasicBlock *cur, BasicBlock *pred) {
  if (m_summary_failed)
    return;
  // Bounded loop unrolling (Incorrectness Logic under-approximation):
  // traverse each loop backedge up to maxUnroll times along the current DFS
  // path before cutting it.
  const bool isBackedge =
      pred && (cur == pred || m_backedges[cur].contains(pred));
  struct BackedgeGuard {
    unsigned *counter = nullptr;
    ~BackedgeGuard() {
      if (counter && *counter > 0)
        --(*counter);
    }
  } backedgeGuard;
  if (isBackedge) {
    const unsigned maxUnroll = m_building_summary
                                   ? std::min(m_max_loop_unroll, 2u)
                                   : m_max_loop_unroll;
    auto &cnt = m_backedge_counts[{pred, cur}];
    if (cnt >= maxUnroll) {
      if (m_building_summary)
        m_summary_backedge_hit = true;
      else
        m_loop_cut_hit = true;
      return;
    }
    ++cnt;
    backedgeGuard.counter = &cnt;
  }

  // Cap path exploration to avoid blowups on large CFGs.
  if (m_path_limit > 0) {
    if (m_paths_explored++ >= m_path_limit) {
      if (!m_path_limit_hit) {
        MKINT_WARN() << "Path exploration limit reached for function "
                     << (cur && cur->getParent() ? cur->getParent()->getName()
                                                 : "<unknown>")
                     << " (limit=" << m_path_limit << "). Analysis incomplete.";
        m_path_limit_hit = true;
      }
      return;
    }
  }

  // Check for timeout
  if (m_function_timeout > 0) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - m_function_start_time)
                       .count();
    if (elapsed > static_cast<int64_t>(m_function_timeout)) {
      MKINT_WARN() << "Timeout reached for function "
                   << cur->getParent()->getName() << " after " << elapsed
                   << " seconds. Analysis incomplete.";
      m_timeout_hit = true;
      return;
    }
  }

  // Track this basic block in the current execution path (skip while building
  // summaries so caller bug paths remain untouched and summary builds avoid
  // unnecessary string allocations).
  if (!m_building_summary && m_bug_detection) {
    std::string bbDesc = "Basic block ";
    if (cur->hasName()) {
      bbDesc += cur->getName().str();
    } else {
      bbDesc += "<unnamed>";
    }
    if (cur->getParent()) {
      bbDesc += " in function " + cur->getParent()->getName().str();
    }

    PathPoint pathPoint(cur, nullptr, bbDesc);
    m_bug_detection->addPathPoint(pathPoint);
  }

  auto pointerAccessBytes = [&](const Value *ptr) -> uint64_t {
    if (!ptr || !ptr->getType()->isPointerTy())
      return 0;
    Type *elemTy = ptr->getType()->getPointerElementType();
    if (!elemTy || elemTy->isVoidTy() || elemTy->isFunctionTy())
      return 0;
    return getTypeStoreSizeSafe(m_dl, elemTy).value_or(0);
  };

  auto ensureTrackedObject = [&](const Value *ptr) -> const Value * {
    if (!ptr || !ptr->getType()->isPointerTy())
      return nullptr;
    const Value *obj = getObjectForPtr(ptr);
    if (obj)
      return obj;
    const Value *under = llvm::getUnderlyingObject(ptr->stripPointerCasts());
    if (const auto *gv = dyn_cast_or_null<GlobalVariable>(under)) {
      ensureGlobalObject(gv);
      return gv;
    }
    if (const auto *ai = dyn_cast_or_null<AllocaInst>(under)) {
      ensureAllocaObject(ai, cur, pred);
      return ai;
    }
    return nullptr;
  };

  auto havocObjectAtPtr = [&](const Value *obj, const Value *ptr,
                              uint64_t numBytes,
                              const std::string &hint) -> bool {
    if (!obj || !ptr || numBytes == 0)
      return false;
    if (!m_obj_base.count(obj) || !m_obj_base[obj].has_value())
      return false;
    const auto addr = getPtrExpr(ptr, cur, pred);
    const auto base = m_obj_base[obj].value();
    havocObjectRange(obj, addr - base, numBytes, hint);
    return true;
  };

  if (nullptr != pred) {
    auto *terminator = pred->getTerminator();
    auto *br = dyn_cast<BranchInst>(terminator);
    if (br) {
      if (br->isConditional() && br->getSuccessor(0) != br->getSuccessor(1)) {
        const bool is_true_br = br->getSuccessor(0) == cur;
        Value *cond = br->getCondition();

        // If the condition is an ICmp, encode it precisely (including all
        // integer and pointer predicates). If unsupported, leave both edges
        // conservatively feasible instead of forcing condBool=true.
        bool hasCond = false;
        z3::expr condBool = m_solver.value().ctx().bool_val(true);
        if (auto *cmp = dyn_cast<ICmpInst>(cond)) {
          auto *lhs = cmp->getOperand(0);
          auto *rhs = cmp->getOperand(1);
          auto encodePredicate = [&](const z3::expr &l, const z3::expr &r) {
            switch (cmp->getPredicate()) {
            case ICmpInst::ICMP_EQ:
              condBool = (l == r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_NE:
              condBool = (l != r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_SGT:
              condBool = z3::sgt(l, r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_SGE:
              condBool = z3::sge(l, r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_SLT:
              condBool = z3::slt(l, r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_SLE:
              condBool = z3::sle(l, r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_UGT:
              condBool = z3::ugt(l, r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_UGE:
              condBool = z3::uge(l, r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_ULT:
              condBool = z3::ult(l, r);
              hasCond = true;
              break;
            case ICmpInst::ICMP_ULE:
              condBool = z3::ule(l, r);
              hasCond = true;
              break;
            default:
              MKINT_WARN() << "Unsupported icmp predicate in branch condition: "
                           << *cmp;
              break;
            }
          };

          if (lhs->getType()->isIntegerTy() && rhs->getType()->isIntegerTy()) {
            encodePredicate(getIntExpr(lhs, pred, nullptr),
                            getIntExpr(rhs, pred, nullptr));
          } else if (lhs->getType()->isPointerTy() &&
                     rhs->getType()->isPointerTy()) {
            encodePredicate(getPtrExpr(lhs, pred, nullptr),
                            getPtrExpr(rhs, pred, nullptr));
          } else {
            MKINT_WARN()
                << "Unsupported icmp operand types in branch condition: "
                << *cmp;
          }

          if (!m_building_summary && m_bug_detection) {
            std::string branchDesc = std::string("Taking ") +
                                     (is_true_br ? "true" : "false") +
                                     " branch from condition: ";
            llvm::raw_string_ostream brOS(branchDesc);
            brOS << *cmp;
            PathPoint branchPoint(pred, cmp, brOS.str());
            m_bug_detection->addPathPoint(branchPoint);
          }
        } else if (cond->getType()->isIntegerTy(1)) {
          // Generic i1 condition.
          const auto c = getIntExpr(cond, pred, nullptr);
          condBool = (c == m_solver.value().ctx().bv_val(1, 1));
          hasCond = true;
        } else {
          MKINT_WARN() << "Unsupported branch condition: " << *cond;
        }

        if (hasCond) {
          addConstraint(is_true_br ? condBool : !condBool);
          auto *cmp = dyn_cast<ICmpInst>(cond);
          if (m_solver.value().check() == z3::unsat) {
            if (!m_building_summary && CheckDeadBranch && cmp) {
              auto &bf = m_branch_feasibility[cmp];
              if (is_true_br)
                bf.unsat_true = true;
              else
                bf.unsat_false = true;
            }
            MKINT_DEBUG() << "[SMT Solving] Pruned unsat edge into "
                          << cur->getName();
            return;
          }
          if (!m_building_summary && CheckDeadBranch && cmp) {
            auto &bf = m_branch_feasibility[cmp];
            if (is_true_br)
              bf.feasible_true = true;
            else
              bf.feasible_false = true;
          }
        }
      }
    } else if (auto *swt = dyn_cast<SwitchInst>(terminator)) {
      auto *cond = swt->getCondition();
      if (cond->getType()->isIntegerTy()) {
        auto &ctx = m_solver.value().ctx();
        const auto condExpr = getIntExpr(cond, pred, nullptr);
        if (swt->getDefaultDest() == cur) {
          // Reaching default (and any explicit cases that also target cur)
          // means cond does not match any case targeting a DIFFERENT block.
          for (auto c : swt->cases()) {
            if (c.getCaseSuccessor() != cur) {
              auto *case_val = c.getCaseValue();
              addConstraint(condExpr !=
                            bvValFromAPInt(ctx, case_val->getValue()));
            }
          }
        } else {
          // Disjunction of ALL cases targeting cur.
          z3::expr_vector matches(ctx);
          for (auto c : swt->cases()) {
            if (c.getCaseSuccessor() == cur) {
              auto *case_val = c.getCaseValue();
              matches.push_back(condExpr ==
                                bvValFromAPInt(ctx, case_val->getValue()));
            }
          }
          if (!matches.empty()) {
            addConstraint(z3::mk_or(matches));
          }
        }
        if (m_solver.value().check() == z3::unsat) {
          MKINT_DEBUG() << "[SMT Solving] Pruned unsat switch edge into "
                        << cur->getName();
          return;
        }
      }
    } else if (isa<InvokeInst>(terminator) || isa<IndirectBrInst>(terminator) ||
               isa<CallBrInst>(terminator)) {
      // No additional constraints; successor feasibility handled
      // conservatively.
    } else {
      // try catch... (thank god, C does not have try-catch)
      // indirectbr... ?
      MKINT_WARN() << "Unknown terminator; proceeding conservatively: "
                   << *pred->getTerminator();
    }
  }

  // Resolve PHI nodes simultaneously (parallel copy semantics) and propagate
  // pointer/integer object-alias and offset metadata across edges.
  if (pred) {
    struct ResolvedPhi {
      PHINode *phi;
      z3::expr expr;
      const Value *origin;
      std::optional<z3::expr> offset;
    };
    SmallVector<ResolvedPhi, 8> resolvedPhis;
    for (auto &inst : cur->getInstList()) {
      auto *phi = dyn_cast<PHINode>(&inst);
      if (!phi)
        break;
      if (Value *incoming = phi->getIncomingValueForBlock(pred)) {
        z3::expr incomingExpr = getValueExpr(incoming, pred, nullptr);
        const Value *origin = getPointerOrigin(incoming);
        auto offset = getPointerOffset(incoming);
        resolvedPhis.push_back(
            {phi, std::move(incomingExpr), origin, std::move(offset)});
      }
    }
    for (const auto &rp : resolvedPhis) {
      setSym(rp.phi, rp.expr);
      if (rp.phi->getType()->isPointerTy()) {
        setObjAlias(rp.phi, rp.origin);
        setPtrOffset(rp.phi, rp.offset);
      } else if (rp.phi->getType()->isIntegerTy()) {
        setIntAlias(rp.phi, rp.origin);
        setIntOffset(rp.phi, rp.offset);
      }
    }
  }

  for (auto &inst : cur->getInstList()) {
    if (isa<PHINode>(&inst))
      continue;

    // When revisiting a block via bounded loop unrolling, clear any cached
    // expression from an earlier iteration so this instruction recomputes
    // against the current iteration's PHI/memory state.
    if (m_v2sym.count(&inst)) {
      eraseSym(&inst);
    }

    if (auto *ret = dyn_cast<ReturnInst>(&inst)) {
      if (m_building_summary && !recordSummaryCase(ret, cur, pred)) {
        m_summary_failed = true;
        return;
      }
      continue;
    }

    if (auto *assumeI = dyn_cast<AssumeInst>(&inst)) {
      // llvm.assume(cond) adds a constraint to the current path.
      const auto *cond = assumeI->getArgOperand(0);
      if (cond && cond->getType()->isIntegerTy(1)) {
        const auto c = getIntExpr(cond, cur, pred);
        addConstraint(c == m_solver.value().ctx().bv_val(1, 1));
        if (m_solver.value().check() == z3::unsat)
          return;
      }
      continue;
    }

    if (auto *woi = dyn_cast<WithOverflowInst>(&inst)) {
      // Encode arithmetic/overflow semantics for llvm.*with.overflow
      // intrinsics. Bug checking: if overflow is satisfiable under current path
      // constraints, report an overflow.
      if (!m_building_summary && CheckIntOverflow && woi->arg_size() >= 2 &&
          woi->getArgOperand(0)->getType()->isIntegerTy() &&
          (!m_overflow_insts.count(woi) || !m_dump_ef_path.empty())) {
        z3::expr res = m_solver.value().ctx().bv_val(
            0, woi->getArgOperand(0)->getType()->getIntegerBitWidth());
        z3::expr ov = m_solver.value().ctx().bool_val(false);
        if (computeWithOverflow(
                woi, m_solver.value(),
                [&](const llvm::Value *x) { return getIntExpr(x, cur, pred); },
                res, ov)) {
          if (checkBugCondition(woi, interr::INT_OVERFLOW, ov)) {
            m_overflow_insts.insert(woi);
            if (m_bug_detection)
              m_bug_detection->recordBug(woi, interr::INT_OVERFLOW);
          }
        }
      }
      continue;
    }

    // Model memory intrinsics.
    // These often appear as `llvm.memset/memcpy/memmove.*` and bypass normal
    // CallInst handling.
    if (auto *memsetI = dyn_cast<MemSetInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memsetI->getRawDest(), cur, pred);
      const auto val = getIntExpr(memsetI->getValue(), cur, pred);
      const Value *obj = ensureTrackedObject(memsetI->getRawDest());
      if (const auto len = getConstantU64(memsetI->getLength())) {
        if (*len <= kMaxBytes) {
          if (obj && m_obj_base.count(obj) && m_obj_base[obj].has_value() &&
              m_obj_mem.count(obj) && m_obj_mem[obj].has_value()) {
            const auto base = m_obj_base[obj].value();
            const auto off = dst - base;
            z3::expr b = val;
            const unsigned bw = b.get_sort().bv_size();
            if (bw < 8)
              b = z3::zext(b, 8 - bw);
            else if (bw > 8)
              b = b.extract(7, 0);
            z3::expr curMem = m_obj_mem[obj].value();
            for (uint64_t i = 0; i < *len; ++i) {
              curMem = z3::store(
                  curMem, off + m_solver.value().ctx().bv_val(i, m_ptr_bits),
                  b);
            }
            setObjMem(obj, curMem);
          } else {
            m_smt_mem->memsetBytes(dst, val, *len);
          }
        } else {
          bool handledWhole = false;
          if (obj && m_obj_base.count(obj) && m_obj_base[obj].has_value() &&
              m_obj_size.count(obj) && m_obj_size[obj].has_value() &&
              m_obj_size[obj].value().is_numeral()) {
            uint64_t objSize = 0;
            uint64_t offVal = 0;
            z3::expr offSimpl = (dst - m_obj_base[obj].value()).simplify();
            if (Z3_get_numeral_uint64(m_obj_size[obj].value().ctx(),
                                      m_obj_size[obj].value(), &objSize) &&
                offSimpl.is_numeral() &&
                Z3_get_numeral_uint64(offSimpl.ctx(), offSimpl, &offVal) &&
                offVal == 0 && *len == objSize) {
              z3::expr b = val;
              const unsigned bw = b.get_sort().bv_size();
              if (bw < 8)
                b = z3::zext(b, 8 - bw);
              else if (bw > 8)
                b = b.extract(7, 0);
              setObjMem(obj, z3::const_array(
                                 m_solver.value().ctx().bv_sort(m_ptr_bits), b));
              handledWhole = true;
            }
          }
          if (!handledWhole) {
            if (obj)
              havocObject(obj, "memset_large");
            else
              m_smt_mem->havoc("memset_large");
          }
        }
        if (!maybeCheckOOB(memsetI, memsetI->getRawDest(), *len, cur, pred))
          return;
      } else {
        if (obj)
          havocObject(obj, "memset_sym");
        else
          m_smt_mem->havoc("memset_sym");
      }
      continue;
    }

    if (auto *memcpyI = dyn_cast<MemCpyInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memcpyI->getRawDest(), cur, pred);
      const auto src = getPtrExpr(memcpyI->getRawSource(), cur, pred);
      const Value *dstObj = ensureTrackedObject(memcpyI->getRawDest());
      const Value *srcObj = ensureTrackedObject(memcpyI->getRawSource());
      if (const auto len = getConstantU64(memcpyI->getLength())) {
        if (*len <= kMaxBytes) {
          if (dstObj && srcObj && m_obj_base.count(dstObj) &&
              m_obj_base[dstObj].has_value() && m_obj_base.count(srcObj) &&
              m_obj_base[srcObj].has_value() && m_obj_mem.count(dstObj) &&
              m_obj_mem[dstObj].has_value() && m_obj_mem.count(srcObj) &&
              m_obj_mem[srcObj].has_value()) {
            const auto dstBase = m_obj_base[dstObj].value();
            const auto srcBase = m_obj_base[srcObj].value();
            z3::expr dstOff = dst - dstBase;
            z3::expr srcOff = src - srcBase;
            z3::expr dstMem = m_obj_mem[dstObj].value();
            z3::expr srcMem = m_obj_mem[srcObj].value();
            for (uint64_t i = 0; i < *len; ++i) {
              z3::expr b = z3::select(
                  srcMem,
                  srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits));
              dstMem = z3::store(
                  dstMem, dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits),
                  b);
            }
            setObjMem(dstObj, dstMem);
          } else if (dstObj) {
            havocObject(dstObj, "memcpy_unknown_src");
          } else {
            m_smt_mem->memcpyBytes(dst, src, *len);
          }
        } else {
          if (dstObj)
            havocObject(dstObj, "memcpy_large");
          else
            m_smt_mem->havoc("memcpy_large");
        }
        if (!maybeCheckOOB(memcpyI, memcpyI->getRawDest(), *len, cur, pred))
          return;
        if (!maybeCheckOOB(memcpyI, memcpyI->getRawSource(), *len, cur, pred))
          return;
      } else {
        if (dstObj)
          havocObject(dstObj, "memcpy_sym");
        else
          m_smt_mem->havoc("memcpy_sym");
      }
      continue;
    }

    if (auto *memmoveI = dyn_cast<MemMoveInst>(&inst)) {
      constexpr uint64_t kMaxBytes = 256;
      const auto dst = getPtrExpr(memmoveI->getRawDest(), cur, pred);
      const auto src = getPtrExpr(memmoveI->getRawSource(), cur, pred);
      const Value *dstObj = ensureTrackedObject(memmoveI->getRawDest());
      const Value *srcObj = ensureTrackedObject(memmoveI->getRawSource());
      if (const auto len = getConstantU64(memmoveI->getLength())) {
        if (*len <= kMaxBytes) {
          if (dstObj && srcObj && m_obj_base.count(dstObj) &&
              m_obj_base[dstObj].has_value() && m_obj_base.count(srcObj) &&
              m_obj_base[srcObj].has_value() && m_obj_mem.count(dstObj) &&
              m_obj_mem[dstObj].has_value() && m_obj_mem.count(srcObj) &&
              m_obj_mem[srcObj].has_value()) {
            const auto dstBase = m_obj_base[dstObj].value();
            const auto srcBase = m_obj_base[srcObj].value();
            z3::expr dstOff = dst - dstBase;
            z3::expr srcOff = src - srcBase;
            z3::expr dstMem = m_obj_mem[dstObj].value();
            z3::expr srcMem = m_obj_mem[srcObj].value();
            std::vector<z3::expr> snapshot;
            snapshot.reserve(*len);
            for (uint64_t i = 0; i < *len; ++i) {
              snapshot.push_back(z3::select(
                  srcMem, srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits)));
            }
            for (uint64_t i = 0; i < *len; ++i) {
              dstMem = z3::store(
                  dstMem, dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits),
                  snapshot[i]);
            }
            setObjMem(dstObj, dstMem);
          } else if (dstObj) {
            havocObject(dstObj, "memmove_unknown_src");
          } else {
            m_smt_mem->memmoveBytes(dst, src, *len);
          }
        } else {
          if (dstObj)
            havocObject(dstObj, "memmove_large");
          else
            m_smt_mem->havoc("memmove_large");
        }
        if (!maybeCheckOOB(memmoveI, memmoveI->getRawDest(), *len, cur, pred))
          return;
        if (!maybeCheckOOB(memmoveI, memmoveI->getRawSource(), *len, cur, pred))
          return;
      } else {
        if (dstObj)
          havocObject(dstObj, "memmove_sym");
        else
          m_smt_mem->havoc("memmove_sym");
      }
      continue;
    }

    if (auto *ai = dyn_cast<AllocaInst>(&inst)) {
      ensureAllocaObject(ai, cur, pred);
      setPtrOffset(ai, m_solver.value().ctx().bv_val(0, m_ptr_bits));
      setSym(ai, m_obj_base[ai].value());
      continue;
    }

    if (auto *gep = dyn_cast<GetElementPtrInst>(&inst)) {
      if (gep->getType()->isPointerTy())
        setSym(gep, getPtrExpr(gep, cur, pred));
      continue;
    }

    if (auto *load = dyn_cast<LoadInst>(&inst)) {
      const uint64_t bytes =
          getTypeStoreSizeSafe(m_dl, load->getType()).value_or(0);
      if (bytes > 0 &&
          !maybeCheckOOB(load, load->getPointerOperand(), bytes, cur, pred))
        return;
      if (load->getType()->isIntegerTy() || load->getType()->isPointerTy()) {
        const auto addr = getPtrExpr(load->getPointerOperand(), cur, pred);
        const unsigned bw = load->getType()->isPointerTy()
                                ? m_ptr_bits
                                : load->getType()->getIntegerBitWidth();
        const Value *obj = ensureTrackedObject(load->getPointerOperand());
        z3::expr v = m_solver.value().ctx().bv_val(0, bw == 0 ? 1 : bw);
        if (bytes > 0) {
          const bool hasPreciseObjectMem =
              (obj && m_obj_base.count(obj) && m_obj_base[obj].has_value() &&
               m_obj_mem.count(obj) && m_obj_mem[obj].has_value());
          if (hasPreciseObjectMem) {
            const auto base = m_obj_base[obj].value();
            const auto off = addr - base;
            z3::expr bytesExpr = loadBytesFromMem(
                m_obj_mem[obj].value(), off, static_cast<unsigned>(bytes),
                isLittleEndian());
            const unsigned loadedBits = static_cast<unsigned>(bytes) * 8;
            if (bw == loadedBits) {
              v = bytesExpr;
            } else if (bw < loadedBits) {
              v = bytesExpr.extract(bw - 1, 0);
            } else {
              v = z3::zext(bytesExpr, bw - loadedBits);
            }
          } else {
            v = load->getType()->isPointerTy()
                    ? m_smt_mem->loadBytes(addr, static_cast<unsigned>(bytes),
                                           isLittleEndian())
                    : m_smt_mem->loadInt(addr, bw, static_cast<unsigned>(bytes),
                                         isLittleEndian());
          }
        }
        setSym(load, v);
        if (m_robust_universal_unknown_loads) {
          bool unknown = true;
          const Value *underlying =
              llvm::getUnderlyingObject(load->getPointerOperand());
          if (underlying && m_obj_base.count(underlying)) {
            unknown = false;
          }
          if (unknown) {
            registerUniversal(v);
          }
        }
        if (m_robust_universal_external_globals) {
          if (const auto *gv = dyn_cast_or_null<GlobalVariable>(
                  llvm::getUnderlyingObject(load->getPointerOperand()))) {
            if (gv->isDeclaration()) {
              registerUniversal(v);
            }
          }
        }
      }
      continue;
    }

    if (auto *store = dyn_cast<StoreInst>(&inst)) {
      auto *val = store->getValueOperand();
      const uint64_t bytes =
          val ? getTypeStoreSizeSafe(m_dl, val->getType()).value_or(0) : 0;
      if (bytes > 0 &&
          !maybeCheckOOB(store, store->getPointerOperand(), bytes, cur, pred))
        return;
      const Value *obj = ensureTrackedObject(store->getPointerOperand());
      if (val && bytes > 0 &&
          (val->getType()->isIntegerTy() || val->getType()->isPointerTy())) {
        const auto addr = getPtrExpr(store->getPointerOperand(), cur, pred);
        const unsigned bw = val->getType()->isPointerTy()
                                ? m_ptr_bits
                                : val->getType()->getIntegerBitWidth();
        const auto v = val->getType()->isPointerTy()
                           ? getPtrExpr(val, cur, pred)
                           : getIntExpr(val, cur, pred);
        if (obj && m_obj_base.count(obj) && m_obj_base[obj].has_value() &&
            m_obj_mem.count(obj) && m_obj_mem[obj].has_value()) {
          const auto base = m_obj_base[obj].value();
          const auto off = addr - base;
          z3::expr newMem = storeBytesToMem(m_obj_mem[obj].value(), off, v,
                                            static_cast<unsigned>(bytes),
                                            isLittleEndian());
          setObjMem(obj, newMem);
        } else {
          m_smt_mem->storeInt(addr, v, bw, static_cast<unsigned>(bytes),
                              isLittleEndian());
        }
      } else if (val) {
        // Non-integer/non-pointer store (e.g., float, struct, vector): havoc
        // the written byte range so subsequent integer loads don't read stale
        // values through unions or bitcasts.
        if (obj && bytes > 0 &&
            havocObjectAtPtr(obj, store->getPointerOperand(), bytes,
                             "store_nonint")) {
          // precise range havoc
        } else if (obj) {
          havocObject(obj, "store_nonint");
        } else {
          m_smt_mem->havoc("store_nonint");
        }
      }
      continue;
    }

    if (auto *rmw = dyn_cast<AtomicRMWInst>(&inst)) {
      const Value *obj = ensureTrackedObject(rmw->getPointerOperand());
      const uint64_t bytes =
          getTypeStoreSizeSafe(m_dl, rmw->getValOperand()->getType())
              .value_or(0);
      if (obj && bytes > 0 &&
          havocObjectAtPtr(obj, rmw->getPointerOperand(), bytes, "atomicrmw")) {
        // handled precisely
      } else if (obj) {
        havocObject(obj, "atomicrmw");
      } else {
        m_smt_mem->havoc("atomicrmw");
      }
      if (rmw->getType()->isIntegerTy()) {
        (void)getIntExpr(rmw, cur, pred);
      }
      continue;
    }

    if (auto *cx = dyn_cast<AtomicCmpXchgInst>(&inst)) {
      const Value *obj = ensureTrackedObject(cx->getPointerOperand());
      const uint64_t bytes =
          getTypeStoreSizeSafe(m_dl, cx->getCompareOperand()->getType())
              .value_or(0);
      if (obj && bytes > 0 &&
          havocObjectAtPtr(obj, cx->getPointerOperand(), bytes, "cmpxchg")) {
        // handled precisely
      } else if (obj) {
        havocObject(obj, "cmpxchg");
      } else {
        m_smt_mem->havoc("cmpxchg");
      }
      continue;
    }

    if (auto *call = dyn_cast<CallInst>(&inst)) {
      // Model common libc memory routines (when they survive as regular calls).
      Function *callee = call->getCalledFunction();
      bool is_allocator = false;
      if (callee) {
        constexpr uint64_t kMaxBytes = 256;
        const auto name = callee->getName();

        if ((name == "memset" || name == "__memset") && call->arg_size() >= 3 &&
            call->getArgOperand(0)->getType()->isPointerTy() &&
            call->getArgOperand(1)->getType()->isIntegerTy() &&
            call->getArgOperand(2)->getType()->isIntegerTy()) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto val = getIntExpr(call->getArgOperand(1), cur, pred);
          const Value *obj = ensureTrackedObject(call->getArgOperand(0));
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              if (obj && m_obj_base.count(obj) && m_obj_base[obj].has_value() &&
                  m_obj_mem.count(obj) && m_obj_mem[obj].has_value()) {
                const auto base = m_obj_base[obj].value();
                const auto off = dst - base;
                z3::expr b = val;
                const unsigned bw = b.get_sort().bv_size();
                if (bw < 8)
                  b = z3::zext(b, 8 - bw);
                else if (bw > 8)
                  b = b.extract(7, 0);
                z3::expr curMem = m_obj_mem[obj].value();
                for (uint64_t i = 0; i < *len; ++i) {
                  curMem = z3::store(
                      curMem,
                      off + m_solver.value().ctx().bv_val(i, m_ptr_bits), b);
                }
                setObjMem(obj, curMem);
              } else {
                m_smt_mem->memsetBytes(dst, val, *len);
              }
            } else {
              bool handledWhole = false;
              if (obj && m_obj_base.count(obj) && m_obj_base[obj].has_value() &&
                  m_obj_size.count(obj) && m_obj_size[obj].has_value() &&
                  m_obj_size[obj].value().is_numeral()) {
                uint64_t objSize = 0;
                uint64_t offVal = 0;
                z3::expr offSimpl = (dst - m_obj_base[obj].value()).simplify();
                if (Z3_get_numeral_uint64(m_obj_size[obj].value().ctx(),
                                          m_obj_size[obj].value(), &objSize) &&
                    offSimpl.is_numeral() &&
                    Z3_get_numeral_uint64(offSimpl.ctx(), offSimpl, &offVal) &&
                    offVal == 0 && *len == objSize) {
                  z3::expr b = val;
                  const unsigned bw = b.get_sort().bv_size();
                  if (bw < 8)
                    b = z3::zext(b, 8 - bw);
                  else if (bw > 8)
                    b = b.extract(7, 0);
                  setObjMem(obj,
                            z3::const_array(
                                m_solver.value().ctx().bv_sort(m_ptr_bits), b));
                  handledWhole = true;
                }
              }
              if (!handledWhole) {
                if (obj)
                  havocObject(obj, "memset_large");
                else
                  m_smt_mem->havoc("memset_large");
              }
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
          } else {
            if (obj)
              havocObject(obj, "memset_sym");
            else
              m_smt_mem->havoc("memset_sym");
          }
          // memset returns dst.
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if ((name == "memcpy" || name == "__memcpy") && call->arg_size() >= 3 &&
            call->getArgOperand(0)->getType()->isPointerTy() &&
            call->getArgOperand(1)->getType()->isPointerTy() &&
            call->getArgOperand(2)->getType()->isIntegerTy()) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto src = getPtrExpr(call->getArgOperand(1), cur, pred);
          const Value *dstObj = ensureTrackedObject(call->getArgOperand(0));
          const Value *srcObj = ensureTrackedObject(call->getArgOperand(1));
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              if (dstObj && srcObj && m_obj_base.count(dstObj) &&
                  m_obj_base[dstObj].has_value() && m_obj_base.count(srcObj) &&
                  m_obj_base[srcObj].has_value() && m_obj_mem.count(dstObj) &&
                  m_obj_mem[dstObj].has_value() && m_obj_mem.count(srcObj) &&
                  m_obj_mem[srcObj].has_value()) {
                const auto dstBase = m_obj_base[dstObj].value();
                const auto srcBase = m_obj_base[srcObj].value();
                z3::expr dstOff = dst - dstBase;
                z3::expr srcOff = src - srcBase;
                z3::expr dstMem = m_obj_mem[dstObj].value();
                z3::expr srcMem = m_obj_mem[srcObj].value();
                for (uint64_t i = 0; i < *len; ++i) {
                  z3::expr b = z3::select(
                      srcMem,
                      srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits));
                  dstMem = z3::store(
                      dstMem,
                      dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits), b);
                }
                setObjMem(dstObj, dstMem);
              } else if (dstObj) {
                if (!havocObjectAtPtr(dstObj, call->getArgOperand(0), *len,
                                      "memcpy_unknown_src")) {
                  havocObject(dstObj, "memcpy_unknown_src");
                }
              } else {
                m_smt_mem->memcpyBytes(dst, src, *len);
              }
            } else {
              if (dstObj)
                havocObject(dstObj, "memcpy_large");
              else
                m_smt_mem->havoc("memcpy_large");
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
            if (!maybeCheckOOB(call, call->getArgOperand(1), *len, cur, pred))
              return;
          } else {
            if (dstObj)
              havocObject(dstObj, "memcpy_sym");
            else
              m_smt_mem->havoc("memcpy_sym");
          }
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if ((name == "memmove" || name == "__memmove") &&
            call->arg_size() >= 3 &&
            call->getArgOperand(0)->getType()->isPointerTy() &&
            call->getArgOperand(1)->getType()->isPointerTy() &&
            call->getArgOperand(2)->getType()->isIntegerTy()) {
          const auto dst = getPtrExpr(call->getArgOperand(0), cur, pred);
          const auto src = getPtrExpr(call->getArgOperand(1), cur, pred);
          const Value *dstObj = ensureTrackedObject(call->getArgOperand(0));
          const Value *srcObj = ensureTrackedObject(call->getArgOperand(1));
          if (const auto len = getConstantU64(call->getArgOperand(2))) {
            if (*len <= kMaxBytes) {
              if (dstObj && srcObj && m_obj_base.count(dstObj) &&
                  m_obj_base[dstObj].has_value() && m_obj_base.count(srcObj) &&
                  m_obj_base[srcObj].has_value() && m_obj_mem.count(dstObj) &&
                  m_obj_mem[dstObj].has_value() && m_obj_mem.count(srcObj) &&
                  m_obj_mem[srcObj].has_value()) {
                const auto dstBase = m_obj_base[dstObj].value();
                const auto srcBase = m_obj_base[srcObj].value();
                z3::expr dstOff = dst - dstBase;
                z3::expr srcOff = src - srcBase;
                z3::expr dstMem = m_obj_mem[dstObj].value();
                z3::expr srcMem = m_obj_mem[srcObj].value();
                for (uint64_t i = 0; i < *len; ++i) {
                  z3::expr b = z3::select(
                      srcMem,
                      srcOff + m_solver.value().ctx().bv_val(i, m_ptr_bits));
                  dstMem = z3::store(
                      dstMem,
                      dstOff + m_solver.value().ctx().bv_val(i, m_ptr_bits), b);
                }
                setObjMem(dstObj, dstMem);
              } else if (dstObj) {
                if (!havocObjectAtPtr(dstObj, call->getArgOperand(0), *len,
                                      "memmove_unknown_src")) {
                  havocObject(dstObj, "memmove_unknown_src");
                }
              } else {
                m_smt_mem->memmoveBytes(dst, src, *len);
              }
            } else {
              if (dstObj)
                havocObject(dstObj, "memmove_large");
              else
                m_smt_mem->havoc("memmove_large");
            }
            if (!maybeCheckOOB(call, call->getArgOperand(0), *len, cur, pred))
              return;
            if (!maybeCheckOOB(call, call->getArgOperand(1), *len, cur, pred))
              return;
          } else {
            if (dstObj)
              havocObject(dstObj, "memmove_sym");
            else
              m_smt_mem->havoc("memmove_sym");
          }
          if (call->getType()->isPointerTy())
            setSym(call, dst);
          continue;
        }

        if (!callee->isDeclaration() &&
            !(name == "malloc" || name == "calloc" || name == "realloc" ||
              name == "free" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc")) {
          const SummaryAvailability summaryStatus =
              applySummary(call, cur, pred);
          if (summaryStatus == SummaryAvailability::Available) {
            if (m_solver.value().check() == z3::unsat)
              return;
            continue;
          }
          if (m_building_summary &&
              summaryStatus == SummaryAvailability::Unsupported) {
            m_summary_failed = true;
            if (m_summary_failure_reason.empty())
              m_summary_failure_reason =
                  "unsupported nested direct call in summary build";
            return;
          }
          if (summaryStatus == SummaryAvailability::Unsupported &&
              InterprocSummaryMode == SummaryMode::Required) {
            MKINT_WARN() << "Falling back to conservative call modeling for "
                         << callee->getName();
          }
        }

        // Terminate path immediately on noreturn calls (exit, abort, assert).
        if (call->doesNotReturn() || isKnownNoReturnLibcCall(name)) {
          return;
        }

        is_allocator = isAllocatorLike(callee) || name == "free";
        const bool is_known_readonly = isKnownReadOnlyLibcCall(name);
        if (m_building_summary && callee->isDeclaration() && !is_allocator &&
            !is_known_readonly && !call->doesNotAccessMemory() &&
            !call->onlyReadsMemory()) {
          m_summary_failed = true;
          if (m_summary_failure_reason.empty()) {
            m_summary_failure_reason =
                "unsupported external side-effecting call in summary";
          }
          return;
        }

        if (name == "free" && call->arg_size() >= 1) {
          if (const Value *obj = ensureTrackedObject(call->getArgOperand(0))) {
            invalidateObject(obj, "free");
          } else {
            m_smt_mem->havoc("free_unknown");
          }
          continue;
        }

        if (name == "strlen" && call->arg_size() >= 1 &&
            call->getType()->isIntegerTy() &&
            call->getArgOperand(0)->getType()->isPointerTy()) {
          auto retExpr = getIntExpr(call, cur, pred);
          const Value *srcObj = ensureTrackedObject(call->getArgOperand(0));
          if (srcObj && m_obj_base.count(srcObj) &&
              m_obj_base[srcObj].has_value() && m_obj_size.count(srcObj) &&
              m_obj_size[srcObj].has_value() &&
              m_obj_size[srcObj].value().is_numeral()) {
            uint64_t objSize = 0;
            uint64_t offVal = 0;
            auto srcPtr = getPtrExpr(call->getArgOperand(0), cur, pred);
            z3::expr offSimpl = (srcPtr - m_obj_base[srcObj].value()).simplify();
            if (Z3_get_numeral_uint64(m_obj_size[srcObj].value().ctx(),
                                      m_obj_size[srcObj].value(), &objSize) &&
                objSize > 0 && offSimpl.is_numeral() &&
                Z3_get_numeral_uint64(offSimpl.ctx(), offSimpl, &offVal) &&
                offVal < objSize) {
              const uint64_t remaining = objSize - offVal;
              const unsigned rbw = retExpr.get_sort().bv_size();
              if (rbw >= 64 || remaining < (1ULL << rbw)) {
                addConstraint(
                    z3::ult(retExpr,
                            m_solver.value().ctx().bv_val(remaining, rbw)));
              }
            }
          }
          continue;
        }
      } else if (call->doesNotReturn()) {
        return;
      }

      // Unknown direct or indirect call with memory side effects:
      // conservatively havoc memory so subsequent loads don't assume stale
      // contents.
      const bool knownReadOnly =
          callee && isKnownReadOnlyLibcCall(callee->getName());
      const bool knownArgMemOnly =
          callee && isKnownArgMemOnlyLibcCall(callee->getName());
      if (!is_allocator && !knownReadOnly && !isa<IntrinsicInst>(call) &&
          !call->doesNotAccessMemory() && !call->onlyReadsMemory()) {
        bool any = false;
        SmallPtrSet<const Value *, 8> touchedObjects;
        SmallPtrSet<const Value *, 8> touchedRoots;
        for (unsigned idx = 0; idx < call->arg_size(); ++idx) {
          const Value *arg = call->getArgOperand(idx);
          if (!arg || !arg->getType()->isPointerTy())
            continue;
          const Value *obj = getObjectForPtr(arg);
          if (!obj || m_obj_freed.contains(obj) ||
              (!knownArgMemOnly && !callMayModObject(call, obj))) {
            continue;
          }
          const Value *resolved = resolveAliasedObject(obj);
          if (resolved && touchedRoots.contains(resolved))
            continue;
          const uint64_t bytes = pointerAccessBytes(arg);
          // For argmemonly calls like read/fgets/snprintf/strcpy, the call may
          // write beyond a single char element of the passed buffer; havoc the
          // whole target object when element size is 1 byte (char*/i8*).
          if (knownArgMemOnly && bytes <= 1) {
            havocObject(obj, "call_argmem_buf");
            touchedObjects.insert(obj);
            if (resolved)
              touchedRoots.insert(resolved);
            any = true;
          } else if (bytes > 0 &&
                     havocObjectAtPtr(obj, arg, bytes, "call_sidefx_field")) {
            touchedObjects.insert(obj);
            if (resolved)
              touchedRoots.insert(resolved);
            any = true;
          }
        }
        if (!knownArgMemOnly) {
          for (const auto *obj : m_obj_list) {
            if (touchedObjects.contains(obj) || m_obj_freed.contains(obj))
              continue;
            const Value *resolved = resolveAliasedObject(obj);
            if (resolved && touchedRoots.contains(resolved))
              continue;
            if (callMayModObject(call, obj)) {
              havocObject(obj, "call_sidefx");
              any = true;
            }
          }
        }
        if (!any) {
          m_smt_mem->havoc("call_sidefx");
        }
      }

      // Model common allocators as fresh, disjoint heap objects.
      if (call->getType()->isPointerTy()) {
        if (callee) {
          const auto name = callee->getName();
          z3::expr sizeBytes = m_solver.value().ctx().bv_val(0, m_ptr_bits);
          bool sizeKnown = false;
          bool hasSizeArg = false;
          if (name == "malloc" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc") {
            if (call->arg_size() >= 1 &&
                call->getArgOperand(0)->getType()->isIntegerTy()) {
              sizeBytes = getIntExpr(call->getArgOperand(0), cur, pred);
              const unsigned abw = sizeBytes.get_sort().bv_size();
              if (abw < m_ptr_bits)
                sizeBytes = z3::zext(sizeBytes, m_ptr_bits - abw);
              if (abw > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
              hasSizeArg = true;
            }
            sizeKnown = hasSizeArg && sizeBytes.is_numeral();
          } else if (name == "calloc") {
            if (call->arg_size() >= 2 &&
                call->getArgOperand(0)->getType()->isIntegerTy() &&
                call->getArgOperand(1)->getType()->isIntegerTy()) {
              auto n = getIntExpr(call->getArgOperand(0), cur, pred);
              auto m = getIntExpr(call->getArgOperand(1), cur, pred);
              const unsigned n_bw = n.get_sort().bv_size();
              const unsigned m_bw = m.get_sort().bv_size();
              const unsigned target =
                  std::max(std::max(n_bw, m_bw), m_ptr_bits);
              if (n_bw < target)
                n = z3::zext(n, target - n_bw);
              if (m_bw < target)
                m = z3::zext(m, target - m_bw);
              sizeBytes = (n * m);
              if (target > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
              hasSizeArg = true;
            }
            sizeKnown = hasSizeArg && sizeBytes.is_numeral();
          } else if (name == "realloc") {
            if (call->arg_size() >= 2 &&
                call->getArgOperand(1)->getType()->isIntegerTy()) {
              sizeBytes = getIntExpr(call->getArgOperand(1), cur, pred);
              const unsigned abw = sizeBytes.get_sort().bv_size();
              if (abw < m_ptr_bits)
                sizeBytes = z3::zext(sizeBytes, m_ptr_bits - abw);
              if (abw > m_ptr_bits)
                sizeBytes = sizeBytes.extract(m_ptr_bits - 1, 0);
              hasSizeArg = true;
            }
            sizeKnown = hasSizeArg && sizeBytes.is_numeral();
          }

          if (name == "malloc" || name == "kmalloc" || name == "kzalloc" ||
              name == "vmalloc" || name == "calloc" || name == "realloc") {
            if (name == "realloc" && call->arg_size() >= 1) {
              if (const Value *oldObj =
                      ensureTrackedObject(call->getArgOperand(0))) {
                invalidateObject(oldObj, "realloc_old");
              }
            }
            ensureObject(call,
                         ("heap." + cur->getParent()->getName().str() + "." +
                          std::to_string(getStableValueId(call))),
                         sizeBytes, sizeKnown, /*hasRealSize=*/hasSizeArg);
            if (name == "calloc" || name == "kzalloc") {
              setObjMem(call, z3::const_array(
                                  m_solver.value().ctx().bv_sort(m_ptr_bits),
                                  m_solver.value().ctx().bv_val(0, 8)));
            }
            setPtrOffset(call, m_solver.value().ctx().bv_val(0, m_ptr_bits));
            setSym(call, m_obj_base[call].value());
            continue;
          }
        }

        // Unknown pointer-returning call: treat as fresh pointer value.
        setSym(call, getPtrExpr(call, cur, pred));
        continue;
      }

      // Inline asm return value can be treated as unknown.
      if (call->getType()->isIntegerTy() && call->isInlineAsm()) {
        auto v = getIntExpr(call, cur, pred);
        if (m_robust_universal_inline_asm) {
          registerUniversal(v);
        }
        continue;
      }
    }

    // Integer SSA: keep existing bug checks, but also allow values derived from
    // loads, selects, etc.
    if (inst.getType()->isIntegerTy()) {
      if (auto *op = dyn_cast<BinaryOperator>(&inst)) {
        (void)getIntExpr(op->getOperand(0), cur, pred);
        (void)getIntExpr(op->getOperand(1), cur, pred);
        if (!m_building_summary) {
          std::function<void(interr, const z3::expr &)> dumpCb;
          if (!m_dump_ef_path.empty()) {
            dumpCb = [this, op](interr type, const z3::expr &q) {
              dumpEfConstraint(op, type, q);
            };
          }
          m_bug_detection->binary_check(
              op, m_solver.value(), m_v2sym, m_overflow_insts,
              m_bad_shift_insts, m_div_zero_insts, m_robust_reachability,
              &m_path_constraints, &m_universal_vars, dumpCb,
              [this](interr type) { return isRobustBugEnabled(type); });
        }
        if (!addWellDefinedConstraints(op, cur, pred))
          return;
        const auto r =
            m_bug_detection->binary_op_propagate(op, m_v2sym, m_solver.value());
        setSym(op, r);
      } else if (auto *op = dyn_cast<CastInst>(&inst)) {
        if (isa<PtrToIntInst>(op)) {
          const auto r = getIntExpr(op, cur, pred);
          setSym(op, r);
        } else {
          (void)getValueExpr(op->getOperand(0), cur, pred);
          const auto r = m_bug_detection->cast_op_propagate(op, m_v2sym,
                                                            m_solver.value());
          setSym(op, r);
        }
      } else {
        (void)getIntExpr(&inst, cur, pred);
      }
    } else if (inst.getType()->isPointerTy()) {
      (void)getPtrExpr(&inst, cur, pred);
    }
  }

  for (auto *succ : m_bbpaths[cur]) {
    if (m_summary_failed || m_timeout_hit || m_path_limit_hit)
      break;
    m_solver.value().push();
    pushSymFrame();
    pushObjectFrame();
    m_smt_mem->push();
    pushConstraintFrame();
    // Record the path depth before recursing so we can restore it in place.
    const size_t pathDepthBefore =
        (!m_building_summary && m_bug_detection)
            ? m_bug_detection->getCurrentPath().size()
            : 0;
    path_solving(succ, cur);
    if (!m_building_summary && m_bug_detection) {
      m_bug_detection->truncateCurrentPath(pathDepthBefore);
    }
    m_smt_mem->pop();
    popSymFrame();
    popObjectFrame();
    popConstraintFrame();
    m_solver.value().pop();
  }
}

} // namespace kint
