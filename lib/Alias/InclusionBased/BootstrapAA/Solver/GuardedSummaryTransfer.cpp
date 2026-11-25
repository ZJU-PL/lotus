#include "Alias/InclusionBased/BootstrapAA/Solver/GuardedSummarySolver.h"

#include <algorithm>
#include <limits>

namespace lotus {
namespace bootstrap {
namespace detail {

// Materialize Deref(s) at `site` into guarded Mem(obj) / Top entries using
// `PT_s` (preferring `Point::After` when `site` is the defining Load/Call of
// `s`, otherwise `Point::Before`).
void GuardedSummarySolver::materializeDeref(const Expr &deref, Id site,
                                            std::size_t target_depth,
                                            GuardId guard,
                                            BddManager::NodeId path,
                                            std::vector<SummaryEntry> &out,
                                            Point site_point) {
  PointsToSet raw_pts;
  if (site_point == Point::After && deref.id != INVALID &&
      !m_program.values[deref.id].constant) {
    FsciKey akey{deref.id, site, Point::After};
    if (m_cyclic_pass1_depth != std::numeric_limits<std::size_t>::max() &&
        m_hierarchy.depth[m_hierarchy.value_components[deref.id]] >=
            m_cyclic_pass1_depth) {
      raw_pts = lookupPass1Fsci(deref.id, site, Point::After);
    } else {
      auto it = m_fsci_cache.find(akey);
      if (it != m_fsci_cache.end()) {
        raw_pts = it->second;
      } else if (!m_active_fsci.count(akey)) {
        raw_pts = computeFsci(deref.id, site, Point::After);
        m_fsci_cache.emplace(akey, raw_pts);
      } else {
        raw_pts = fsciForSummary(deref.id, site, target_depth);
      }
    }
  } else {
    raw_pts = fsciForSummary(deref.id, site, target_depth);
  }
  const PointsToSet addr_pts =
      applyOffset(raw_pts, deref.deref_offset, deref.deref_offset_known,
                  deref.deref_stride);
  if (addr_pts.empty())
    return;
  if (addr_pts.isTop()) {
    out.push_back({Expr::makeTop(), guard, path});
    return;
  }
  const bool constant_addr = m_program.values[deref.id].constant;
  for (Id obj : addr_pts.objects()) {
    if (obj == NULL_OBJECT)
      continue;
    GuardId g = guard;
    if (!constant_addr && site_point == Point::Before) {
      GuardAtom atom;
      atom.site = site;
      atom.kind = GuardKind::PointsTo;
      atom.lhs = deref.id;
      atom.rhs = obj;
      atom.lhs_offset = deref.deref_offset;
      atom.lhs_offset_known = deref.deref_offset_known;
      atom.lhs_stride = deref.deref_stride;
      g = conjoinAtom(g, atom);
      if (g == GUARD_FALSE)
        continue;
    }
    const Object &info = m_program.objects[obj];
    if (!isMemory(info) ||
        (deref.read_bytes != 0 &&
         (info.bytes == 0 || info.bytes != deref.read_bytes))) {
      out.push_back({Expr::makeTop(), g, path});
    } else {
      out.push_back({Expr::makeMem(obj, deref.val_offset,
                                   deref.val_offset_known, deref.val_stride),
                     g, path});
    }
  }
}

// Algorithm 4 (Kahlon PLDI '08, p. 257): backward transfer of `(q, guard,
// path)` across a single non-Call instruction `inst` at site `m`.
void GuardedSummarySolver::transferBackward(const Instruction &inst,
                                            const Expr &q, GuardId guard,
                                            BddManager::NodeId path,
                                            std::size_t target_depth,
                                            std::vector<SummaryEntry> &out) {
  tick();
  // Soundly forget any path-condition variable(s) defined/killed by this
  // instruction when stepping backwards before its definition.
  path = quantifyInstConditions(inst, path);

  // Algorithm 4 line 2: statements outside St_P leave the tracked expression
  // unchanged (p. 251: "all statements of Prog other than those in St_Q are
  // ignored in Prog_Q").
  if (q.isTerminal() || !m_slice.instructions[inst.id]) {
    out.push_back({q, guard, path});
    return;
  }

  switch (inst.opcode) {
  case Opcode::Nop:
  case Opcode::Return:
    out.push_back({q, guard, path});
    return;

  case Opcode::Unknown:
    if ((q.kind == ExprKind::Val || q.kind == ExprKind::Deref) &&
        q.id == inst.result)
      out.push_back({Expr::makeTop(), guard, path});
    else
      out.push_back({q, guard, path});
    return;

  case Opcode::Address:
    if (q.kind == ExprKind::Val && q.id == inst.result) {
      const PointsToSet resolved = m_program.resolveField(
          inst.object, q.val_offset, q.val_offset_known, q.val_stride);
      emitPointsToSources(resolved, 0, true, 0, guard, path, out);
    } else if (q.kind == ExprKind::Deref && q.id == inst.result) {
      const PointsToSet resolved = m_program.resolveField(
          inst.object, q.deref_offset, q.deref_offset_known, q.deref_stride);
      if (resolved.isTop()) {
        out.push_back({Expr::makeTop(), guard, path});
      } else {
        for (Id obj : resolved.objects()) {
          const Object &info = m_program.objects[obj];
          if (!isMemory(info) ||
              (q.read_bytes != 0 &&
               (info.bytes == 0 || info.bytes != q.read_bytes)))
            out.push_back({Expr::makeTop(), guard, path});
          else
            out.push_back({Expr::makeMem(obj, q.val_offset, q.val_offset_known,
                                         q.val_stride),
                           guard, path});
        }
      }
    } else {
      out.push_back({q, guard, path});
    }
    return;

  case Opcode::Allocate: {
    if (q.kind == ExprKind::Val && q.id == inst.result) {
      const PointsToSet resolved = m_program.resolveField(
          inst.object, q.val_offset, q.val_offset_known, q.val_stride);
      emitPointsToSources(resolved, 0, true, 0, guard, path, out);
      if (inst.may_be_null)
        out.push_back({Expr::makeNull(), guard, path});
      for (Id op : inst.operands) {
        if (m_program.values[op].constant)
          emitPointsToSources(m_program.values[op].initial, q.val_offset,
                              q.val_offset_known, q.val_stride, guard, path,
                              out);
        else
          out.push_back({Expr::makeVal(op, q.val_offset, q.val_offset_known,
                                       q.val_stride),
                         guard, path});
      }
      return;
    }
    if (q.kind == ExprKind::Deref && q.id == inst.result) {
      const PointsToSet resolved = m_program.resolveField(
          inst.object, q.deref_offset, q.deref_offset_known, q.deref_stride);
      if (resolved.isTop()) {
        out.push_back({Expr::makeTop(), guard, path});
      } else {
        for (Id obj : resolved.objects()) {
          const Object &info = m_program.objects[obj];
          if (!isMemory(info) ||
              (q.read_bytes != 0 &&
               (info.bytes == 0 || info.bytes != q.read_bytes)))
            out.push_back({Expr::makeTop(), guard, path});
          else
            out.push_back({Expr::makeMem(obj, q.val_offset, q.val_offset_known,
                                         q.val_stride),
                           guard, path});
        }
      }
      for (Id op : inst.operands)
        out.push_back({Expr::makeDeref(op, q.deref_offset, q.deref_offset_known,
                                       q.deref_stride, q.val_offset,
                                       q.val_offset_known, q.val_stride,
                                       q.read_bytes),
                       guard, path});
      return;
    }
    if (q.kind == ExprKind::Mem &&
        m_program.baseOf(q.id) == m_program.baseOf(inst.object)) {
      const Object &info = m_program.objects[q.id];
      emitPointsToSources(info.initial, q.val_offset, q.val_offset_known,
                          q.val_stride, guard, path, out);
      if (!info.singleton)
        out.push_back({q, guard, path});
      return;
    }
    if (q.kind == ExprKind::Deref) {
      const PointsToSet pts = applyOffset(
          fsciForSummary(q.id, inst.id, target_depth), q.deref_offset,
          q.deref_offset_known, q.deref_stride);
      const Id alloc_base = m_program.baseOf(inst.object);
      bool overlaps = false;
      if (pts.isTop()) {
        overlaps = true;
      } else {
        for (Id o : pts.objects())
          if (m_program.baseOf(o) == alloc_base)
            overlaps = true;
      }
      if (overlaps) {
        std::vector<SummaryEntry> mat;
        materializeDeref(q, inst.id, target_depth, guard, path, mat);
        for (const SummaryEntry &me : mat)
          transferBackward(inst, me.source, me.guard, me.path, target_depth,
                           out);
        return;
      }
    }
    out.push_back({q, guard, path});
    return;
  }

  case Opcode::Copy:
  case Opcode::Join:
    if (q.kind == ExprKind::Val && q.id == inst.result) {
      for (Id op : inst.operands) {
        if (op == INVALID)
          out.push_back({Expr::makeTop(), guard, path});
        else if (m_program.values[op].constant)
          emitPointsToSources(m_program.values[op].initial, q.val_offset,
                              q.val_offset_known, q.val_stride, guard, path,
                              out);
        else
          out.push_back({Expr::makeVal(op, q.val_offset, q.val_offset_known,
                                       q.val_stride),
                         guard, path});
      }
      return;
    }
    if (q.kind == ExprKind::Deref && q.id == inst.result) {
      for (Id op : inst.operands) {
        if (op == INVALID)
          out.push_back({Expr::makeTop(), guard, path});
        else if (m_program.values[op].constant) {
          Expr d = q;
          d.id = op;
          materializeDeref(d, inst.id, target_depth, guard, path, out);
        } else {
          out.push_back({Expr::makeDeref(op, q.deref_offset,
                                         q.deref_offset_known, q.deref_stride,
                                         q.val_offset, q.val_offset_known,
                                         q.val_stride, q.read_bytes),
                         guard, path});
        }
      }
      return;
    }
    out.push_back({q, guard, path});
    return;

  case Opcode::Field:
    if (q.kind == ExprKind::Val && q.id == inst.result) {
      auto [off, kn, stride] = combineOffset(
          q.val_offset, q.val_offset_known, q.val_stride, inst.field_offset,
          inst.field_known, inst.field_array_stride);
      const Id base_val = inst.operands[0];
      if (base_val == INVALID)
        out.push_back({Expr::makeTop(), guard, path});
      else if (m_program.values[base_val].constant)
        emitPointsToSources(m_program.values[base_val].initial, off, kn, stride,
                            guard, path, out);
      else
        out.push_back({Expr::makeVal(base_val, off, kn, stride), guard, path});
      return;
    }
    if (q.kind == ExprKind::Deref && q.id == inst.result) {
      auto [doff, dkn, dstride] = combineOffset(
          q.deref_offset, q.deref_offset_known, q.deref_stride,
          inst.field_offset, inst.field_known, inst.field_array_stride);
      const Id base_val = inst.operands[0];
      if (base_val == INVALID) {
        out.push_back({Expr::makeTop(), guard, path});
      } else if (m_program.values[base_val].constant) {
        Expr d = Expr::makeDeref(base_val, doff, dkn, dstride, q.val_offset,
                                 q.val_offset_known, q.val_stride,
                                 q.read_bytes);
        materializeDeref(d, inst.id, target_depth, guard, path, out);
      } else {
        out.push_back({Expr::makeDeref(base_val, doff, dkn, dstride,
                                       q.val_offset, q.val_offset_known,
                                       q.val_stride, q.read_bytes),
                       guard, path});
      }
      return;
    }
    out.push_back({q, guard, path});
    return;

  case Opcode::Load: {
    if (q.kind == ExprKind::Val && q.id == inst.result) {
      const Id addr = inst.operands[0];
      if (addr == INVALID) {
        out.push_back({Expr::makeTop(), guard, path});
        return;
      }
      if (m_program.values[addr].constant) {
        const PointsToSet pts = m_program.values[addr].initial;
        if (pts.isTop()) {
          out.push_back({Expr::makeTop(), guard, path});
          return;
        }
        for (Id obj : pts.objects()) {
          if (obj == NULL_OBJECT)
            continue;
          const Object &info = m_program.objects[obj];
          if (!isMemory(info) ||
              (inst.read_bytes != 0 &&
               (info.bytes == 0 || info.bytes != inst.read_bytes)))
            out.push_back({Expr::makeTop(), guard, path});
          else
            out.push_back({Expr::makeMem(obj, q.val_offset, q.val_offset_known,
                                         q.val_stride),
                           guard, path});
        }
        return;
      }
      const PointsToSet pts = fsciForSummary(addr, inst.id, target_depth);
      if (pts.empty())
        return;
      if (pts.isTop()) {
        out.push_back({Expr::makeDeref(addr, 0, true, 0, q.val_offset,
                                       q.val_offset_known, q.val_stride,
                                       inst.read_bytes),
                       guard, path});
        return;
      }
      bool has_compatible_mem = false;
      std::size_t compatible_count = 0;
      Id single_obj = INVALID;
      for (Id obj : pts.objects()) {
        if (obj == NULL_OBJECT)
          continue;
        const Object &info = m_program.objects[obj];
        if (!isMemory(info) ||
            (inst.read_bytes != 0 &&
             (info.bytes == 0 || info.bytes != inst.read_bytes))) {
          GuardAtom atom;
          atom.site = inst.id;
          atom.kind = GuardKind::PointsTo;
          atom.lhs = addr;
          atom.rhs = obj;
          GuardId g = conjoinAtom(guard, atom);
          if (g != GUARD_FALSE)
            out.push_back({Expr::makeTop(), g, path});
        } else {
          has_compatible_mem = true;
          ++compatible_count;
          single_obj = obj;
        }
      }
      if (has_compatible_mem) {
        if (compatible_count == 1 && !isSameOrHigherDepth(addr, target_depth) &&
            (pts.size() == 1 ||
             (pts.size() == 2 && pts.contains(NULL_OBJECT)))) {
          out.push_back({Expr::makeMem(single_obj, q.val_offset,
                                       q.val_offset_known, q.val_stride),
                         guard, path});
        } else {
          out.push_back({Expr::makeDeref(addr, 0, true, 0, q.val_offset,
                                         q.val_offset_known, q.val_stride,
                                         inst.read_bytes),
                         guard, path});
        }
      }
      return;
    }
    if (q.kind == ExprKind::Deref && q.id == inst.result) {
      materializeDeref(q, inst.id, target_depth, guard, path, out,
                       Point::After);
      return;
    }
    out.push_back({q, guard, path});
    return;
  }

  case Opcode::Store: {
    // Algorithm 4 lines 10-37: statement `*u = t` (or direct variable store
    // `r = t` when `u` is a constant singleton `&r`).
    const Id u = inst.operands[0];
    const Id t = inst.operands[1];
    if (q.kind == ExprKind::Mem) {
      const PointsToSet pt_u = fsciForSummary(u, inst.id, target_depth);
      if (!pt_u.contains(q.id)) {
        out.push_back({q, guard, path});
        return;
      }
      const Object &info = m_program.objects[q.id];
      const bool same_depth = isSameOrHigherDepth(u, target_depth);
      const bool const_u = (u != INVALID && m_program.values[u].constant);
      const bool must_point = !same_depth && !pt_u.isTop() && pt_u.size() == 1 &&
                              pt_u.contains(q.id) && info.singleton;

      // Branch 1: u -> q.id (Algorithm 4 line 24)
      GuardId taken_g = guard;
      if (!const_u && !must_point) {
        GuardAtom atom;
        atom.site = inst.id;
        atom.kind = GuardKind::PointsTo;
        atom.lhs = u;
        atom.rhs = q.id;
        taken_g = conjoinAtom(guard, atom);
      }
      if (taken_g != GUARD_FALSE) {
        const bool incompatible =
            inst.write_bytes != 0 &&
            (info.bytes == 0 || info.bytes != inst.write_bytes);
        if (incompatible || t == INVALID) {
          out.push_back({Expr::makeTop(), taken_g, path});
        } else if (m_program.values[t].constant) {
          emitPointsToSources(m_program.values[t].initial, q.val_offset,
                              q.val_offset_known, q.val_stride, taken_g, path,
                              out);
        } else {
          out.push_back({Expr::makeVal(t, q.val_offset, q.val_offset_known,
                                       q.val_stride),
                         taken_g, path});
        }
      }

      // Branch 2: u -/-> q.id (Algorithm 4 line 26)
      if (!must_point) {
        GuardId not_taken_g = guard;
        if (!const_u && info.singleton) {
          GuardAtom atom;
          atom.site = inst.id;
          atom.kind = GuardKind::NotPointsTo;
          atom.lhs = u;
          atom.rhs = q.id;
          not_taken_g = conjoinAtom(guard, atom);
        }
        if (not_taken_g != GUARD_FALSE)
          out.push_back({q, not_taken_g, path});
      }
      return;
    }

    if (q.kind == ExprKind::Deref) {
      // Algorithm 4 lines 10-19 (direct constant target `&r`) and lines 28-35
      // (indirect LHS `*u`, tracked `q = *s`).
      const PointsToSet pt_u = fsciForSummary(u, inst.id, target_depth);
      const PointsToSet pt_s =
          applyOffset(fsciForSummary(q.id, inst.id, target_depth),
                      q.deref_offset, q.deref_offset_known, q.deref_stride);
      if (!memoryIntersects(pt_s, pt_u)) {
        out.push_back({q, guard, path});
        return;
      }
      const bool same_depth = isSameOrHigherDepth(u, target_depth) ||
                              isSameOrHigherDepth(q.id, target_depth);
      const bool must_alias =
          !same_depth && !pt_s.isTop() && !pt_u.isTop() && pt_s.size() == 1 &&
          pt_s == pt_u && *pt_s.begin() < m_program.objects.size() &&
          m_program.objects[*pt_s.begin()].singleton;

      // Detect Algorithm 4 lines 10-19 direct-variable assignment `r = t`
      // (constant address `u = &r_obj`) so we emit sharper `s -> r` / `s -/-> r`
      // guards instead of `s ~ u` / `s !~ u`.
      Id direct_target_obj = INVALID;
      if (u != INVALID && m_program.values[u].constant && !pt_u.isTop()) {
        std::size_t mem_cnt = 0;
        for (Id obj : pt_u.objects()) {
          if (obj == NULL_OBJECT)
            continue;
          direct_target_obj = obj;
          ++mem_cnt;
        }
        if (mem_cnt != 1)
          direct_target_obj = INVALID;
      }

      bool any_incompatible = false;
      bool any_compatible = false;
      if (inst.write_bytes == 0) {
        any_compatible = true;
      } else if (pt_s.isTop() || pt_u.isTop()) {
        any_incompatible = true;
        any_compatible = true;
      } else {
        for (Id obj : pt_s.objects()) {
          if (obj == NULL_OBJECT || !pt_u.contains(obj))
            continue;
          const Object &info = m_program.objects[obj];
          if (!isMemory(info) || info.bytes == 0 ||
              info.bytes != inst.write_bytes ||
              (q.read_bytes != 0 && q.read_bytes != inst.write_bytes))
            any_incompatible = true;
          else
            any_compatible = true;
        }
      }

      // Branch 1: s -> r (line 15) or s ~ u (line 32)
      GuardId alias_g = guard;
      if (!must_alias) {
        GuardAtom atom;
        atom.site = inst.id;
        atom.lhs = q.id;
        atom.lhs_offset = q.deref_offset;
        atom.lhs_offset_known = q.deref_offset_known;
        atom.lhs_stride = q.deref_stride;
        if (direct_target_obj != INVALID) {
          atom.kind = GuardKind::PointsTo;
          atom.rhs = direct_target_obj;
        } else {
          atom.kind = GuardKind::Alias;
          atom.rhs = u;
        }
        alias_g = conjoinAtom(guard, atom);
      }
      if (alias_g != GUARD_FALSE) {
        if (any_incompatible || t == INVALID)
          out.push_back({Expr::makeTop(), alias_g, path});
        if (any_compatible && t != INVALID) {
          if (m_program.values[t].constant)
            emitPointsToSources(m_program.values[t].initial, q.val_offset,
                                q.val_offset_known, q.val_stride, alias_g, path,
                                out);
          else
            out.push_back({Expr::makeVal(t, q.val_offset, q.val_offset_known,
                                         q.val_stride),
                           alias_g, path});
        }
      }

      // Branch 2: s -/-> r (line 17) or s !~ u (line 34)
      if (!must_alias) {
        GuardAtom atom;
        atom.site = inst.id;
        atom.lhs = q.id;
        atom.lhs_offset = q.deref_offset;
        atom.lhs_offset_known = q.deref_offset_known;
        atom.lhs_stride = q.deref_stride;
        if (direct_target_obj != INVALID) {
          atom.kind = GuardKind::NotPointsTo;
          atom.rhs = direct_target_obj;
        } else {
          atom.kind = GuardKind::NotAlias;
          atom.rhs = u;
        }
        GuardId not_alias_g = conjoinAtom(guard, atom);
        if (not_alias_g != GUARD_FALSE)
          out.push_back({q, not_alias_g, path});
      }
      return;
    }

    out.push_back({q, guard, path});
    return;
  }

  case Opcode::MemCopy: {
    const Id dst = inst.operands[0];
    const Id src = inst.operands[1];
    if (q.kind == ExprKind::Deref) {
      const PointsToSet pt_dst = fsciForSummary(dst, inst.id, target_depth);
      const PointsToSet pt_s =
          applyOffset(fsciForSummary(q.id, inst.id, target_depth),
                      q.deref_offset, q.deref_offset_known, q.deref_stride);
      bool overlaps = pt_dst.isTop() || pt_s.isTop();
      if (!overlaps) {
        for (Id od : pt_dst.objects())
          for (Id fd : m_program.overlappingFields(od, inst.write_bytes))
            if (pt_s.contains(fd))
              overlaps = true;
      }
      if (!overlaps) {
        out.push_back({q, guard, path});
        return;
      }
      std::vector<SummaryEntry> mat;
      materializeDeref(q, inst.id, target_depth, guard, path, mat);
      for (const SummaryEntry &me : mat)
        transferBackward(inst, me.source, me.guard, me.path, target_depth, out);
      return;
    }
    if (q.kind == ExprKind::Mem) {
      const PointsToSet pt_dst = fsciForSummary(dst, inst.id, target_depth);
      const PointsToSet pt_src = fsciForSummary(src, inst.id, target_depth);
      if (pt_dst.isTop()) {
        out.push_back({Expr::makeTop(), guard, path});
        return;
      }
      bool any_overlap = false;
      Id matching_od = INVALID;
      std::size_t overlap_count = 0;
      for (Id od : pt_dst.objects()) {
        const auto fields = m_program.overlappingFields(od, inst.write_bytes);
        if (std::find(fields.begin(), fields.end(), q.id) != fields.end()) {
          any_overlap = true;
          matching_od = od;
          ++overlap_count;
        }
      }
      if (!any_overlap) {
        out.push_back({q, guard, path});
        return;
      }
      const Object &fd_info = m_program.objects[q.id];
      const bool same_depth = isSameOrHigherDepth(dst, target_depth);
      const bool must_overwrite = !same_depth && pt_dst.size() == 1 &&
                                  overlap_count == 1 && fd_info.singleton;

      for (Id od : pt_dst.objects()) {
        const auto fields = m_program.overlappingFields(od, inst.write_bytes);
        if (std::find(fields.begin(), fields.end(), q.id) == fields.end())
          continue;
        GuardId g = guard;
        if (!m_program.values[dst].constant && !must_overwrite) {
          GuardAtom atom;
          atom.site = inst.id;
          atom.kind = GuardKind::PointsTo;
          atom.lhs = dst;
          atom.rhs = od;
          g = conjoinAtom(guard, atom);
          if (g == GUARD_FALSE)
            continue;
        }
        if (inst.write_bytes == 0 || pt_src.isTop() || fd_info.bytes == 0) {
          out.push_back({Expr::makeTop(), g, path});
          continue;
        }
        const std::int64_t rel =
            static_cast<std::int64_t>(fd_info.offset) -
            static_cast<std::int64_t>(m_program.objects[od].offset);
        for (Id os : pt_src.objects()) {
          if (os == NULL_OBJECT)
            continue;
          GuardId sg = g;
          if (!m_program.values[src].constant && pt_src.size() > 1) {
            GuardAtom satom;
            satom.site = inst.id;
            satom.kind = GuardKind::PointsTo;
            satom.lhs = src;
            satom.rhs = os;
            sg = conjoinAtom(g, satom);
            if (sg == GUARD_FALSE)
              continue;
          }
          const PointsToSet fs_pts = m_program.resolveField(os, rel, true);
          if (fs_pts.isTop() || fs_pts.empty()) {
            out.push_back({Expr::makeTop(), sg, path});
            continue;
          }
          for (Id fs : fs_pts.objects()) {
            const Object &fs_info = m_program.objects[fs];
            if (!isMemory(fs_info) || fs_info.bytes != fd_info.bytes)
              out.push_back({Expr::makeTop(), sg, path});
            else
              out.push_back({Expr::makeMem(fs, q.val_offset, q.val_offset_known,
                                           q.val_stride),
                             sg, path});
          }
        }
      }
      if (!must_overwrite) {
        GuardId ng = guard;
        if (overlap_count == 1 && !m_program.values[dst].constant &&
            fd_info.singleton) {
          GuardAtom atom;
          atom.site = inst.id;
          atom.kind = GuardKind::NotPointsTo;
          atom.lhs = dst;
          atom.rhs = matching_od;
          ng = conjoinAtom(guard, atom);
        }
        if (ng != GUARD_FALSE)
          out.push_back({q, ng, path});
      }
      return;
    }
    out.push_back({q, guard, path});
    return;
  }

  case Opcode::Havoc: {
    if ((q.kind == ExprKind::Val || q.kind == ExprKind::Deref) &&
        q.id == inst.result) {
      out.push_back({Expr::makeTop(), guard, path});
      return;
    }
    if (q.kind == ExprKind::Mem) {
      if (inst.operands.empty()) {
        out.push_back({Expr::makeTop(), guard, path});
        return;
      }
      const Id u = inst.operands[0];
      const PointsToSet pt_u = fsciForSummary(u, inst.id, target_depth);
      if (pt_u.isTop()) {
        out.push_back({Expr::makeTop(), guard, path});
        return;
      }
      bool overlaps = false;
      Id hit_obj = INVALID;
      std::size_t hit_count = 0;
      for (Id o : pt_u.objects()) {
        if (inst.write_bytes != 0) {
          const auto fields = m_program.overlappingFields(o, inst.write_bytes);
          if (std::find(fields.begin(), fields.end(), q.id) != fields.end()) {
            overlaps = true;
            hit_obj = o;
            ++hit_count;
          }
        } else if (o == q.id) {
          overlaps = true;
          hit_obj = o;
          ++hit_count;
        }
      }
      if (!overlaps) {
        out.push_back({q, guard, path});
        return;
      }
      const bool must_hit = !isSameOrHigherDepth(u, target_depth) &&
                            pt_u.size() == 1 && hit_count == 1;
      GuardId tg = guard;
      if (!must_hit && hit_count == 1 && !m_program.values[u].constant) {
        GuardAtom atom;
        atom.site = inst.id;
        atom.kind = GuardKind::PointsTo;
        atom.lhs = u;
        atom.rhs = hit_obj;
        tg = conjoinAtom(guard, atom);
      }
      if (tg != GUARD_FALSE)
        out.push_back({Expr::makeTop(), tg, path});
      if (!must_hit) {
        GuardId ng = guard;
        if (hit_count == 1 && !m_program.values[u].constant &&
            m_program.objects[q.id].singleton) {
          GuardAtom atom;
          atom.site = inst.id;
          atom.kind = GuardKind::NotPointsTo;
          atom.lhs = u;
          atom.rhs = hit_obj;
          ng = conjoinAtom(guard, atom);
        }
        if (ng != GUARD_FALSE)
          out.push_back({q, ng, path});
      }
      return;
    }
    if (q.kind == ExprKind::Deref) {
      if (inst.operands.empty()) {
        out.push_back({Expr::makeTop(), guard, path});
        return;
      }
      const Id u = inst.operands[0];
      const PointsToSet pt_u = fsciForSummary(u, inst.id, target_depth);
      const PointsToSet pt_s =
          applyOffset(fsciForSummary(q.id, inst.id, target_depth),
                      q.deref_offset, q.deref_offset_known, q.deref_stride);
      bool overlaps = pt_u.isTop() || pt_s.isTop();
      if (!overlaps) {
        for (Id o : pt_u.objects()) {
          if (inst.write_bytes != 0) {
            for (Id fo : m_program.overlappingFields(o, inst.write_bytes))
              if (pt_s.contains(fo))
                overlaps = true;
          } else if (pt_s.contains(o)) {
            overlaps = true;
          }
        }
      }
      if (!overlaps) {
        out.push_back({q, guard, path});
        return;
      }
      std::vector<SummaryEntry> mat;
      materializeDeref(q, inst.id, target_depth, guard, path, mat);
      for (const SummaryEntry &me : mat)
        transferBackward(inst, me.source, me.guard, me.path, target_depth, out);
      return;
    }
    out.push_back({q, guard, path});
    return;
  }

  case Opcode::Call:
    out.push_back({q, guard, path});
    return;
  }
}

void GuardedSummarySolver::applyPhiBackward(const Edge &edge, const Expr &q,
                                            GuardId guard,
                                            BddManager::NodeId path,
                                            std::size_t target_depth,
                                            Id pred_site,
                                            std::vector<SummaryEntry> &out) {
  if (q.kind == ExprKind::Val) {
    for (const auto &phi : edge.phi) {
      if (phi.first == q.id) {
        if (phi.second == INVALID)
          out.push_back({Expr::makeTop(), guard, path});
        else if (m_program.values[phi.second].constant)
          emitPointsToSources(m_program.values[phi.second].initial,
                              q.val_offset, q.val_offset_known, q.val_stride,
                              guard, path, out);
        else
          out.push_back({Expr::makeVal(phi.second, q.val_offset,
                                       q.val_offset_known, q.val_stride),
                         guard, path});
        return;
      }
    }
  } else if (q.kind == ExprKind::Deref) {
    for (const auto &phi : edge.phi) {
      if (phi.first == q.id) {
        if (phi.second == INVALID) {
          out.push_back({Expr::makeTop(), guard, path});
        } else if (m_program.values[phi.second].constant) {
          Expr d = q;
          d.id = phi.second;
          materializeDeref(d, pred_site, target_depth, guard, path, out);
        } else {
          out.push_back({Expr::makeDeref(phi.second, q.deref_offset,
                                         q.deref_offset_known, q.deref_stride,
                                         q.val_offset, q.val_offset_known,
                                         q.val_stride, q.read_bytes),
                         guard, path});
        }
        return;
      }
    }
  }
  out.push_back({q, guard, path});
}

} // namespace detail
} // namespace bootstrap
} // namespace lotus
