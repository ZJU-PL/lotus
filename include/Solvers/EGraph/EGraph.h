#pragma once

#include "Solvers/EGraph/Analysis.h"
#include "Solvers/EGraph/EClass.h"
#include "Solvers/EGraph/FlatHashMap.h"
#include "Solvers/EGraph/RecExpr.h"
#include "Solvers/EGraph/Subst.h"
#include "Solvers/EGraph/UnionFind.h"

#include <cassert>
#include <cmath>

#ifndef LOTUS_EGRAPH_ENABLE_JSON
#define LOTUS_EGRAPH_ENABLE_JSON 1
#endif

#if LOTUS_EGRAPH_ENABLE_JSON
#include "Utils/Formats/json11.hpp"
#endif

namespace lotus::egraph {

template <typename L> struct ENodeOrVar;
template <typename L> class Pattern;
template <typename L> using PatternAst = RecExpr<ENodeOrVar<L>>;

struct UnionEvent {
  Id left;
  Id right;
  Justification justification = Justification::congruence();
};

struct ExplanationConnection {
  Justification justification = Justification::congruence();
  bool is_rewrite_forward = false;
  Id next = Id::fromIndex(0);
  Id current = Id::fromIndex(0);
};

struct ExplanationNode {
  std::vector<ExplanationConnection> neighbors;
  ExplanationConnection parent_connection;
};

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct LanguageMapper;

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct SimpleLanguageMapper;

// Compact flat row inside an operator-partitioned table.
struct OpRow {
  static constexpr Id kDeadClass = Id(UINT32_MAX);

  Id eclass = kDeadClass;
  uint32_t node_idx = 0;
  uint32_t version = 0;
  Id c0{};
  Id c1{};
  Id c2{};

  bool isDead() const noexcept { return eclass == kDeadClass; }
};

struct OpCsrRange {
  uint32_t begin = 0;
  uint32_t end = 0;

  bool empty() const noexcept { return begin == end; }
  uint32_t size() const noexcept { return end - begin; }
};

struct OpCsrIndex {
  uint64_t built_generation = 0;
  bool is_dense = false;
  std::vector<OpCsrRange> dense_ranges;
  FlatHashMap<Id, OpCsrRange> sparse_ranges;
  std::vector<OpRow> entries;

  OpCsrRange lookup(Id id) const noexcept {
    if (is_dense) {
      size_t idx = id.index();
      return idx < dense_ranges.size() ? dense_ranges[idx] : OpCsrRange{};
    }
    auto it = sparse_ranges.find(id);
    return it != sparse_ranges.end() ? it->second : OpCsrRange{};
  }
};

struct OpTable {
  uint32_t op_id = 0;
  uint32_t dead_count = 0;
  uint64_t mutation_generation = 1;
  uint32_t live_count = 0;
  bool use_binary_memo = false;
  BinaryMemoTable binary_memo;
  PodVec<OpRow> rows;
  std::vector<uint32_t> version_offsets;

  mutable OpCsrIndex by_eclass;
  mutable llvm::SmallVector<OpCsrIndex, 3> by_col;
  mutable bool by_eclass_sorted_c0 = false;

  uint32_t deltaStart(uint32_t since_version) const noexcept {
    if (since_version == 0) {
      return 0;
    }
    if (since_version < version_offsets.size()) {
      return version_offsets[since_version];
    }
    return static_cast<uint32_t>(rows.size());
  }
};

template <typename L, typename AnalysisT> class EGraph {
public:
  using Analysis = AnalysisT;
  using Data = typename AnalysisT::Data;
  using Class = EClass<L, Data>;
  static constexpr uint32_t kInvalidOpId = UINT32_MAX;
  static constexpr bool kIsNoAnalysis = std::is_same_v<AnalysisT, NoAnalysis<L>>;

  struct EnodeMeta {
    static constexpr uint32_t kDeadBit = 0x80000000u;
    static constexpr uint32_t kBinaryBit = 0x40000000u;
    static constexpr uint32_t kOpMask = 0x3FFFFFFFu;

    Id owner{};
    uint32_t op_flags = 0;
    uint32_t op_row = 0;
    uint32_t next_in_cls = UINT32_MAX;

    EnodeMeta() = default;
    EnodeMeta(Id own, uint32_t flags, uint32_t orow, uint32_t next) noexcept
        : owner(own), op_flags(flags), op_row(orow), next_in_cls(next) {}

    bool isDead() const noexcept { return (op_flags & kDeadBit) != 0; }
    void setDead() noexcept { op_flags |= kDeadBit; }
    bool isBinary() const noexcept { return (op_flags & kBinaryBit) != 0; }
    uint32_t opId() const noexcept { return op_flags & kOpMask; }
  };
  using EnodeRecord = EnodeMeta;

  struct GeneralEnode {
    uint64_t hash;
    L node;

    GeneralEnode(uint64_t h, L n) : hash(h), node(std::move(n)) {}
  };

  struct OpPayload {
    std::optional<L> binary_template;
    std::vector<GeneralEnode> general_nodes;
  };

  struct OrigEntry {
    static constexpr uint32_t kGeneralBit = 0x80000000u;
    static constexpr uint32_t kIndexMask = 0x7FFFFFFFu;

    uint32_t op_or_general = 0;
    Id c0{};
    Id c1{};
  };

  struct ParentEdge {
    uint32_t nidx;
    uint32_t next;
  };

  struct ParentMeta {
    uint32_t parent_head = UINT32_MAX;
    uint32_t parent_count = 0;
  };

  struct ClassMeta {
    static constexpr uint32_t kNil = UINT32_MAX;
    static constexpr uint32_t kAliveBit = 0x80000000u;
    static constexpr uint32_t kCountMask = 0x7FFFFFFFu;

    uint32_t node_head = kNil;
    uint32_t node_count_alive = 0;

    bool isAlive() const noexcept { return (node_count_alive & kAliveBit) != 0; }
    uint32_t nodeCount() const noexcept {
      return node_count_alive & kCountMask;
    }
    void setAlive(uint32_t count) noexcept {
      node_count_alive = kAliveBit | count;
    }
    void setDead() noexcept { node_count_alive = 0; }
    void incNodes() noexcept { ++node_count_alive; }
    void addNodes(uint32_t delta) noexcept { node_count_alive += delta; }
  };

  EGraph() = default;
  explicit EGraph(AnalysisT analysis) : analysis_(std::move(analysis)) {}

  AnalysisT &analysis() { return analysis_; }
  const AnalysisT &analysis() const { return analysis_; }

  uint32_t version() const noexcept { return completed_version_; }
  size_t idSpaceSize() const noexcept { return union_find_.size(); }

  std::vector<std::reference_wrapper<const Class>> classes() const {
    assertClean();
    const auto &ids = classIds();
    std::vector<std::reference_wrapper<const Class>> out;
    out.reserve(ids.size());
    for (Id id : ids) {
      out.emplace_back(classAtConst(id));
    }
    return out;
  }

  std::vector<std::reference_wrapper<Class>> classesMut() {
    std::vector<std::reference_wrapper<Class>> out;
    out.reserve(class_count_);
    for (size_t i = 0; i < class_meta_.size(); ++i) {
      if (class_meta_[i].isAlive()) {
        out.emplace_back(classAtMut(Id::fromIndex(i)));
      }
    }
    return out;
  }

  const std::vector<Id> &
  classesForOp(const typename L::Discriminant &op) const {
    assertClean();
    ensureClassesByOp();
    auto it = classes_by_op_.find(op);
    if (it == classes_by_op_.end()) {
      static const std::vector<Id> empty;
      return empty;
    }
    return it->second;
  }

  uint32_t findOpId(const typename L::Discriminant &op) const noexcept {
    auto it = op_ids_.find(op);
    return it != op_ids_.end() ? it->second : kInvalidOpId;
  }

  const OpTable &opTable(uint32_t op_id) const noexcept {
    return op_tables_[op_id];
  }

  // Returns const ref for non-binary e-nodes (used by RelationalQuery when
  // !exact_match, which is never a binary-memo op).
  const L &enodeAt(uint32_t node_idx) const noexcept {
    const auto &m = enode_meta_[node_idx];
    return op_payloads_[m.opId()].general_nodes[m.op_row].node;
  }

  L materializeEnode(uint32_t node_idx) const {
    const auto &m = enode_meta_[node_idx];
    const uint32_t op_id = m.opId();
    if (m.isBinary()) {
      const OpRow &row = op_tables_[op_id].rows[m.op_row];
      L node = *op_payloads_[op_id].binary_template;
      auto &&ch = node.childrenMut();
      ch[0] = row.c0;
      ch[1] = row.c1;
      return node;
    }
    return op_payloads_[op_id].general_nodes[m.op_row].node;
  }

  template <typename Fn>
  void forEachEnodeChild(uint32_t node_idx, Fn &&fn) const {
    const auto &m = enode_meta_[node_idx];
    const uint32_t op_id = m.opId();
    if (m.isBinary()) {
      const OpRow &row = op_tables_[op_id].rows[m.op_row];
      fn(row.c0);
      fn(row.c1);
      return;
    }
    for (Id c : op_payloads_[op_id].general_nodes[m.op_row].node.children()) {
      fn(c);
    }
  }

  Id enodeChild(const OpRow &row, size_t col) const noexcept {
    if (col == 0) {
      return row.c0;
    }
    if (col == 1) {
      return row.c1;
    }
    if (col == 2) {
      return row.c2;
    }
    const auto &m = enode_meta_[row.node_idx];
    return op_payloads_[m.opId()].general_nodes[m.op_row].node.children()[col];
  }

  // Zero-allocation direct traversal of live canonical nodes in class `canon`
  // (used by Extractor to avoid materializing EClass views!).
  template <typename Fn> void forEachClassNode(Id canon, Fn &&fn) const {
    const auto &cm = class_meta_[canon.index()];
    for (uint32_t nidx = cm.node_head; nidx != ClassMeta::kNil;
         nidx = enode_meta_[nidx].next_in_cls) {
      const auto &m = enode_meta_[nidx];
      if (m.isDead()) {
        continue;
      }
      const uint32_t op_id = m.opId();
      if (m.isBinary()) {
        const OpRow &row = op_tables_[op_id].rows[m.op_row];
        L node = *op_payloads_[op_id].binary_template;
        auto &&ch = node.childrenMut();
        ch[0] = row.c0;
        ch[1] = row.c1;
        fn(nidx, static_cast<const L &>(node));
      } else {
        fn(nidx, op_payloads_[op_id].general_nodes[m.op_row].node);
      }
    }
  }

  template <typename Fn> void forEachParentClass(Id canon, Fn &&fn) const {
    ensureParentEdgesMaterialized();
    const size_t idx = canon.index();
    if (idx >= parent_meta_.size()) {
      return;
    }
    for (uint32_t e = parent_meta_[idx].parent_head; e != ClassMeta::kNil;
         e = parent_edge_pool_[e].next) {
      const auto &m = enode_meta_[parent_edge_pool_[e].nidx];
      if (!m.isDead()) {
        fn(find(m.owner));
      }
    }
  }

  template <typename Fn> void forEachParentEnode(Id canon, Fn &&fn) const {
    ensureParentEdgesMaterialized();
    const size_t idx = canon.index();
    if (idx >= parent_meta_.size()) {
      return;
    }
    const bool is_clean = clean_;
    for (uint32_t e = parent_meta_[idx].parent_head; e != ClassMeta::kNil;
         e = parent_edge_pool_[e].next) {
      const uint32_t nidx = parent_edge_pool_[e].nidx;
      const auto &m = enode_meta_[nidx];
      if (!m.isDead()) {
        fn(nidx, is_clean ? m.owner : find(m.owner));
      }
    }
  }

  template <typename Fn> void forEachLeafEnode(Fn &&fn) const {
    const bool is_clean = clean_;
    for (uint32_t op_id = 0; op_id < op_tables_.size(); ++op_id) {
      const OpTable &tbl = op_tables_[op_id];
      if (tbl.live_count == 0 || tbl.use_binary_memo) {
        continue;
      }
      const auto &gen = op_payloads_[op_id].general_nodes;
      const size_t nrows = tbl.rows.size();
      for (size_t r = 0; r < nrows; ++r) {
        const OpRow &row = tbl.rows[r];
        if (row.isDead()) {
          continue;
        }
        const L &node = gen[r].node;
        if (node.children().empty()) {
          fn(row.node_idx, is_clean ? row.eclass : find(row.eclass), node);
        }
      }
    }
  }

  template <typename Fn> void visitEnode(uint32_t nidx, Fn &&fn) const {
    const auto &m = enode_meta_[nidx];
    const uint32_t op_id = m.opId();
    if (m.isBinary()) {
      const OpRow &row = op_tables_[op_id].rows[m.op_row];
      L node = *op_payloads_[op_id].binary_template;
      auto &&ch = node.childrenMut();
      ch[0] = row.c0;
      ch[1] = row.c1;
      fn(static_cast<const L &>(node));
    } else {
      fn(op_payloads_[op_id].general_nodes[m.op_row].node);
    }
  }

  bool enodeIsBinary(uint32_t nidx) const noexcept {
    return enode_meta_[nidx].isBinary();
  }

  std::pair<Id, Id> binaryEnodeChildren(uint32_t nidx) const noexcept {
    const auto &m = enode_meta_[nidx];
    const OpRow &row = op_tables_[m.opId()].rows[m.op_row];
    return {row.c0, row.c1};
  }

  void prepareParentEdgesForExtraction() const {
    ensureParentEdgesMaterialized();
  }

  const OpCsrIndex &opByEClass(uint32_t op_id) const {
    const OpTable &table = op_tables_[op_id];
    if (table.by_eclass.built_generation != table.mutation_generation) {
      buildCsrIndex(table, table.by_eclass, /*col=*/-1, /*sort_c0=*/false);
      table.by_eclass_sorted_c0 = false;
    }
    return table.by_eclass;
  }

  const OpCsrIndex &opByEClassSortedC0(uint32_t op_id) const {
    const OpTable &table = op_tables_[op_id];
    if (table.by_eclass.built_generation != table.mutation_generation) {
      buildCsrIndex(table, table.by_eclass, /*col=*/-1, /*sort_c0=*/true);
      table.by_eclass_sorted_c0 = true;
    } else if (!table.by_eclass_sorted_c0) {
      sortCsrByC0(table.by_eclass);
      table.by_eclass_sorted_c0 = true;
    }
    return table.by_eclass;
  }

  const OpCsrIndex &opByCol(uint32_t op_id, size_t col) const {
    const OpTable &table = op_tables_[op_id];
    if (table.by_col.size() <= col) {
      table.by_col.resize(col + 1);
    }
    OpCsrIndex &csr = table.by_col[col];
    if (csr.built_generation != table.mutation_generation) {
      buildCsrIndex(table, csr, static_cast<int>(col), /*sort_c0=*/false);
    }
    return csr;
  }

  const std::vector<L> &nodes() const {
    ensureNodesMaterialized();
    return nodes_;
  }
  const L &originalNode(Id id) const {
    ensureNodesMaterialized();
    return nodes_.at(id.index());
  }

  bool empty() const noexcept { return total_memo_entries_ == 0; }
  bool clean() const noexcept { return clean_; }
  size_t totalSize() const noexcept { return live_node_count_; }
  size_t memoSize() const noexcept { return total_memo_entries_; }
  size_t totalNumberOfNodes() const noexcept { return live_node_count_; }
  size_t numberOfClasses() const noexcept { return class_count_; }

  uint32_t getOrCreateOpIdForNode(const L &sample) {
    return getOrCreateOpId(sample.discriminant(), &sample);
  }

  bool opUsesBinaryMemo(uint32_t op_id) const noexcept {
    return op_id < op_tables_.size() && op_tables_[op_id].use_binary_memo;
  }

  inline void prefetchBinary(uint32_t op_id, uint64_t h) const noexcept {
    op_tables_[op_id].binary_memo.prefetch(h);
  }

  inline void prefetchUnionFind(Id id) const noexcept {
    union_find_.prefetch(id);
  }

  inline void prefetchClassMeta(Id id) const noexcept {
    __builtin_prefetch(&class_meta_[id.index()], 1, 1);
  }

  void reserveForIteration(llvm::ArrayRef<size_t> op_extra, size_t total_nodes,
                           size_t total_classes, size_t node_limit) {
    if (total_nodes < 2048 || live_node_count_ >= node_limit) {
      return;
    }
    const size_t budget = node_limit - live_node_count_;
    const size_t est_nodes = std::min(budget, (total_nodes * 3) >> 2);
    const size_t est_classes = std::min(budget, (total_classes * 3) >> 2);

    for (uint32_t op_id = 0; op_id < op_extra.size() && op_id < op_tables_.size();
         ++op_id) {
      if (op_extra[op_id] < 1024 || !op_tables_[op_id].use_binary_memo) {
        continue;
      }
      auto &table = op_tables_[op_id];
      const size_t est_op = std::min(budget, (op_extra[op_id] * 2) / 3);
      table.binary_memo.reserve(table.binary_memo.size() + est_op);
      table.rows.reserveAdditional(est_op);
    }

    enode_meta_.reserveAdditional(est_nodes);
    if (est_classes > 0) {
      union_find_.reserveAdditional(est_classes);
      class_meta_.reserveAdditional(est_classes);
      active_class_positions_.reserveAdditional(est_classes);
      orig_entries_.reserveAdditional(est_classes);
      if (active_class_ids_.size() + est_classes > active_class_ids_.capacity()) {
        active_class_ids_.reserve(std::max(active_class_ids_.capacity() * 2,
                                           active_class_ids_.size() +
                                               est_classes));
      }
    }
  }

  void reserveBinaryBatch(uint32_t op_id, size_t extra, bool creates_classes) {
    if (extra < 1024) {
      return;
    }
    auto &table = op_tables_[op_id];
    table.binary_memo.reserve(table.binary_memo.size() + (extra >> 1));
    table.rows.reserveAdditional(extra);
    enode_meta_.reserveAdditional(extra);
    if (creates_classes) {
      const size_t cls_extra = extra >> 1;
      union_find_.reserveAdditional(cls_extra);
      class_meta_.reserveAdditional(cls_extra);
      active_class_positions_.reserveAdditional(cls_extra);
      if (active_class_ids_.size() + cls_extra > active_class_ids_.capacity()) {
        active_class_ids_.reserve(std::max(active_class_ids_.capacity() * 2,
                                           active_class_ids_.size() + cls_extra));
      }
      orig_entries_.reserveAdditional(cls_extra);
    }
  }

  // Hoisted batch executor for intermediate binary RHS steps (`bn <= 32`):
  // ONE capacity check, ZERO per-item markOpDirty/counter flushes, straight-line
  // unchecked stores into compact POD arenas!
  void applyBinaryStepBatch(uint32_t op_id, size_t bn, const Id *bc0,
                            const Id *bc1, const uint64_t *bkey,
                            const uint64_t *bh, Id *out_ids) {
    clean_ = false;
    OpTable &table = op_tables_[op_id];
    table.binary_memo.ensureSpace(bn);
    table.rows.reserveAdditional(bn);
    enode_meta_.reserveAdditional(bn);
    orig_entries_.reserveAdditional(bn);
    class_meta_.reserveAdditional(bn);
    active_class_positions_.reserveAdditional(bn);
    if (active_class_ids_.size() + bn > active_class_ids_.capacity()) {
      active_class_ids_.reserve(
          std::max(active_class_ids_.capacity() * 2,
                   active_class_ids_.size() + bn));
    }
    union_find_.reserveAdditional(bn);

    const uint32_t op_flags = op_id | EnodeMeta::kBinaryBit;
    const uint32_t cur_epoch = epoch_;
    size_t added = 0;

    for (size_t b = 0; b < bn; ++b) {
      uint32_t new_nidx = 0;
      Id new_id{};
      auto [slot, inserted] = table.binary_memo.findOrInsertUnchecked(
          bkey[b], bh[b], [&]() -> std::pair<uint32_t, Id> {
            new_id = union_find_.makeSetUnchecked();
            new_nidx = static_cast<uint32_t>(enode_meta_.size());
            return {new_nidx, new_id};
          });
      if (!inserted) {
        Id own = findMut(slot->owner);
        slot->owner = own;
        out_ids[b] = own;
        continue;
      }
      const Id c0 = bc0[b];
      const Id c1 = bc1[b];
      orig_entries_.pushBackUnchecked(OrigEntry{op_id, c0, c1});
      const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
      enode_meta_.pushBackUnchecked(
          EnodeMeta(new_id, op_flags, row_idx, ClassMeta::kNil));
      ClassMeta cm;
      cm.node_head = new_nidx;
      cm.setAlive(1);
      class_meta_.pushBackUnchecked(cm);
      active_class_positions_.pushBackUnchecked(
          static_cast<uint32_t>(active_class_ids_.size()));
      active_class_ids_.push_back(new_id);
      table.rows.pushBackUnchecked(
          OpRow{new_id, new_nidx, cur_epoch, c0, c1, Id(0)});
      ++added;
      out_ids[b] = new_id;
    }

    if (added > 0) {
      total_memo_entries_ += added;
      class_count_ += added;
      live_node_count_ += added;
      table.live_count += static_cast<uint32_t>(added);
      ++table.mutation_generation;
      markOpDirty(op_id);
      ++view_generation_;
    }
  }

  // Hoisted batch executor for fused binary RHS-root step (`bn <= 32`):
  size_t applyBinaryRootBatch(uint32_t op_id, size_t bn, const Id *lhs_base,
                              uint32_t stride, const Id *bc0, const Id *bc1,
                              const uint64_t *bkey, const uint64_t *bh) {
    clean_ = false;
    OpTable &table = op_tables_[op_id];
    table.binary_memo.ensureSpace(bn);
    table.rows.reserveAdditional(bn);
    enode_meta_.reserveAdditional(bn);

    const uint32_t op_flags = op_id | EnodeMeta::kBinaryBit;
    const uint32_t cur_epoch = epoch_;
    const size_t initial_classes = class_count_;
    size_t added = 0;
    size_t applied = 0;

    for (size_t b = 0; b < bn; ++b) {
      Id c0 = bc0[b];
      Id c1 = bc1[b];
      uint64_t key = bkey[b];
      uint64_t h = bh[b];
      if (class_count_ != initial_classes) {
        Id nc0 = findMut(c0);
        Id nc1 = findMut(c1);
        if (nc0 != c0 || nc1 != c1) {
          c0 = nc0;
          c1 = nc1;
          key = BinaryMemoTable::packKey(c0, c1);
          h = fastMix64(key);
        }
      }
      Id target = findMut(lhs_base[b * stride]);
      uint32_t new_nidx = 0;
      auto [slot, inserted] = table.binary_memo.findOrInsertUnchecked(
          key, h, [&]() -> std::pair<uint32_t, Id> {
            new_nidx = static_cast<uint32_t>(enode_meta_.size());
            return {new_nidx, target};
          });
      if (!inserted) {
        Id existing_owner = findMut(slot->owner);
        slot->owner = existing_owner;
        if (existing_owner == target) {
          continue;
        }
        auto [merged, did] = uniteDistinctFast(target, existing_owner);
        slot->owner = merged;
        if (did) {
          ++applied;
        }
        continue;
      }

      auto &cm = class_meta_[target.index()];
      const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
      enode_meta_.pushBackUnchecked(
          EnodeMeta(target, op_flags, row_idx, cm.node_head));
      cm.node_head = new_nidx;
      cm.incNodes();
      table.rows.pushBackUnchecked(
          OpRow{target, new_nidx, cur_epoch, c0, c1, Id(0)});
      ++added;
      ++applied;
    }

    if (added > 0) {
      total_memo_entries_ += added;
      live_node_count_ += added;
      table.live_count += static_cast<uint32_t>(added);
      ++table.mutation_generation;
      markOpDirty(op_id);
      ++view_generation_;
    }
    return applied;
  }

  // Ultra-fast binary e-node add for pre-canonicalized (c0, c1) and pre-hashed
  // key/h. Zero L construction, zero switch dispatches, zero parent RMWs!
  Id addBinaryPrehashed(uint32_t op_id, Id c0, Id c1, uint64_t key,
                        uint64_t h) {
    clean_ = false;
    OpTable &table = op_tables_[op_id];
    uint32_t new_nidx = 0;
    Id new_id{};
    auto [slot, inserted] = table.binary_memo.findOrInsert(
        key, h, [&]() -> std::pair<uint32_t, Id> {
          new_id = union_find_.makeSet();
          new_nidx = static_cast<uint32_t>(enode_meta_.size());
          return {new_nidx, new_id};
        });
    if (!inserted) {
      Id own = findMut(slot->owner);
      slot->owner = own;
      return own;
    }
    ++total_memo_entries_;

    orig_entries_.push_back(OrigEntry{op_id, c0, c1});
    const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
    enode_meta_.emplace_back(new_id, op_id | EnodeMeta::kBinaryBit, row_idx,
                             ClassMeta::kNil);

    ClassMeta cm;
    cm.node_head = new_nidx;
    cm.setAlive(1);
    class_meta_.push_back(cm);
    active_class_positions_.push_back(
        static_cast<uint32_t>(active_class_ids_.size()));
    active_class_ids_.push_back(new_id);
    ++class_count_;
    ++live_node_count_;

    table.rows.push_back(OpRow{new_id, new_nidx, epoch_, c0, c1, Id(0)});
    ++table.live_count;
    ++table.mutation_generation;
    markOpDirty(op_id);
    ++view_generation_;
    return new_id;
  }

  // Ultra-fast fused binary RHS-root add + unite for pre-canonicalized (c0, c1)
  // and pre-hashed key/h! Zero L construction, zero temporary e-class, zero
  // parent-edge RMWs!
  bool addAndUniteBinaryPrehashed(Id target_class, uint32_t op_id, Id c0, Id c1,
                                  uint64_t key, uint64_t h) {
    clean_ = false;
    Id target = findMut(target_class);
    OpTable &table = op_tables_[op_id];
    uint32_t new_nidx = 0;
    auto [slot, inserted] = table.binary_memo.findOrInsert(
        key, h, [&]() -> std::pair<uint32_t, Id> {
          new_nidx = static_cast<uint32_t>(enode_meta_.size());
          return {new_nidx, target};
        });
    if (!inserted) {
      Id existing_owner = findMut(slot->owner);
      slot->owner = existing_owner;
      if (existing_owner == target) {
        return false;
      }
      auto [merged, did] = uniteDistinctFast(target, existing_owner);
      slot->owner = merged;
      return did;
    }
    ++total_memo_entries_;

    auto &cm = class_meta_[target.index()];
    const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
    enode_meta_.emplace_back(target, op_id | EnodeMeta::kBinaryBit, row_idx,
                             cm.node_head);
    cm.node_head = new_nidx;
    cm.incNodes();
    ++live_node_count_;

    table.rows.push_back(OpRow{target, new_nidx, epoch_, c0, c1, Id(0)});
    ++table.live_count;
    ++table.mutation_generation;
    markOpDirty(op_id);
    ++view_generation_;
    return true;
  }

  Id find(Id id) const noexcept { return union_find_.find(id); }
  Id findMut(Id id) noexcept { return union_find_.findMut(id); }

  Class &operator[](Id id) { return classAtMut(findMut(id)); }
  const Class &operator[](Id id) const { return classAtConst(find(id)); }

  const std::vector<Id> &classIds() const {
    assertClean();
    if (class_ids_dirty_) {
      class_ids_ = active_class_ids_;
      std::sort(class_ids_.begin(), class_ids_.end());
      class_ids_dirty_ = false;
    }
    return class_ids_;
  }

  const std::vector<Id> &activeClassIds() const noexcept {
    return active_class_ids_;
  }

  EGraph withExplanationsEnabled() const {
    if (explanations_enabled_) {
      return *this;
    }
    if (!nodes_.empty() || !orig_entries_.empty()) {
      throw std::runtime_error("Need to set explanations enabled before adding "
                               "any expressions to the egraph");
    }
    EGraph copy = *this;
    copy.explanations_enabled_ = true;
    copy.optimize_explanation_lengths_ = true;
    return copy;
  }

  EGraph withExplanationsDisabled() const {
    EGraph copy = *this;
    copy.explanations_enabled_ = false;
    copy.union_events_.clear();
    copy.explanation_nodes_.clear();
    copy.uncanonical_memo_.clear();
    copy.original_node_ids_.clear();
    copy.enode_orig_ids_.clear();
    if constexpr (kIsNoAnalysis) {
      if (copy.orig_entries_.size() < copy.nodes_.size()) {
        copy.orig_entries_.reserve(copy.nodes_.size());
        for (size_t i = copy.orig_entries_.size(); i < copy.nodes_.size(); ++i) {
          uint32_t gidx = static_cast<uint32_t>(copy.general_orig_nodes_.size());
          copy.general_orig_nodes_.push_back(copy.nodes_[i]);
          copy.orig_entries_.push_back(
              OrigEntry{OrigEntry::kGeneralBit | gidx, Id(0), Id(0)});
        }
      }
    }
    return copy;
  }

  EGraph withoutExplanations() const { return withExplanationsDisabled(); }

  EGraph withoutExplanationLengthOptimization() const {
    if (!explanations_enabled_) {
      throw std::runtime_error("Need to set explanations enabled before "
                               "setting length optimization");
    }
    EGraph copy = *this;
    copy.optimize_explanation_lengths_ = false;
    return copy;
  }

  EGraph withExplanationLengthOptimization() const {
    if (!explanations_enabled_) {
      throw std::runtime_error("Need to set explanations enabled before "
                               "setting length optimization");
    }
    EGraph copy = *this;
    copy.optimize_explanation_lengths_ = true;
    return copy;
  }

  bool areExplanationsEnabled() const noexcept { return explanations_enabled_; }
  bool optimizeExplanationLengths() const noexcept {
    return optimize_explanation_lengths_;
  }
  const std::vector<UnionEvent> &unionEvents() const { return union_events_; }
  const std::vector<ExplanationNode> &explanationNodes() const {
    return explanation_nodes_;
  }

  std::vector<std::tuple<Id, Id, Symbol>> getUnionEqualities() const {
    if (!explanations_enabled_) {
      throw std::runtime_error(
          "Use withExplanationsEnabled before requesting union equalities");
    }
    std::vector<std::tuple<Id, Id, Symbol>> out;
    out.reserve(union_events_.size());
    for (const auto &event : union_events_) {
      if (event.justification.isRule()) {
        out.emplace_back(event.left, event.right, event.justification.rule);
      }
    }
    return out;
  }

  std::optional<Id> lookupInternal(L node) const {
    canonicalizeInPlace(node);
    return memoLookup(node);
  }

  EGraph copyWithoutUnions(AnalysisT analysis) const {
    if (!explanations_enabled_) {
      throw std::runtime_error("Use withExplanationsEnabled before copying an "
                               "e-graph without unions");
    }
    ensureNodesMaterialized();
    ensureNodesMaterialized();
    EGraph copy(std::move(analysis));
    std::vector<Id> ids;
    ids.reserve(nodes_.size());
    for (const auto &node : nodes_) {
      ids.push_back(copy.add(
          node.mapChildren([&](Id child) { return ids.at(child.index()); })));
    }
    return copy;
  }

  EGraph egraphIntersect(const EGraph &other, AnalysisT analysis) const {
    if (!clean_ || !other.clean_) {
      EGraph lhs = *this;
      EGraph rhs = other;
      if (!lhs.clean_) {
        lhs.rebuild();
      }
      if (!rhs.clean_) {
        rhs.rebuild();
      }
      return lhs.egraphIntersect(rhs, std::move(analysis));
    }

    ensureClassesByOp();
    other.ensureClassesByOp();

    FlatHashMap<std::pair<Id, Id>, Id, PairHash> product_map;
    FlatHashMap<std::pair<Id, Id>, uint8_t, PairHash> visited_pairs;
    std::vector<std::pair<L, Id>> enodes;

    for (const auto &[discriminant, left_ids] : classes_by_op_) {
      auto right_it = other.classes_by_op_.find(discriminant);
      if (right_it == other.classes_by_op_.end()) {
        continue;
      }
      for (Id left : left_ids) {
        for (Id right : right_it->second) {
          if (visited_pairs.try_emplace(std::make_pair(left, right), 1)
                  .second) {
            intersectClasses(other, enodes, left, right, product_map);
          }
        }
      }
    }

    return fromEnodes(std::move(enodes), std::move(analysis),
                      UnresolvedPolicy::Drop);
  }

  void egraphUnion(const EGraph &other) {
    if (!clean_) {
      rebuild();
    }
    if (!other.clean_) {
      EGraph copy = other;
      copy.rebuild();
      egraphUnion(copy);
      return;
    }

    for (const auto &[left, right, why] : other.getUnionEqualities()) {
      auto [left_pat, left_subst] = other.idToPattern(left, {});
      auto [right_pat, right_subst] = other.idToPattern(right, {});
      if (!left_subst.bindings().empty() || !right_subst.bindings().empty()) {
        throw std::runtime_error(
            "Unexpected variables while replaying egraph union");
      }
      unionInstantiations(left_pat.ast(), right_pat.ast(), Subst{}, why);
    }
    rebuild();
  }

  // Fused RHS-root insertion: inserts `canonical` directly into `target_class`
  // WITHOUT creating & immediately destroying a temporary e-class!
  // Only valid for NoAnalysis with explanations disabled.
  bool addAndUniteFast(Id target_class, L canonical, uint32_t &cached_op_id) {
    clean_ = false;
    if (cached_op_id == kInvalidOpId) {
      cached_op_id = getOrCreateOpId(canonical.discriminant(), &canonical);
    }
    if (op_tables_[cached_op_id].use_binary_memo) {
      const auto &ch = canonical.children();
      Id c0 = findMut(ch[0]);
      Id c1 = findMut(ch[1]);
      uint64_t key = BinaryMemoTable::packKey(c0, c1);
      return addAndUniteBinaryPrehashed(target_class, cached_op_id, c0, c1, key,
                                        fastMix64(key));
    }

    Id target = findMut(target_class);
    canonicalizeInPlaceMut(canonical);
    const uint64_t h = hashNode(canonical);
    const uint32_t op_id = cached_op_id;
    OpTable &table = op_tables_[op_id];
    auto &gen_nodes = op_payloads_[op_id].general_nodes;

    uint32_t new_nidx = 0;
    auto [idx, inserted] = memo_index_.findOrInsert(
        h,
        [&](uint32_t i) {
          const auto &m = enode_meta_[i];
          const auto &g = op_payloads_[m.opId()].general_nodes[m.op_row];
          return g.hash == h && g.node == canonical;
        },
        [&]() {
          new_nidx = static_cast<uint32_t>(enode_meta_.size());
          const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
          auto &cm = class_meta_[target.index()];
          enode_meta_.emplace_back(target, op_id, row_idx, cm.node_head);
          table.rows.push_back(makeOpRow(target, new_nidx, epoch_, canonical));
          gen_nodes.emplace_back(h, canonical);
          return new_nidx;
        },
        [&](uint32_t i) {
          const auto &m = enode_meta_[i];
          return op_payloads_[m.opId()].general_nodes[m.op_row].hash;
        });

    if (!inserted) {
      Id existing_owner = findMut(enode_meta_[idx].owner);
      if (existing_owner == target) {
        return false;
      }
      return uniteDistinctFast(target, existing_owner).second;
    }
    ++total_memo_entries_;

    auto &cm = class_meta_[target.index()];
    cm.node_head = new_nidx;
    cm.incNodes();
    ++live_node_count_;

    ++table.live_count;
    ++table.mutation_generation;
    markOpDirty(op_id);
    ++view_generation_;
    return true;
  }

  Id addCanonicalFast(L canonical) {
    uint32_t tmp_op = kInvalidOpId;
    return addCanonicalWithOpId(std::move(canonical), tmp_op);
  }

  Id addCanonicalWithOpId(L canonical, uint32_t &cached_op_id) {
    clean_ = false;
    if (cached_op_id == kInvalidOpId) {
      cached_op_id = getOrCreateOpId(canonical.discriminant(), &canonical);
    }
    if constexpr (kIsNoAnalysis) {
      if (!explanations_enabled_ && op_tables_[cached_op_id].use_binary_memo) {
        const auto &ch = canonical.children();
        Id c0 = findMut(ch[0]);
        Id c1 = findMut(ch[1]);
        uint64_t key = BinaryMemoTable::packKey(c0, c1);
        return addBinaryPrehashed(cached_op_id, c0, c1, key, fastMix64(key));
      }
    }

    canonicalizeInPlaceMut(canonical);
    const uint64_t h = hashNode(canonical);
    const uint32_t op_id = cached_op_id;
    OpTable &table = op_tables_[op_id];
    auto &gen_nodes = op_payloads_[op_id].general_nodes;

    uint32_t new_nidx = 0;
    Id new_id{};
    auto [idx, inserted] = memo_index_.findOrInsert(
        h,
        [&](uint32_t i) {
          const auto &m = enode_meta_[i];
          const auto &g = op_payloads_[m.opId()].general_nodes[m.op_row];
          return g.hash == h && g.node == canonical;
        },
        [&]() {
          new_id = union_find_.makeSet();
          new_nidx = static_cast<uint32_t>(enode_meta_.size());
          const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
          enode_meta_.emplace_back(new_id, op_id, row_idx, ClassMeta::kNil);
          table.rows.push_back(makeOpRow(new_id, new_nidx, epoch_, canonical));
          gen_nodes.emplace_back(h, canonical);
          return new_nidx;
        },
        [&](uint32_t i) {
          const auto &m = enode_meta_[i];
          return op_payloads_[m.opId()].general_nodes[m.op_row].hash;
        });

    if (!inserted) {
      return findMut(enode_meta_[idx].owner);
    }
    ++total_memo_entries_;

    finishNewEClassWithOp(new_id, new_nidx, canonical, op_id);
    return findMut(new_id);
  }

  Id add(const L &node) {
    if (!explanations_enabled_) {
      return addCanonicalFast(node);
    }
    return find(addUncanonical(node));
  }

  Id addUncanonical(const L &node) {
    clean_ = false;
    uint32_t op_id = getOrCreateOpId(node.discriminant(), &node);
    if (!explanations_enabled_) {
      if constexpr (kIsNoAnalysis) {
        if (op_tables_[op_id].use_binary_memo) {
          const auto &ch = node.children();
          Id c0 = findMut(ch[0]);
          Id c1 = findMut(ch[1]);
          uint64_t key = BinaryMemoTable::packKey(c0, c1);
          return addBinaryPrehashed(op_id, c0, c1, key, fastMix64(key));
        }
      }
    }

    L canonical = node;
    canonicalizeInPlaceMut(canonical);
    const uint64_t h = hashNode(canonical);
    OpTable &table = op_tables_[op_id];
    auto &gen_nodes = op_payloads_[op_id].general_nodes;

    uint32_t new_nidx = 0;
    Id new_id{};
    auto [idx, inserted] = memo_index_.findOrInsert(
        h,
        [&](uint32_t i) {
          const auto &m = enode_meta_[i];
          const auto &g = op_payloads_[m.opId()].general_nodes[m.op_row];
          return g.hash == h && g.node == canonical;
        },
        [&]() {
          new_id = union_find_.makeSet();
          new_nidx = static_cast<uint32_t>(enode_meta_.size());
          const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
          enode_meta_.emplace_back(new_id, op_id, row_idx, ClassMeta::kNil);
          table.rows.push_back(makeOpRow(new_id, new_nidx, epoch_, canonical));
          gen_nodes.emplace_back(h, canonical);
          if (explanations_enabled_) {
            enode_orig_ids_.push_back(new_id);
          }
          return new_nidx;
        },
        [&](uint32_t i) {
          const auto &m = enode_meta_[i];
          return op_payloads_[m.opId()].general_nodes[m.op_row].hash;
        });

    if (!inserted) {
      if (!explanations_enabled_) {
        return enode_meta_[idx].owner;
      }

      Id existing = enode_orig_ids_[idx];
      auto uncanon_it = uncanonical_memo_.find(node);
      if (uncanon_it != uncanonical_memo_.end()) {
        return uncanon_it->second;
      }

      Id dup_id = union_find_.makeSet();
      ensureNodeStorage(dup_id, node);
      if (class_meta_.size() <= dup_id.index()) {
        class_meta_.resize(dup_id.index() + 1);
        active_class_positions_.resize(dup_id.index() + 1,
                                       std::numeric_limits<uint32_t>::max());
      }
      uncanonical_memo_.try_emplace(node, dup_id);
      union_find_.unite(findMut(existing), dup_id);
      recordExplanation(existing, dup_id, Justification::congruence());
      recordUnion(existing, dup_id, Justification::congruence());
      return dup_id;
    }
    ++total_memo_entries_;

    finishNewEClassWithOp(new_id, new_nidx, node, op_id);
    return new_id;
  }

  Id addExpr(const RecExpr<L> &expr) {
    if (!explanations_enabled_) {
      llvm::SmallVector<Id, 32> ids;
      ids.reserve(expr.size());
      for (const auto &node : expr.items()) {
        ids.push_back(addCanonicalFast(
            node.mapChildren([&](Id id) { return ids[id.index()]; })));
      }
      return ids.back();
    }
    return find(addExprUncanonical(expr));
  }

  Id addExprUncanonical(const RecExpr<L> &expr) {
    std::vector<Id> ids;
    ids.reserve(expr.size());
    for (const auto &node : expr.items()) {
      ids.push_back(addUncanonical(
          node.mapChildren([&](Id id) { return ids[id.index()]; })));
    }
    return ids.back();
  }

  std::optional<Id> lookup(const L &node) const {
    assertClean();
    L canonical = node;
    canonicalizeInPlace(canonical);
    auto found = memoLookup(canonical);
    if (!found) {
      return std::nullopt;
    }
    return find(*found);
  }

  std::optional<Id> lookupCanonical(const L &canonical) const noexcept {
    uint32_t op_id = findOpId(canonical.discriminant());
    if (op_id == kInvalidOpId) {
      return std::nullopt;
    }
    const OpTable &table = op_tables_[op_id];
    if (table.use_binary_memo) {
      const auto &ch = canonical.children();
      uint64_t key = BinaryMemoTable::packKey(ch[0], ch[1]);
      const auto *slot = table.binary_memo.find(key, fastMix64(key));
      return slot ? std::optional<Id>(find(slot->owner)) : std::nullopt;
    }
    const uint64_t h = hashNode(canonical);
    const uint32_t idx = memo_index_.find(h, [&](uint32_t i) {
      const auto &m = enode_meta_[i];
      const auto &g = op_payloads_[m.opId()].general_nodes[m.op_row];
      return g.hash == h && g.node == canonical;
    });
    if (idx == CompactHashIndex::kInvalid) {
      return std::nullopt;
    }
    return find(enode_meta_[idx].owner);
  }

  std::optional<Id> lookupExpr(const RecExpr<L> &expr) const {
    auto ids = lookupExprIds(expr);
    if (!ids || ids->empty()) {
      return std::nullopt;
    }
    return ids->back();
  }

  std::optional<std::vector<Id>> lookupExprIds(const RecExpr<L> &expr) const {
    assertClean();
    std::vector<Id> ids;
    ids.reserve(expr.size());
    for (const auto &node : expr.items()) {
      L mapped = node.mapChildren([&](Id id) { return ids[id.index()]; });
      auto found = lookup(mapped);
      if (!found) {
        return std::nullopt;
      }
      ids.push_back(*found);
    }
    return ids;
  }

  std::optional<std::vector<Id>>
  lookupExprUncanonicalIds(const RecExpr<L> &expr) const {
    assertClean();
    std::vector<Id> ids;
    ids.reserve(expr.size());

    for (const auto &node : expr.items()) {
      L materialized = node.mapChildren([&](Id id) { return ids[id.index()]; });

      std::optional<Id> exact;
      if (explanations_enabled_) {
        auto it = uncanonical_memo_.find(materialized);
        if (it != uncanonical_memo_.end()) {
          exact = it->second;
        }
      }

      if (!exact && explanations_enabled_) {
        auto it = original_node_ids_.find(materialized);
        if (it != original_node_ids_.end()) {
          exact = it->second;
        }
      }

      if (exact) {
        ids.push_back(*exact);
        continue;
      }

      auto canonical = lookup(materialized);
      if (!canonical) {
        return std::nullopt;
      }
      ids.push_back(*canonical);
    }

    return ids;
  }

  Id addInstantiation(const PatternAst<L> &pat, const Subst &subst);
  Id addInstantiationUncanonical(const PatternAst<L> &pat, const Subst &subst) {
    return addInstantiationNoncanonical(pat, subst);
  }

  Id unite(Id lhs, Id rhs, const Symbol &reason = {}) {
    if constexpr (kIsNoAnalysis) {
      if (!explanations_enabled_) {
        return uniteFast(lhs, rhs).first;
      }
    }
    std::optional<Justification> justification;
    if (reason != Symbol()) {
      justification = Justification::ruleJustification(reason);
    }
    auto [id, _] = uniteImpl(lhs, rhs, justification);
    return id;
  }

  std::pair<Id, bool> uniteChecked(Id lhs, Id rhs, const Symbol &reason = {}) {
    if constexpr (kIsNoAnalysis) {
      if (!explanations_enabled_) {
        return uniteFast(lhs, rhs);
      }
    }
    std::optional<Justification> justification;
    if (reason != Symbol()) {
      justification = Justification::ruleJustification(reason);
    }
    return uniteImpl(lhs, rhs, justification);
  }

  std::pair<Id, bool> uniteFast(Id lhs, Id rhs) {
    clean_ = false;
    Id lhs_class = findMut(lhs);
    Id rhs_class = findMut(rhs);
    if (lhs_class == rhs_class) {
      return {lhs_class, false};
    }
    return uniteDistinctFast(lhs_class, rhs_class);
  }

  std::pair<Id, bool> unionInstantiations(const PatternAst<L> &from_pat,
                                          const PatternAst<L> &to_pat,
                                          const Subst &subst,
                                          const Symbol &reason);

  bool unionTrusted(Id from, Id to, const Symbol &reason) {
    return uniteImpl(from, to, Justification::ruleJustification(reason)).second;
  }

  std::vector<Id> equivs(const RecExpr<L> &expr1,
                         const RecExpr<L> &expr2) const {
    assertClean();
    Pattern<L> pat1(expr1);
    Pattern<L> pat2(expr2);
    auto matches1 = pat1.search(*this);
    auto matches2 = pat2.search(*this);

    FlatHashMap<Id, uint8_t> rhs_classes;
    rhs_classes.reserve(matches2.size());
    for (const auto &rhs : matches2) {
      rhs_classes.try_emplace(find(rhs.eclass), 1);
    }
    std::vector<Id> out;
    for (const auto &lhs : matches1) {
      if (rhs_classes.count(find(lhs.eclass))) {
        out.push_back(lhs.eclass);
      }
    }
    return out;
  }

  bool equivalent(const RecExpr<L> &expr1, const RecExpr<L> &expr2) const {
    return !equivs(expr1, expr2).empty();
  }

#if LOTUS_EGRAPH_ENABLE_JSON
  json11::Json toJson() const {
    if (!clean_) {
      EGraph copy = *this;
      copy.rebuild();
      return copy.toJson();
    }

    ensureNodesMaterialized();
    json11::Json::array nodes_json;
    nodes_json.reserve(nodes_.size());
    for (const auto &node : nodes_) {
      nodes_json.emplace_back(nodeToJson(node));
    }

    json11::Json::array classes_json;
    auto class_refs = classes();
    classes_json.reserve(class_refs.size());
    for (const auto &class_ref : class_refs) {
      const auto &klass = class_ref.get();
      json11::Json::array class_nodes;
      class_nodes.reserve(klass.nodes.size());
      for (const auto &node : klass.nodes) {
        class_nodes.emplace_back(nodeToJson(node));
      }

      llvm::SmallVector<Id, 4> canonical_parents;
      canonical_parents.reserve(klass.parents.size());
      for (Id parent : klass.parents) {
        canonical_parents.push_back(find(parent));
      }
      std::sort(canonical_parents.begin(), canonical_parents.end());
      canonical_parents.erase(
          std::unique(canonical_parents.begin(), canonical_parents.end()),
          canonical_parents.end());

      json11::Json::array parents_json;
      parents_json.reserve(canonical_parents.size());
      for (Id parent : canonical_parents) {
        parents_json.emplace_back(static_cast<double>(parent.value()));
      }

      classes_json.emplace_back(json11::Json::object{
          {"id", static_cast<double>(klass.id.value())},
          {"nodes", std::move(class_nodes)},
          {"parents", std::move(parents_json)},
      });
    }

    json11::Json::object root{
        {"nodes", std::move(nodes_json)},
        {"classes", std::move(classes_json)},
        {"memo_size", static_cast<double>(total_memo_entries_)},
        {"clean", clean_},
        {"explanations_enabled", explanations_enabled_},
    };
    return root;
  }

  static EGraph fromJson(const json11::Json &json,
                         AnalysisT analysis = AnalysisT()) {
    if (!json.is_object()) {
      throw std::runtime_error("EGraph JSON must be an object");
    }

    const auto &obj = json.object_items();
    auto classes_it = obj.find("classes");
    if (classes_it == obj.end() || !classes_it->second.is_array()) {
      throw std::runtime_error("EGraph JSON missing classes array");
    }

    std::vector<std::pair<L, Id>> enodes;
    for (const auto &class_json : classes_it->second.array_items()) {
      if (!class_json.is_object()) {
        throw std::runtime_error("EClass JSON must be an object");
      }
      const auto &class_obj = class_json.object_items();
      auto id_it = class_obj.find("id");
      auto nodes_it = class_obj.find("nodes");
      if (id_it == class_obj.end() || nodes_it == class_obj.end() ||
          !id_it->second.is_number() || !nodes_it->second.is_array()) {
        throw std::runtime_error("EClass JSON missing id or nodes");
      }
      Id class_id = idFromJson(id_it->second, "EClass id");
      for (const auto &node_json : nodes_it->second.array_items()) {
        enodes.emplace_back(nodeFromJson(node_json), class_id);
      }
    }

    return fromEnodes(std::move(enodes), std::move(analysis),
                      UnresolvedPolicy::Reject);
  }

  static EGraph parseJson(std::string_view text,
                          AnalysisT analysis = AnalysisT()) {
    std::string error;
    auto json = json11::Json::parse(std::string(text), error);
    if (!error.empty()) {
      throw std::runtime_error("Failed to parse EGraph JSON: " + error);
    }
    return fromJson(json, std::move(analysis));
  }
#endif

  RecExpr<L> idToExpr(Id id) const {
    if (explanations_enabled_) {
      return originalExpr(id);
    }

    FlatHashMap<Id, size_t> choices;
    FlatHashMap<Id, uint8_t> visiting;
    Id canonical = find(id);
    if (!chooseFiniteNode(canonical, choices, visiting)) {
      throw std::runtime_error(
          "E-class has no finite expression representative");
    }

    FlatHashMap<Id, Id> cache;
    RecExpr<L> expr;
    materializeFiniteExpr(expr, canonical, choices, cache);
    return expr;
  }

  RecExpr<L> originalExpr(Id id) const {
    FlatHashMap<Id, Id> cache;
    RecExpr<L> expr;
    originalExprInternal(expr, id, cache);
    return expr;
  }

  RecExpr<L> idToExprFlat(Id id) const {
    RecExpr<L> expr;
    const auto &node = classAtConst(find(id)).nodes.front();
    expr.add(node);
    return expr;
  }

  std::pair<Pattern<L>, Subst>
  idToPattern(Id id, const std::unordered_map<Id, Id> &substitutions) const;

  size_t getNumCongr() const {
    if (!explanations_enabled_) {
      throw std::runtime_error(
          "Use withExplanationsEnabled before requesting explanations");
    }
    FlatHashMap<std::pair<Id, Id>, uint8_t, PairHash> edges;
    for (size_t i = 0; i < explanation_nodes_.size(); ++i) {
      for (const auto &neighbor : explanation_nodes_[i].neighbors) {
        if (!neighbor.justification.isCongruence()) {
          continue;
        }
        Id a = Id::fromIndex(i);
        Id b = neighbor.next;
        if (b < a) {
          std::swap(a, b);
        }
        edges.try_emplace(std::make_pair(a, b), 1);
      }
    }
    return edges.size();
  }

  size_t getExplanationNumNodes() const {
    if (!explanations_enabled_) {
      throw std::runtime_error(
          "Use withExplanationsEnabled before requesting explanations");
    }
    return explanation_nodes_.size();
  }

  void setAnalysisData(Id id, Data data) {
    Id canonical = findMut(id);
    auto &klass = classAtMut(canonical);
    klass.data = std::move(data);
    forEachParentClass(canonical, [&](Id p) { analysis_pending_.insert(p); });
    analysis_.modify(*this, canonical);
  }

  size_t rebuild() {
    size_t unions = processPending();
    finalizeOpTables();
    completed_version_ = epoch_;
    ++epoch_;
    class_ids_dirty_ = true;
    classes_by_op_dirty_ = true;
    ++view_generation_;
#ifndef NDEBUG
    assertCongruenceInvariant();
#endif
    clean_ = true;
    return unions;
  }

private:
#if LOTUS_EGRAPH_ENABLE_JSON
  static json11::Json nodeToJson(const L &node) {
    json11::Json::array children;
    children.reserve(node.children().size());
    for (Id child : node.children()) {
      children.emplace_back(static_cast<double>(child.value()));
    }
    json11::Json::object object{{"op", displayNode(node)},
                                {"children", std::move(children)}};
    if (auto variant = LanguageSerializationOps<L>::variantName(node)) {
      object.emplace("variant", std::string(*variant));
    }
    return object;
  }

  static L nodeFromJson(const json11::Json &json) {
    if (!json.is_object()) {
      throw std::runtime_error("Node JSON must be an object");
    }
    const auto &obj = json.object_items();
    auto op_it = obj.find("op");
    auto children_it = obj.find("children");
    if (op_it == obj.end() || children_it == obj.end() ||
        !op_it->second.is_string() || !children_it->second.is_array()) {
      throw std::runtime_error("Node JSON missing op or children");
    }

    std::vector<Id> children;
    children.reserve(children_it->second.array_items().size());
    for (const auto &child_json : children_it->second.array_items()) {
      if (!child_json.is_number()) {
        throw std::runtime_error("Node child must be numeric");
      }
      children.push_back(idFromJson(child_json, "Node child"));
    }

    std::optional<L> node;
    auto variant_it = obj.find("variant");
    if (variant_it != obj.end()) {
      if (!variant_it->second.is_string()) {
        throw std::runtime_error("Node variant must be a string");
      }
      node = LanguageSerializationOps<L>::fromVariant(
          variant_it->second.string_value(), op_it->second.string_value(),
          children);
    } else {
      node = LanguageOps<L>::fromOp(op_it->second.string_value(), children);
    }
    if (!node) {
      throw std::runtime_error("Failed to decode node from JSON");
    }
    return *node;
  }

  static Id idFromJson(const json11::Json &json, std::string_view field) {
    if (!json.is_number()) {
      throw std::runtime_error(std::string(field) + " must be numeric");
    }

    double value = json.number_value();
    constexpr double max_id =
        static_cast<double>(std::numeric_limits<uint32_t>::max());
    if (!std::isfinite(value) || value < 0.0 || std::floor(value) != value ||
        value > max_id) {
      throw std::runtime_error(std::string(field) + " is outside the Id range");
    }
    return Id(static_cast<uint32_t>(value));
  }
#endif

  template <typename SrcL, typename SrcA, typename DstL, typename DstA>
  friend struct LanguageMapper;

  static uint64_t hashNode(const L &node) noexcept {
    return fastMix64(static_cast<uint64_t>(LanguageHash<L>{}(node)));
  }

  void assertClean() const {
    if (!clean_) {
      throw std::runtime_error(
          "Tried to query a dirty e-graph; call rebuild() first");
    }
  }

  bool hasClass(Id id) const noexcept {
    return id.index() < class_meta_.size() && class_meta_[id.index()].isAlive();
  }

  void ensureClassView(Id canon) const {
    const size_t idx = canon.index();
    if constexpr (kIsNoAnalysis) {
      const size_t n = union_find_.size();
      if (classes_.size() < n) {
        classes_.resize(n);
        class_view_gen_.resize(n, 0);
      }
      if (classes_[idx].has_value() &&
          class_view_gen_[idx] == view_generation_) {
        return;
      }
      ensureParentEdgesMaterialized();
      if (!classes_[idx].has_value()) {
        classes_[idx].emplace();
      }
      Class &klass = *classes_[idx];
      klass.id = canon;
      klass.nodes.clear();
      const auto &cm = class_meta_[idx];
      klass.nodes.reserve(cm.nodeCount());
      forEachClassNode(canon, [&](uint32_t, const L &node) {
        klass.nodes.push_back(node);
      });
      if (klass.nodes.size() > 1) {
        std::sort(klass.nodes.begin(), klass.nodes.end());
      }
      klass.parents.clear();
      if (idx < parent_meta_.size()) {
        const auto &pm = parent_meta_[idx];
        klass.parents.reserve(pm.parent_count);
        for (uint32_t e = pm.parent_head; e != ClassMeta::kNil;
             e = parent_edge_pool_[e].next) {
          const auto &m = enode_meta_[parent_edge_pool_[e].nidx];
          if (!m.isDead()) {
            klass.parents.push_back(find(m.owner));
          }
        }
      }
      if (klass.parents.size() > 1) {
        std::sort(klass.parents.begin(), klass.parents.end());
        klass.parents.erase(
            std::unique(klass.parents.begin(), klass.parents.end()),
            klass.parents.end());
      }
      klass.rebuildMatchingIndex();
      klass.nodes_dirty = false;
      class_view_gen_[idx] = view_generation_;
    }
  }

  Class &classAtMut(Id id) {
    if constexpr (kIsNoAnalysis) {
      ensureClassView(id);
    }
    return *classes_[id.index()];
  }

  const Class &classAtConst(Id id) const {
    if constexpr (kIsNoAnalysis) {
      ensureClassView(id);
    }
    return *classes_[id.index()];
  }

  uint32_t getOrCreateOpId(const typename L::Discriminant &disc,
                           const L *sample = nullptr) {
    auto it = op_ids_.find(disc);
    if (it != op_ids_.end()) {
      return it->second;
    }
    uint32_t id = static_cast<uint32_t>(op_tables_.size());
    op_ids_.try_emplace(disc, id);
    OpTable table;
    table.op_id = id;
    OpPayload payload;
    if constexpr (kIsNoAnalysis) {
      if (!explanations_enabled_ && sample != nullptr &&
          sample->children().size() == 2 && discriminantImpliesMatch(*sample)) {
        table.use_binary_memo = true;
        payload.binary_template.emplace(*sample);
      }
    }
    op_tables_.push_back(std::move(table));
    op_payloads_.push_back(std::move(payload));
    op_discriminants_.push_back(disc);
    op_dirty_marks_.push_back(0);
    return id;
  }

  static OpRow makeOpRow(Id owner, uint32_t nidx, uint32_t ver,
                         const L &node) noexcept {
    OpRow row;
    row.eclass = owner;
    row.node_idx = nidx;
    row.version = ver;
    const auto &ch = node.children();
    const size_t n = ch.size();
    if (n >= 1) {
      row.c0 = ch[0];
    }
    if (n >= 2) {
      row.c1 = ch[1];
    }
    if (n >= 3) {
      row.c2 = ch[2];
    }
    return row;
  }

  inline void pushParentEdge(Id child, uint32_t nidx) const {
    const size_t idx = child.index();
    if (parent_meta_.size() <= idx) {
      parent_meta_.resize(std::max(union_find_.size(), idx + 1));
    }
    const uint32_t e = static_cast<uint32_t>(parent_edge_pool_.size());
    auto &pm = parent_meta_[idx];
    parent_edge_pool_.push_back(ParentEdge{nidx, pm.parent_head});
    pm.parent_head = e;
    ++pm.parent_count;
  }

  inline void registerParentEdges(uint32_t nidx, const L &canonical) {
    const auto &ch = canonical.children();
    const size_t arity = ch.size();
    if (arity == 0) {
      return;
    }
    if (arity == 1) {
      Id c0 = findMut(ch[0]);
      pushParentEdge(c0, nidx);
      if constexpr (!kIsNoAnalysis) {
        classes_[c0.index()]->parents.push_back(enode_meta_[nidx].owner);
      }
      return;
    }
    if (arity == 2) {
      Id c0 = findMut(ch[0]);
      Id c1 = findMut(ch[1]);
      pushParentEdge(c0, nidx);
      if constexpr (!kIsNoAnalysis) {
        classes_[c0.index()]->parents.push_back(enode_meta_[nidx].owner);
      }
      if (c1 != c0) {
        pushParentEdge(c1, nidx);
        if constexpr (!kIsNoAnalysis) {
          classes_[c1.index()]->parents.push_back(enode_meta_[nidx].owner);
        }
      }
      return;
    }
    llvm::SmallVector<Id, 8> seen;
    seen.reserve(arity);
    for (Id child : ch) {
      Id c = findMut(child);
      if (std::find(seen.begin(), seen.end(), c) == seen.end()) {
        seen.push_back(c);
        pushParentEdge(c, nidx);
        if constexpr (!kIsNoAnalysis) {
          classes_[c.index()]->parents.push_back(enode_meta_[nidx].owner);
        }
      }
    }
  }

  void ensureParentEdgesMaterialized(
      std::vector<uint32_t> *dirty_out = nullptr) const {
    const uint32_t total = static_cast<uint32_t>(enode_meta_.size());
    if (parent_indexed_until_ >= total) {
      return;
    }
    const size_t n_classes = union_find_.size();
    if (parent_meta_.size() < n_classes) {
      parent_meta_.resize(n_classes);
    }
    for (uint32_t nidx = parent_indexed_until_; nidx < total; ++nidx) {
      const auto &m = enode_meta_[nidx];
      if (m.isDead()) {
        continue;
      }
      const uint32_t op_id = m.opId();
      if (m.isBinary()) {
        const OpRow &row = op_tables_[op_id].rows[m.op_row];
        Id c0 = union_find_.find(row.c0);
        Id c1 = union_find_.find(row.c1);
        pushParentEdge(c0, nidx);
        if (c1 != c0) {
          pushParentEdge(c1, nidx);
        }
        if (dirty_out && (c0 != row.c0 || c1 != row.c1)) {
          dirty_out->push_back(nidx);
        }
      } else {
        const auto &ch = op_payloads_[op_id].general_nodes[m.op_row].node.children();
        const size_t arity = ch.size();
        bool dirty = false;
        if (arity == 1) {
          Id c0 = union_find_.find(ch[0]);
          pushParentEdge(c0, nidx);
          dirty = (c0 != ch[0]);
        } else if (arity == 2) {
          Id c0 = union_find_.find(ch[0]);
          Id c1 = union_find_.find(ch[1]);
          pushParentEdge(c0, nidx);
          if (c1 != c0) {
            pushParentEdge(c1, nidx);
          }
          dirty = (c0 != ch[0] || c1 != ch[1]);
        } else if (arity > 2) {
          llvm::SmallVector<Id, 8> seen;
          seen.reserve(arity);
          for (Id child : ch) {
            Id c = union_find_.find(child);
            if (c != child) {
              dirty = true;
            }
            if (std::find(seen.begin(), seen.end(), c) == seen.end()) {
              seen.push_back(c);
              pushParentEdge(c, nidx);
            }
          }
        }
        if (dirty_out && dirty) {
          dirty_out->push_back(nidx);
        }
      }
    }
    parent_indexed_until_ = total;
  }

  void markClassDirty(Id id) {
    if constexpr (!kIsNoAnalysis) {
      size_t idx = id.index();
      if (class_dirty_marks_.size() <= idx) {
        class_dirty_marks_.resize(idx + 1, 0);
      }
      if (!class_dirty_marks_[idx]) {
        class_dirty_marks_[idx] = 1;
        dirty_classes_.push_back(id);
      }
    }
  }

  void markParentsDirty(Id id) {
    if constexpr (!kIsNoAnalysis) {
      size_t idx = id.index();
      if (parents_dirty_marks_.size() <= idx) {
        parents_dirty_marks_.resize(idx + 1, 0);
      }
      if (!parents_dirty_marks_[idx]) {
        parents_dirty_marks_[idx] = 1;
        dirty_parents_classes_.push_back(id);
      }
    }
  }

  void markOpDirty(uint32_t op_id) {
    if (!op_dirty_marks_[op_id]) {
      op_dirty_marks_[op_id] = 1;
      dirty_ops_.push_back(op_id);
    }
  }

  void ensureClassesByOp() const {
    if (!classes_by_op_dirty_) {
      return;
    }
    classes_by_op_.clear();
    classes_by_op_.reserve(op_tables_.size());
    for (uint32_t op_id = 0; op_id < op_tables_.size(); ++op_id) {
      const OpTable &table = op_tables_[op_id];
      if (table.live_count == 0) {
        continue;
      }
      std::vector<Id> ids;
      ids.reserve(table.live_count);
      for (const OpRow &row : table.rows) {
        if (!row.isDead()) {
          ids.push_back(row.eclass);
        }
      }
      if (ids.empty()) {
        continue;
      }
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
      classes_by_op_.try_emplace(op_discriminants_[op_id], std::move(ids));
    }
    classes_by_op_dirty_ = false;
  }

  static void sortCsrByC0(OpCsrIndex &csr) {
    if (csr.is_dense) {
      for (const auto r : csr.dense_ranges) {
        if (r.end - r.begin > 8) {
          std::sort(csr.entries.begin() + r.begin, csr.entries.begin() + r.end,
                    [](const OpRow &a, const OpRow &b) {
                      return a.c0.value() < b.c0.value();
                    });
        }
      }
    } else {
      for (const auto &[_, r] : csr.sparse_ranges) {
        if (r.end - r.begin > 8) {
          std::sort(csr.entries.begin() + r.begin, csr.entries.begin() + r.end,
                    [](const OpRow &a, const OpRow &b) {
                      return a.c0.value() < b.c0.value();
                    });
        }
      }
    }
  }

  void buildCsrIndex(const OpTable &table, OpCsrIndex &csr, int col,
                     bool sort_c0) const {
    const size_t id_space = union_find_.size();
    const size_t live = table.live_count;
    csr.built_generation = table.mutation_generation;
    csr.entries.clear();
    if (live == 0) {
      csr.is_dense = false;
      csr.dense_ranges.clear();
      csr.sparse_ranges.clear();
      return;
    }

    auto keyOf = [&](const OpRow &row) noexcept -> Id {
      if (col < 0) {
        return row.eclass;
      }
      return enodeChild(row, static_cast<size_t>(col));
    };

    bool use_dense = (id_space <= 4096) || (live * 16 >= id_space);
    csr.is_dense = use_dense;

    if (use_dense) {
      csr.sparse_ranges.clear();
      csr.dense_ranges.assign(id_space, OpCsrRange{0, 0});
      for (const OpRow &row : table.rows) {
        if (!row.isDead()) {
          ++csr.dense_ranges[keyOf(row).index()].end;
        }
      }
      uint32_t offset = 0;
      for (size_t i = 0; i < id_space; ++i) {
        uint32_t cnt = csr.dense_ranges[i].end;
        csr.dense_ranges[i].begin = offset;
        csr.dense_ranges[i].end = offset;
        offset += cnt;
      }
      csr.entries.resize(offset);
      for (const OpRow &row : table.rows) {
        if (!row.isDead()) {
          uint32_t pos = csr.dense_ranges[keyOf(row).index()].end++;
          csr.entries[pos] = row;
        }
      }
      if (sort_c0) {
        for (size_t i = 0; i < id_space; ++i) {
          const auto r = csr.dense_ranges[i];
          if (r.end - r.begin > 8) {
            std::sort(csr.entries.begin() + r.begin,
                      csr.entries.begin() + r.end,
                      [](const OpRow &a, const OpRow &b) {
                        return a.c0.value() < b.c0.value();
                      });
          }
        }
      }
    } else {
      csr.dense_ranges.clear();
      csr.sparse_ranges.clear();
      csr.sparse_ranges.reserve(live);
      for (const OpRow &row : table.rows) {
        if (!row.isDead()) {
          ++csr.sparse_ranges[keyOf(row)].end;
        }
      }
      uint32_t offset = 0;
      for (auto &[_, range] : csr.sparse_ranges) {
        uint32_t cnt = range.end;
        range.begin = offset;
        range.end = offset;
        offset += cnt;
      }
      csr.entries.resize(offset);
      for (const OpRow &row : table.rows) {
        if (!row.isDead()) {
          uint32_t pos = csr.sparse_ranges.find(keyOf(row))->second.end++;
          csr.entries[pos] = row;
        }
      }
      if (sort_c0) {
        for (const auto &[_, r] : csr.sparse_ranges) {
          if (r.end - r.begin > 8) {
            std::sort(csr.entries.begin() + r.begin,
                      csr.entries.begin() + r.end,
                      [](const OpRow &a, const OpRow &b) {
                        return a.c0.value() < b.c0.value();
                      });
          }
        }
      }
    }
  }

  void ensureNodesMaterialized() const {
    if constexpr (kIsNoAnalysis) {
      if (explanations_enabled_ || orig_entries_.size() <= nodes_.size()) {
        return;
      }
      nodes_.reserve(orig_entries_.size());
      for (size_t i = nodes_.size(); i < orig_entries_.size(); ++i) {
        const auto &e = orig_entries_[i];
        if (e.op_or_general & OrigEntry::kGeneralBit) {
          nodes_.push_back(
              general_orig_nodes_[e.op_or_general & OrigEntry::kIndexMask]);
        } else {
          L node = *op_payloads_[e.op_or_general].binary_template;
          auto &&ch = node.childrenMut();
          ch[0] = e.c0;
          ch[1] = e.c1;
          nodes_.push_back(std::move(node));
        }
      }
    }
  }

  void ensureNodeStorage(Id id, const L &node) {
    if constexpr (kIsNoAnalysis) {
      if (!explanations_enabled_) {
        if (orig_entries_.size() != id.index()) {
          throw std::runtime_error("EGraph node storage out of sync");
        }
        uint32_t gidx = static_cast<uint32_t>(general_orig_nodes_.size());
        general_orig_nodes_.push_back(node);
        orig_entries_.push_back(
            OrigEntry{OrigEntry::kGeneralBit | gidx, Id(0), Id(0)});
        return;
      }
    }
    if (nodes_.size() != id.index()) {
      throw std::runtime_error("EGraph node storage out of sync");
    }
    nodes_.push_back(node);
    if (explanations_enabled_) {
      original_node_ids_.try_emplace(node, id);
      if (explanation_nodes_.size() != id.index()) {
        throw std::runtime_error("EGraph explanation node storage out of sync");
      }
      explanation_nodes_.push_back(ExplanationNode{
          {},
          ExplanationConnection{Justification::congruence(), false, id, id}});
    }
  }

  void finishNewEClassWithOp(Id id, uint32_t nidx, const L &original,
                             uint32_t op_id) {
    ensureNodeStorage(id, original);

    Data data{};
    if constexpr (!kIsNoAnalysis) {
      data = AnalysisT::make(*this, original, id);
    }

    const size_t idx = id.index();
    if (class_meta_.size() <= idx) {
      class_meta_.resize(idx + 1);
      active_class_positions_.resize(idx + 1,
                                     std::numeric_limits<uint32_t>::max());
    }

    auto &cm = class_meta_[idx];
    cm.node_head = nidx;
    cm.setAlive(1);

    const auto &m = enode_meta_[nidx];
    const L &canon_node = op_payloads_[op_id].general_nodes[m.op_row].node;

    if constexpr (!kIsNoAnalysis) {
      if (classes_.size() <= idx) {
        classes_.resize(idx + 1);
      }
      classes_[idx].emplace();
      auto &klass = *classes_[idx];
      klass.id = id;
      klass.nodes.push_back(canon_node);
      klass.data = std::move(data);
      klass.nodes_dirty = false;
    }

    active_class_positions_[idx] =
        static_cast<uint32_t>(active_class_ids_.size());
    active_class_ids_.push_back(id);
    ++class_count_;
    ++live_node_count_;

    if constexpr (!kIsNoAnalysis) {
      registerParentEdges(nidx, canon_node);
      parent_indexed_until_ = static_cast<uint32_t>(enode_meta_.size());
    } else if (explanations_enabled_) {
      registerParentEdges(nidx, canon_node);
      parent_indexed_until_ = static_cast<uint32_t>(enode_meta_.size());
    }

    OpTable &table = op_tables_[op_id];
    ++table.live_count;
    ++table.mutation_generation;
    markOpDirty(op_id);
    ++view_generation_;

    if constexpr (!kIsNoAnalysis) {
      analysis_.modify(*this, id);
    }
  }

  void makeExplanationLeader(Id node) {
    std::vector<std::pair<Id, ExplanationConnection>> path;
    for (Id current = node;;) {
      auto connection =
          explanation_nodes_.at(current.index()).parent_connection;
      if (connection.next == current) {
        break;
      }
      path.emplace_back(current, connection);
      current = connection.next;
    }

    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      Id current = it->first;
      const auto &connection = it->second;
      explanation_nodes_.at(connection.next.index()).parent_connection =
          ExplanationConnection{connection.justification,
                                !connection.is_rewrite_forward, current,
                                connection.next};
    }
  }

  void addExplanationNeighbor(Id lhs, Id rhs,
                              const Justification &justification,
                              bool forward) {
    explanation_nodes_.at(lhs.index())
        .neighbors.push_back(
            ExplanationConnection{justification, forward, rhs, lhs});
  }

  void addAlternateRewrite(Id lhs, Id rhs, const Symbol &reason) {
    if (!explanations_enabled_ || lhs == rhs) {
      return;
    }
    Justification justification = Justification::ruleJustification(reason);
    explanation_nodes_.at(lhs.index())
        .neighbors.insert(explanation_nodes_.at(lhs.index()).neighbors.begin(),
                          ExplanationConnection{justification, true, rhs, lhs});
    explanation_nodes_.at(rhs.index())
        .neighbors.insert(
            explanation_nodes_.at(rhs.index()).neighbors.begin(),
            ExplanationConnection{justification, false, lhs, rhs});
  }

  void recordExplanation(Id lhs, Id rhs,
                         const std::optional<Justification> &justification) {
    if (!explanations_enabled_) {
      return;
    }

    Justification edge_justification =
        justification.value_or(Justification::congruence());

    if (lhs == rhs) {
      if (edge_justification.isRule()) {
        addExplanationNeighbor(lhs, rhs, edge_justification, true);
        addExplanationNeighbor(rhs, lhs, edge_justification, false);
      }
      return;
    }

    makeExplanationLeader(lhs);
    explanation_nodes_.at(lhs.index()).parent_connection =
        ExplanationConnection{edge_justification, true, rhs, lhs};
    addExplanationNeighbor(lhs, rhs, edge_justification, true);
    addExplanationNeighbor(rhs, lhs, edge_justification, false);
  }

  void canonicalizeInPlace(L &node) const noexcept {
    for (Id &child : node.childrenMut()) {
      child = find(child);
    }
  }

  void canonicalizeInPlaceMut(L &node) noexcept {
    for (Id &child : node.childrenMut()) {
      child = findMut(child);
    }
  }

  std::optional<Id> memoLookup(const L &node) const noexcept {
    uint32_t op_id = findOpId(node.discriminant());
    if (op_id != kInvalidOpId && op_tables_[op_id].use_binary_memo) {
      const auto &ch = node.children();
      uint64_t key = BinaryMemoTable::packKey(ch[0], ch[1]);
      const auto *slot = op_tables_[op_id].binary_memo.find(key, fastMix64(key));
      return slot ? std::optional<Id>(slot->owner) : std::nullopt;
    }
    const uint64_t h = hashNode(node);
    const uint32_t idx = memo_index_.find(h, [&](uint32_t i) {
      const auto &m = enode_meta_[i];
      const auto &g = op_payloads_[m.opId()].general_nodes[m.op_row];
      return g.hash == h && g.node == node;
    });
    if (idx == CompactHashIndex::kInvalid) {
      return std::nullopt;
    }
    return explanations_enabled_ ? enode_orig_ids_[idx]
                                 : enode_meta_[idx].owner;
  }

  static Id getProductId(
      Id class1, Id class2,
      FlatHashMap<std::pair<Id, Id>, Id, PairHash> &product_map) {
    auto key = std::make_pair(class1, class2);
    if (auto it = product_map.find(key); it != product_map.end()) {
      return it->second;
    }
    Id id = Id::fromIndex(product_map.size());
    product_map.try_emplace(key, id);
    return id;
  }

  enum class UnresolvedPolicy { Drop, Reject };

  static EGraph fromEnodes(std::vector<std::pair<L, Id>> enodes,
                           AnalysisT analysis,
                           UnresolvedPolicy unresolved_policy) {
    EGraph egraph(std::move(analysis));
    FlatHashMap<Id, Id> ids;
    FlatHashMap<Id, std::vector<size_t>> waiters;
    std::vector<size_t> missing(enodes.size(), 0);
    std::vector<uint8_t> processed(enodes.size(), 0);
    std::deque<size_t> ready;
    size_t remaining = enodes.size();

    for (size_t i = 0; i < enodes.size(); ++i) {
      llvm::SmallVector<Id, 4> dependencies(enodes[i].first.children().begin(),
                                            enodes[i].first.children().end());
      std::sort(dependencies.begin(), dependencies.end());
      dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                         dependencies.end());
      missing[i] = dependencies.size();
      if (missing[i] == 0) {
        ready.push_back(i);
      }
      for (Id child : dependencies) {
        waiters[child].push_back(i);
      }
    }

    while (!ready.empty()) {
      size_t index = ready.front();
      ready.pop_front();
      if (processed[index]) {
        continue;
      }

      const auto &[enode, external_id] = enodes[index];
      L mapped = enode.mapChildren(
          [&](Id child) { return ids.find(child)->second; });
      auto existing = egraph.lookupInternal(mapped);
      Id added = existing ? *existing : egraph.add(mapped);
      auto [it, inserted] = ids.try_emplace(external_id, added);
      if (!inserted) {
        egraph.unite(it->second, added);
      } else if (auto waiting = waiters.find(external_id);
                 waiting != waiters.end()) {
        for (size_t dependent : waiting->second) {
          if (missing[dependent] > 0 && --missing[dependent] == 0) {
            ready.push_back(dependent);
          }
        }
      }
      processed[index] = 1;
      --remaining;
    }

    if (remaining > 0 && unresolved_policy == UnresolvedPolicy::Reject) {
      throw std::runtime_error(
          "EGraph input contains cyclic or unresolved e-node references");
    }

    return egraph;
  }

  void intersectClasses(
      const EGraph &other, std::vector<std::pair<L, Id>> &result, Id class1,
      Id class2,
      FlatHashMap<std::pair<Id, Id>, Id, PairHash> &product_map) const {
    const auto &left = classAtConst(class1);
    const auto &right = other.classAtConst(class2);
    std::optional<Id> result_id;
    for (const auto &node1 : left.nodes) {
      forEachMatchingNode(right, node1, [&](const L &node2) {
        if (!result_id) {
          result_id = getProductId(class1, class2, product_map);
        }
        auto merged = node1;
        for (size_t i = 0; i < node1.children().size(); ++i) {
          merged.childrenMut()[i] =
              getProductId(find(node1.children()[i]),
                           other.find(node2.children()[i]), product_map);
        }
        result.emplace_back(std::move(merged), *result_id);
        return true;
      });
    }
  }

  Id addInstantiationNoncanonical(const PatternAst<L> &pat, const Subst &subst);

  std::pair<Id, bool> uniteDistinctFast(Id lhs_class, Id rhs_class) {
    const size_t l0 = lhs_class.index();
    const size_t r0 = rhs_class.index();
    const uint32_t lp =
        l0 < parent_meta_.size() ? parent_meta_[l0].parent_count : 0;
    const uint32_t rp =
        r0 < parent_meta_.size() ? parent_meta_[r0].parent_count : 0;
    if (lp < rp ||
        (lp == rp && class_meta_[l0].nodeCount() < class_meta_[r0].nodeCount())) {
      std::swap(lhs_class, rhs_class);
    }

    const size_t li = lhs_class.index();
    const size_t ri = rhs_class.index();
    union_find_.unite(lhs_class, rhs_class);
    markMergedClass(ri);
    has_unions_since_rebuild_ = true;

    auto &lm = class_meta_[li];
    auto &rm = class_meta_[ri];
    const uint32_t r_node_count = rm.nodeCount();
    rm.setDead();

    const size_t removed_pos = active_class_positions_[ri];
    const Id moved_id = active_class_ids_.back();
    active_class_ids_[removed_pos] = moved_id;
    active_class_positions_[moved_id.index()] =
        static_cast<uint32_t>(removed_pos);
    active_class_ids_.pop_back();
    active_class_positions_[ri] = std::numeric_limits<uint32_t>::max();
    --class_count_;

    if (ri < parent_meta_.size() &&
        parent_meta_[ri].parent_head != ClassMeta::kNil) {
      if (li >= parent_meta_.size()) {
        parent_meta_.resize(union_find_.size());
      }
      auto &lpm = parent_meta_[li];
      auto &rpm = parent_meta_[ri];
      uint32_t last_e = rpm.parent_head;
      for (uint32_t e = rpm.parent_head; e != ClassMeta::kNil;
           e = parent_edge_pool_[e].next) {
        last_e = e;
        uint32_t pnode = parent_edge_pool_[e].nidx;
        if (!enode_meta_[pnode].isDead()) {
          dirty_enodes_.push_back(pnode);
        }
      }
      parent_edge_pool_[last_e].next = lpm.parent_head;
      lpm.parent_head = rpm.parent_head;
      lpm.parent_count += rpm.parent_count;
      rpm.parent_head = ClassMeta::kNil;
      rpm.parent_count = 0;
    }

    if (rm.node_head != ClassMeta::kNil) {
      uint32_t last_n = rm.node_head;
      for (uint32_t nidx = rm.node_head; nidx != ClassMeta::kNil;
           nidx = enode_meta_[nidx].next_in_cls) {
        last_n = nidx;
        if (!enode_meta_[nidx].isDead()) {
          owner_changed_enodes_.push_back(nidx);
        }
      }
      enode_meta_[last_n].next_in_cls = lm.node_head;
      lm.node_head = rm.node_head;
      lm.addNodes(r_node_count);
      rm.node_head = ClassMeta::kNil;
    }

    ++view_generation_;
    return {lhs_class, true};
  }

  std::pair<Id, bool>
  uniteImpl(Id lhs, Id rhs, const std::optional<Justification> &justification) {
    if constexpr (!kIsNoAnalysis) {
      analysis_.preUnion(*this, lhs, rhs, justification);
    }

    clean_ = false;
    Id original_lhs = lhs;
    Id original_rhs = rhs;
    Id lhs_class = findMut(lhs);
    Id rhs_class = findMut(rhs);
    if (lhs_class == rhs_class) {
      if (justification && justification->isRule() && explanations_enabled_) {
        addAlternateRewrite(original_lhs, original_rhs, justification->rule);
        recordUnion(original_lhs, original_rhs, justification);
      }
      return {lhs_class, false};
    }

    const size_t l0 = lhs_class.index();
    const size_t r0 = rhs_class.index();
    const uint32_t lp =
        l0 < parent_meta_.size() ? parent_meta_[l0].parent_count : 0;
    const uint32_t rp =
        r0 < parent_meta_.size() ? parent_meta_[r0].parent_count : 0;
    if (lp < rp ||
        (lp == rp && class_meta_[l0].nodeCount() < class_meta_[r0].nodeCount())) {
      std::swap(lhs_class, rhs_class);
    }

    if (explanations_enabled_) {
      recordExplanation(original_lhs, original_rhs, justification);
      recordUnion(original_lhs, original_rhs, justification);
    }

    const size_t li = lhs_class.index();
    const size_t ri = rhs_class.index();
    union_find_.unite(lhs_class, rhs_class);
    markMergedClass(ri);
    has_unions_since_rebuild_ = true;

    auto &lm = class_meta_[li];
    auto &rm = class_meta_[ri];
    const uint32_t r_node_count = rm.nodeCount();
    rm.setDead();

    const size_t removed_pos = active_class_positions_[ri];
    const Id moved_id = active_class_ids_.back();
    active_class_ids_[removed_pos] = moved_id;
    active_class_positions_[moved_id.index()] =
        static_cast<uint32_t>(removed_pos);
    active_class_ids_.pop_back();
    active_class_positions_[ri] = std::numeric_limits<uint32_t>::max();
    --class_count_;

    if (ri < parent_meta_.size() &&
        parent_meta_[ri].parent_head != ClassMeta::kNil) {
      if (li >= parent_meta_.size()) {
        parent_meta_.resize(union_find_.size());
      }
      auto &lpm = parent_meta_[li];
      auto &rpm = parent_meta_[ri];
      uint32_t last_e = rpm.parent_head;
      for (uint32_t e = rpm.parent_head; e != ClassMeta::kNil;
           e = parent_edge_pool_[e].next) {
        last_e = e;
        uint32_t pnode = parent_edge_pool_[e].nidx;
        if (!enode_meta_[pnode].isDead()) {
          dirty_enodes_.push_back(pnode);
        }
      }
      parent_edge_pool_[last_e].next = lpm.parent_head;
      lpm.parent_head = rpm.parent_head;
      lpm.parent_count += rpm.parent_count;
      rpm.parent_head = ClassMeta::kNil;
      rpm.parent_count = 0;
    }

    if (rm.node_head != ClassMeta::kNil) {
      uint32_t last_n = rm.node_head;
      for (uint32_t nidx = rm.node_head; nidx != ClassMeta::kNil;
           nidx = enode_meta_[nidx].next_in_cls) {
        last_n = nidx;
        if (!enode_meta_[nidx].isDead()) {
          owner_changed_enodes_.push_back(nidx);
        }
      }
      enode_meta_[last_n].next_in_cls = lm.node_head;
      lm.node_head = rm.node_head;
      lm.addNodes(r_node_count);
      rm.node_head = ClassMeta::kNil;
    }

    if constexpr (!kIsNoAnalysis) {
      auto right_class = std::move(*classes_[ri]);
      classes_[ri].reset();
      auto &left_class = *classes_[li];
      DidMerge did_merge =
          analysis_.merge(left_class.data, std::move(right_class.data));
      if (did_merge.left_changed) {
        analysis_pending_.extend(left_class.parents);
      }
      if (did_merge.right_changed) {
        analysis_pending_.extend(right_class.parents);
      }
      if (!right_class.parents.empty()) {
        left_class.parents.append(right_class.parents.begin(),
                                  right_class.parents.end());
        markParentsDirty(lhs_class);
      }
      left_class.nodes.reserve(left_class.nodes.size() +
                               right_class.nodes.size());
      for (auto &n : right_class.nodes) {
        left_class.nodes.push_back(std::move(n));
      }
      left_class.matching_nodes.reset();
      left_class.nodes_dirty = true;
      markClassDirty(lhs_class);
      analysis_.modify(*this, lhs_class);
    }

    ++view_generation_;
    return {lhs_class, true};
  }

  void recordUnion(Id lhs, Id rhs,
                   const std::optional<Justification> &justification) {
    if (explanations_enabled_) {
      union_events_.push_back(
          {lhs, rhs, justification.value_or(Justification::congruence())});
    }
  }

  void rebuildClasses() {
    if constexpr (kIsNoAnalysis) {
      return; // EClass views are materialized 100% lazily on demand!
    } else {
      for (Id dirty_id : dirty_classes_) {
        class_dirty_marks_[dirty_id.index()] = 0;
        Id owner = findMut(dirty_id);
        if (!hasClass(owner)) {
          continue;
        }
        auto &klass = *classes_[owner.index()];
        if (!klass.nodes_dirty) {
          continue;
        }

        const auto &cm = class_meta_[owner.index()];
        klass.nodes.clear();
        klass.nodes.reserve(cm.nodeCount());
        forEachClassNode(owner, [&](uint32_t, const L &node) {
          klass.nodes.push_back(node);
        });
        if (klass.nodes.size() > 1) {
          std::sort(klass.nodes.begin(), klass.nodes.end());
        }
        klass.rebuildMatchingIndex();
        klass.nodes_dirty = false;
      }
      dirty_classes_.clear();

      for (Id pid : dirty_parents_classes_) {
        parents_dirty_marks_[pid.index()] = 0;
        Id owner = findMut(pid);
        if (!hasClass(owner)) {
          continue;
        }
        auto &parents = classes_[owner.index()]->parents;
        for (Id &p : parents) {
          p = findMut(p);
        }
        if (parents.size() > 1) {
          std::sort(parents.begin(), parents.end());
          parents.erase(std::unique(parents.begin(), parents.end()),
                        parents.end());
        }
      }
      dirty_parents_classes_.clear();
    }
  }

  void updateOpRowToCurrentEpoch(uint32_t nidx) {
    auto &m = enode_meta_[nidx];
    if (m.isDead()) {
      return;
    }
    const uint32_t op_id = m.opId();
    OpTable &table = op_tables_[op_id];
    const Id owner = findMut(m.owner);
    m.owner = owner;

    const uint32_t old_row_idx = m.op_row;
    OpRow &old_row = table.rows[old_row_idx];
    if (m.isBinary()) {
      if (old_row.version == epoch_) {
        old_row.eclass = owner;
      } else {
        OpRow copy = old_row;
        copy.eclass = owner;
        copy.version = epoch_;
        old_row.eclass = OpRow::kDeadClass;
        ++table.dead_count;
        m.op_row = static_cast<uint32_t>(table.rows.size());
        table.rows.push_back(copy);
      }
    } else {
      auto &gen_nodes = op_payloads_[op_id].general_nodes;
      if (old_row.version == epoch_) {
        old_row = makeOpRow(owner, nidx, epoch_, gen_nodes[old_row_idx].node);
      } else {
        old_row.eclass = OpRow::kDeadClass;
        ++table.dead_count;
        m.op_row = static_cast<uint32_t>(table.rows.size());
        GeneralEnode moved = std::move(gen_nodes[old_row_idx]);
        table.rows.push_back(makeOpRow(owner, nidx, epoch_, moved.node));
        gen_nodes.push_back(std::move(moved));
      }
    }
    ++table.mutation_generation;
    markOpDirty(op_id);
  }

  void finalizeOpTables() {
    for (uint32_t nidx : rebuilt_enodes_) {
      updateOpRowToCurrentEpoch(nidx);
    }
    rebuilt_enodes_.clear();

    for (uint32_t nidx : owner_changed_enodes_) {
      updateOpRowToCurrentEpoch(nidx);
    }
    owner_changed_enodes_.clear();

    for (uint32_t op_id : dirty_ops_) {
      op_dirty_marks_[op_id] = 0;
      OpTable &table = op_tables_[op_id];

      if (table.dead_count > 0 && table.dead_count * 4 >= table.rows.size()) {
        auto &gen_nodes = op_payloads_[op_id].general_nodes;
        const bool has_gen = !table.use_binary_memo;
        uint32_t write = 0;
        for (uint32_t read = 0; read < table.rows.size(); ++read) {
          if (!table.rows[read].isDead()) {
            if (write != read) {
              table.rows[write] = table.rows[read];
              if (has_gen) {
                gen_nodes[write] = std::move(gen_nodes[read]);
              }
            }
            enode_meta_[table.rows[write].node_idx].op_row = write;
            ++write;
          }
        }
        table.rows.resize(write);
        if (has_gen) {
          gen_nodes.erase(gen_nodes.begin() + write, gen_nodes.end());
        }
        table.dead_count = 0;
        table.version_offsets.clear();
        table.version_offsets.resize(epoch_ + 1, write);
        uint32_t v = 0;
        for (uint32_t i = 0; i < write; ++i) {
          uint32_t rv = table.rows[i].version;
          while (v < rv) {
            table.version_offsets[v++] = i;
          }
        }
        while (v <= epoch_) {
          table.version_offsets[v++] = write;
        }
      } else {
        const uint32_t prev_end =
            table.version_offsets.empty() ? 0 : table.version_offsets.back();
        if (table.version_offsets.size() <= epoch_) {
          uint32_t split = static_cast<uint32_t>(table.rows.size());
          while (split > prev_end && table.rows[split - 1].version == epoch_) {
            --split;
          }
          while (table.version_offsets.size() < epoch_) {
            table.version_offsets.push_back(split);
          }
          table.version_offsets.push_back(
              static_cast<uint32_t>(table.rows.size()));
        } else {
          table.version_offsets[epoch_] =
              static_cast<uint32_t>(table.rows.size());
        }
      }
      ++table.mutation_generation;
    }
    dirty_ops_.clear();
  }

  size_t repairOneEnode(uint32_t nidx) {
    auto &m = enode_meta_[nidx];
    if (m.isDead()) {
      return 0;
    }

    const uint32_t op_id = m.opId();
    OpTable &table = op_tables_[op_id];
    if (m.isBinary()) {
      OpRow &row = table.rows[m.op_row];
      const Id old_c0 = row.c0;
      const Id old_c1 = row.c1;
      const Id new_c0 = findMut(old_c0);
      const Id new_c1 = findMut(old_c1);
      if (new_c0 == old_c0 && new_c1 == old_c1) {
        return 0;
      }
      const uint64_t old_key = BinaryMemoTable::packKey(old_c0, old_c1);
      if (table.binary_memo.erase(old_key, fastMix64(old_key), nidx)) {
        --total_memo_entries_;
      }
      row.c0 = new_c0;
      row.c1 = new_c1;
      const uint64_t new_key = BinaryMemoTable::packKey(new_c0, new_c1);
      const uint64_t new_h = fastMix64(new_key);
      Id owner = findMut(m.owner);
      m.owner = owner;

      auto [slot, inserted] = table.binary_memo.findOrInsert(
          new_key, new_h,
          [&]() -> std::pair<uint32_t, Id> { return {nidx, owner}; });
      if (inserted) {
        ++total_memo_entries_;
        rebuilt_enodes_.push_back(nidx);
        return 0;
      }
      m.setDead();
      --live_node_count_;
      if (!row.isDead()) {
        row.eclass = OpRow::kDeadClass;
        ++table.dead_count;
        --table.live_count;
        markOpDirty(op_id);
      }
      Id existing_owner = findMut(slot->owner);
      slot->owner = existing_owner;
      if (existing_owner != owner) {
        auto [merged, did] = uniteDistinctFast(existing_owner, owner);
        slot->owner = merged;
        enode_meta_[slot->nidx].owner = merged;
        return did ? 1 : 0;
      }
      return 0;
    }

    auto &gen = op_payloads_[op_id].general_nodes[m.op_row];
    L &node = gen.node;
    bool changed = false;
    auto &&ch = node.childrenMut();
    for (Id c : ch) {
      if (findMut(c) != c) {
        changed = true;
        break;
      }
    }
    if (!changed) {
      return 0;
    }

    const uint64_t old_hash = gen.hash;
    if (memo_index_.erase(old_hash, [&](uint32_t idx) { return idx == nidx; })) {
      --total_memo_entries_;
    }

    for (Id &c : ch) {
      c = findMut(c);
    }
    const uint64_t new_hash = hashNode(node);
    gen.hash = new_hash;

    Id owner = findMut(m.owner);
    m.owner = owner;

    auto [existing_idx, inserted] = memo_index_.findOrInsert(
        new_hash,
        [&](uint32_t idx) {
          const auto &om = enode_meta_[idx];
          const auto &og = op_payloads_[om.opId()].general_nodes[om.op_row];
          return og.hash == new_hash && og.node == node;
        },
        [&]() { return nidx; },
        [&](uint32_t idx) {
          const auto &om = enode_meta_[idx];
          return op_payloads_[om.opId()].general_nodes[om.op_row].hash;
        });

    if (inserted) {
      ++total_memo_entries_;
      if constexpr (!kIsNoAnalysis) {
        if (hasClass(owner)) {
          classes_[owner.index()]->nodes_dirty = true;
          markClassDirty(owner);
        }
      }
      rebuilt_enodes_.push_back(nidx);
      return 0;
    }

    m.setDead();
    --live_node_count_;
    OpRow &row = table.rows[m.op_row];
    if (!row.isDead()) {
      row.eclass = OpRow::kDeadClass;
      ++table.dead_count;
      --table.live_count;
      markOpDirty(op_id);
    }
    if constexpr (!kIsNoAnalysis) {
      if (hasClass(owner)) {
        classes_[owner.index()]->nodes_dirty = true;
        markClassDirty(owner);
      }
    }

    Id existing_owner = findMut(enode_meta_[existing_idx].owner);
    enode_meta_[existing_idx].owner = existing_owner;
    if (existing_owner != owner) {
      std::pair<Id, bool> merged;
      if constexpr (kIsNoAnalysis) {
        if (!explanations_enabled_) {
          merged = uniteDistinctFast(existing_owner, owner);
        } else {
          merged = uniteImpl(enode_orig_ids_[existing_idx],
                             enode_orig_ids_[nidx],
                             Justification::congruence());
        }
      } else {
        merged = uniteImpl(
            explanations_enabled_ ? enode_orig_ids_[existing_idx]
                                  : existing_owner,
            explanations_enabled_ ? enode_orig_ids_[nidx] : owner,
            Justification::congruence());
      }
      enode_meta_[existing_idx].owner = merged.first;
      return merged.second ? 1 : 0;
    }
    return 0;
  }

  inline void markMergedClass(size_t ri) {
    const size_t word = ri >> 6;
    if (word >= merged_bits_.size()) {
      merged_bits_.resize(word + 1, 0);
    }
    if (merged_bits_[word] == 0) {
      merged_dirty_words_.push_back(static_cast<uint32_t>(word));
    }
    merged_bits_[word] |= (1ULL << (ri & 63));
  }

  inline void clearMergedBits() noexcept {
    const size_t n = merged_dirty_words_.size();
    const uint32_t *words = merged_dirty_words_.data();
    uint64_t *bits = merged_bits_.data();
    for (size_t i = 0; i < n; ++i) {
      bits[words[i]] = 0;
    }
    merged_dirty_words_.clear();
  }

  size_t repairCongruence() {
    size_t unions = 0;
    if (!has_unions_since_rebuild_ && dirty_enodes_.empty() &&
        merged_dirty_words_.empty()) {
      return 0;
    }

    auto drainDirtyPrefetched = [&]() -> size_t {
      size_t round_unions = 0;
      constexpr size_t kPfBatch = 32;
      uint32_t b_nidx[kPfBatch];
      uint32_t b_op[kPfBatch];
      Id b_nc0[kPfBatch];
      Id b_nc1[kPfBatch];
      uint64_t b_ok[kPfBatch];
      uint64_t b_oh[kPfBatch];
      uint64_t b_nk[kPfBatch];
      uint64_t b_nh[kPfBatch];

      size_t base = 0;
      while (base < dirty_enodes_.size()) {
        const size_t end = std::min(dirty_enodes_.size(), base + kPfBatch);
        const size_t batch_classes_before = class_count_;
        size_t nb = 0;
        // Pass 1: Precompute hashes & prefetch old+new BinaryMemoTable buckets
        // concurrently for all binary nodes in this 32-element batch!
        for (size_t i = base; i < end; ++i) {
          const uint32_t nidx = dirty_enodes_[i];
          const auto &m = enode_meta_[nidx];
          if (m.isDead()) {
            continue;
          }
          if (!m.isBinary()) {
            round_unions += repairOneEnode(nidx);
            continue;
          }
          const uint32_t op_id = m.opId();
          const OpTable &tbl = op_tables_[op_id];
          const OpRow &row = tbl.rows[m.op_row];
          const Id nc0 = findMut(row.c0);
          const Id nc1 = findMut(row.c1);
          if (nc0 == row.c0 && nc1 == row.c1) {
            continue;
          }
          const uint64_t ok = BinaryMemoTable::packKey(row.c0, row.c1);
          const uint64_t oh = fastMix64(ok);
          const uint64_t nk = BinaryMemoTable::packKey(nc0, nc1);
          const uint64_t nh = fastMix64(nk);
          tbl.binary_memo.prefetch(oh);
          tbl.binary_memo.prefetch(nh);
          b_nidx[nb] = nidx;
          b_op[nb] = op_id;
          b_nc0[nb] = nc0;
          b_nc1[nb] = nc1;
          b_ok[nb] = ok;
          b_oh[nb] = oh;
          b_nk[nb] = nk;
          b_nh[nb] = nh;
          ++nb;
        }
        // Pass 2: Execute binary erase + findOrInsert from L1 cache using
        // precomputed keys/hashes (re-canonicalizing only if a union happened
        // earlier in this 32-element batch)!
        for (size_t k = 0; k < nb; ++k) {
          const uint32_t nidx = b_nidx[k];
          auto &m = enode_meta_[nidx];
          if (m.isDead()) {
            continue;
          }
          const uint32_t op_id = b_op[k];
          OpTable &table = op_tables_[op_id];
          OpRow &row = table.rows[m.op_row];
          Id new_c0 = b_nc0[k];
          Id new_c1 = b_nc1[k];
          uint64_t new_key = b_nk[k];
          uint64_t new_h = b_nh[k];
          if (class_count_ != batch_classes_before) {
            Id fc0 = findMut(new_c0);
            Id fc1 = findMut(new_c1);
            if (fc0 != new_c0 || fc1 != new_c1) {
              new_c0 = fc0;
              new_c1 = fc1;
              new_key = BinaryMemoTable::packKey(new_c0, new_c1);
              new_h = fastMix64(new_key);
            }
          }
          if (table.binary_memo.erase(b_ok[k], b_oh[k], nidx)) {
            --total_memo_entries_;
          }
          row.c0 = new_c0;
          row.c1 = new_c1;
          Id owner = findMut(m.owner);
          m.owner = owner;

          auto [slot, inserted] = table.binary_memo.findOrInsert(
              new_key, new_h,
              [&]() -> std::pair<uint32_t, Id> { return {nidx, owner}; });
          if (inserted) {
            ++total_memo_entries_;
            rebuilt_enodes_.push_back(nidx);
          } else {
            m.setDead();
            --live_node_count_;
            if (!row.isDead()) {
              row.eclass = OpRow::kDeadClass;
              ++table.dead_count;
              --table.live_count;
              markOpDirty(op_id);
            }
            Id existing_owner = findMut(slot->owner);
            slot->owner = existing_owner;
            if (existing_owner != owner) {
              auto [merged, did] = uniteDistinctFast(existing_owner, owner);
              slot->owner = merged;
              enode_meta_[slot->nidx].owner = merged;
              if (did) {
                ++round_unions;
              }
            }
          }
        }
        base = end;
      }
      dirty_enodes_.clear();
      return round_unions;
    };

    size_t scan_rounds = 0;
    for (;;) {
      size_t round_unions = drainDirtyPrefetched();
      unions += round_unions;

      if constexpr (kIsNoAnalysis) {
        if (!explanations_enabled_ &&
            parent_indexed_until_ < enode_meta_.size() &&
            (!merged_dirty_words_.empty() || has_unions_since_rebuild_ ||
             round_unions > 0)) {
          has_unions_since_rebuild_ = false;
          if (scan_rounds >= 8 ||
              (scan_rounds >= 4 && enode_meta_.size() < 100000)) {
            clearMergedBits();
            ensureParentEdgesMaterialized(&dirty_enodes_);
            if (!dirty_enodes_.empty()) {
              continue;
            }
            break;
          }

          const bool use_bitset = !merged_dirty_words_.empty();
          if (use_bitset) {
            const size_t needed_words = (union_find_.size() + 63) >> 6;
            if (merged_bits_.size() < needed_words) {
              merged_bits_.resize(needed_words, 0);
            }
          }
          const uint64_t *mb = merged_bits_.data();
          const Id *uf = union_find_.parentsData();
          const uint32_t indexed = parent_indexed_until_;

          // Pass 1: pure sequential scan using 66 KB L1-resident merged_bits_!
          for (uint32_t op_id = 0; op_id < op_tables_.size(); ++op_id) {
            OpTable &tbl = op_tables_[op_id];
            if (tbl.live_count == 0) {
              continue;
            }
            const OpRow *rows = tbl.rows.data();
            const size_t nrows = tbl.rows.size();
            if (tbl.use_binary_memo) {
              if (use_bitset && indexed == 0) {
                for (size_t r = 0; r < nrows; ++r) {
                  const OpRow &row = rows[r];
                  if (row.isDead()) {
                    continue;
                  }
                  const size_t i0 = row.c0.index();
                  const size_t i1 = row.c1.index();
                  if ((((mb[i0 >> 6] >> (i0 & 63)) |
                        (mb[i1 >> 6] >> (i1 & 63))) &
                       1ULL) == 0) {
                    continue;
                  }
                  dirty_enodes_.push_back(row.node_idx);
                }
              } else if (indexed == 0) {
                for (size_t r = 0; r < nrows; ++r) {
                  const OpRow &row = rows[r];
                  if (row.isDead()) {
                    continue;
                  }
                  if (uf[row.c0.index()] == row.c0 &&
                      uf[row.c1.index()] == row.c1) {
                    continue;
                  }
                  dirty_enodes_.push_back(row.node_idx);
                }
              } else {
                for (size_t r = 0; r < nrows; ++r) {
                  const OpRow &row = rows[r];
                  if (row.isDead() || row.node_idx < indexed) {
                    continue;
                  }
                  if (uf[row.c0.index()] == row.c0 &&
                      uf[row.c1.index()] == row.c1) {
                    continue;
                  }
                  dirty_enodes_.push_back(row.node_idx);
                }
              }
            } else {
              auto &gen_nodes = op_payloads_[op_id].general_nodes;
              for (size_t r = 0; r < nrows; ++r) {
                const OpRow &row = rows[r];
                if (row.isDead() || row.node_idx < indexed) {
                  continue;
                }
                for (Id c : gen_nodes[r].node.children()) {
                  if (!union_find_.isRoot(c)) {
                    dirty_enodes_.push_back(row.node_idx);
                    break;
                  }
                }
              }
            }
          }
          clearMergedBits();
          ++scan_rounds;
          if (!dirty_enodes_.empty()) {
            continue;
          }
          break;
        }
      }
      clearMergedBits();
      has_unions_since_rebuild_ = false;
      break;
    }
    return unions;
  }

  size_t processPending() {
    size_t unions = 0;
    bool repair_needed = !clean_ || !dirty_enodes_.empty();

    while (repair_needed || !analysis_pending_.empty()) {
      if (repair_needed) {
        unions += repairCongruence();
        rebuildClasses();
        repair_needed = false;
      }

      if constexpr (!kIsNoAnalysis) {
        while (!analysis_pending_.empty()) {
          auto class_id_opt = analysis_pending_.pop();
          if (!class_id_opt) {
            break;
          }

          Id canonical = findMut(*class_id_opt);
          if (!hasClass(canonical)) {
            continue;
          }

          auto nodes_copy = classes_[canonical.index()]->nodes;
          bool data_changed = false;
          for (const auto &node : nodes_copy) {
            Data node_data = AnalysisT::remake(*this, node, canonical);
            DidMerge did_merge = analysis_.merge(
                classes_[canonical.index()]->data, std::move(node_data));
            data_changed = data_changed || did_merge.left_changed;
          }

          if (data_changed) {
            analysis_pending_.extend(classes_[canonical.index()]->parents);
            analysis_.modify(*this, canonical);
          }
        }
      } else {
        analysis_pending_.clear();
      }

      repair_needed = !dirty_enodes_.empty() || !dirty_classes_.empty();
    }

    return unions;
  }

  bool chooseFiniteNode(Id id, FlatHashMap<Id, size_t> &choices,
                        FlatHashMap<Id, uint8_t> &visiting) const {
    Id canonical = find(id);
    if (choices.count(canonical)) {
      return true;
    }
    if (!visiting.try_emplace(canonical, 1).second) {
      return false;
    }

    const auto &klass = classAtConst(canonical);
    for (size_t index = 0; index < klass.nodes.size(); ++index) {
      const auto &node = klass.nodes[index];
      bool finite = true;
      for (Id child : node.children()) {
        if (!chooseFiniteNode(child, choices, visiting)) {
          finite = false;
          break;
        }
      }
      if (finite) {
        choices.try_emplace(canonical, index);
        visiting.erase(canonical);
        return true;
      }
    }

    visiting.erase(canonical);
    return false;
  }

  Id materializeFiniteExpr(RecExpr<L> &expr, Id id,
                           const FlatHashMap<Id, size_t> &choices,
                           FlatHashMap<Id, Id> &cache) const {
    Id canonical = find(id);
    if (auto it = cache.find(canonical); it != cache.end()) {
      return it->second;
    }

    const auto &node =
        classAtConst(canonical).nodes[choices.find(canonical)->second];
    auto materialized = node.mapChildren([&](Id child) {
      return materializeFiniteExpr(expr, child, choices, cache);
    });
    Id added = expr.add(materialized);
    cache.try_emplace(canonical, added);
    return added;
  }

#ifndef NDEBUG
  void assertCongruenceInvariant() const {
    FlatHashMap<L, Id, LanguageHash<L>> owners;
    owners.reserve(totalNumberOfNodes());

    for (Id class_id : active_class_ids_) {
      assert(find(class_id) == class_id);
      forEachClassNode(class_id, [&](uint32_t, const L &raw_node) {
        L node = raw_node;
        canonicalizeInPlace(node);
        auto [it, inserted] = owners.try_emplace(node, class_id);
        assert(inserted || find(it->second) == class_id);
      });
    }

    assert(owners.size() == total_memo_entries_);
    memo_index_.forEach([&](uint32_t idx) {
      const auto &m = enode_meta_[idx];
      assert(!m.isDead());
      const L &node = op_payloads_[m.opId()].general_nodes[m.op_row].node;
      auto it = owners.find(node);
      assert(it != owners.end());
      assert(find(it->second) == find(m.owner));
    });
    for (const auto &tbl : op_tables_) {
      if (tbl.use_binary_memo) {
        tbl.binary_memo.forEach([&](const BinaryMemoTable::Slot &slot) {
          const auto &m = enode_meta_[slot.nidx];
          assert(!m.isDead());
          L node = materializeEnode(slot.nidx);
          auto it = owners.find(node);
          assert(it != owners.end());
          assert(find(it->second) == find(slot.owner));
        });
      }
    }
  }
#endif

  Id originalExprInternal(RecExpr<L> &expr, Id id,
                          FlatHashMap<Id, Id> &cache) const {
    if (auto it = cache.find(id); it != cache.end()) {
      return it->second;
    }

    ensureNodesMaterialized();
    const auto &node = nodes_.at(id.index());
    auto materialized = node.mapChildren(
        [&](Id child) { return originalExprInternal(expr, child, cache); });
    Id added = expr.add(materialized);
    cache.try_emplace(id, added);
    return added;
  }

  Id idToPatternInternal(RecExpr<ENodeOrVar<L>> &pat, Id id,
                         const std::unordered_map<Id, Id> &substitutions,
                         Subst &subst, FlatHashMap<Id, Id> &cache) const;

  AnalysisT analysis_;
  UnionFind union_find_;

  // Original uncanonical node table (lazy for NoAnalysis without explanations!)
  mutable std::vector<L> nodes_;
  PodVec<OrigEntry> orig_entries_;
  std::vector<L> general_orig_nodes_;

  // Ultra-compact 16-byte e-node metadata arena (zero L storage for binary ops)
  PodVec<EnodeMeta> enode_meta_;
  std::vector<Id> enode_orig_ids_;

  // Compact SwissTable hashcons index (general non-binary ops -> enode_meta_ idx)
  CompactHashIndex memo_index_;
  size_t total_memo_entries_ = 0;

  // Explanation-mode tables
  FlatHashMap<L, Id, LanguageHash<L>> original_node_ids_;
  FlatHashMap<L, Id, LanguageHash<L>> uncanonical_memo_;

  // Compact 8-byte per-class node lists + lazy parent edge pool
  PodVec<ClassMeta> class_meta_;
  mutable std::vector<ParentMeta> parent_meta_;
  mutable std::vector<ParentEdge> parent_edge_pool_;
  mutable uint32_t parent_indexed_until_ = 0;
  bool has_unions_since_rebuild_ = false;
  size_t class_count_ = 0;
  std::vector<Id> active_class_ids_;
  PodVec<uint32_t> active_class_positions_;

  // EClass views: eager for custom AnalysisT, 100% lazy for NoAnalysis!
  mutable std::vector<std::optional<Class>> classes_;
  mutable std::vector<uint64_t> class_view_gen_;
  uint64_t view_generation_ = 1;

  // Rebuild worklists
  PodVec<uint64_t> merged_bits_;
  PodVec<uint32_t> merged_dirty_words_;
  std::vector<uint32_t> dirty_enodes_;
  std::vector<uint32_t> rebuilt_enodes_;
  std::vector<uint32_t> owner_changed_enodes_;
  std::vector<Id> dirty_classes_;
  std::vector<uint8_t> class_dirty_marks_;
  std::vector<Id> dirty_parents_classes_;
  std::vector<uint8_t> parents_dirty_marks_;
  UniqueQueue<Id> analysis_pending_;

  // Operator-partitioned flat tables + semi-naive versioning
  FlatHashMap<typename L::Discriminant, uint32_t> op_ids_;
  std::vector<OpTable> op_tables_;
  std::vector<OpPayload> op_payloads_;
  std::vector<typename L::Discriminant> op_discriminants_;
  std::vector<uint32_t> dirty_ops_;
  std::vector<uint8_t> op_dirty_marks_;
  uint32_t epoch_ = 1;
  uint32_t completed_version_ = 0;

  // Lazily materialized caches
  mutable std::vector<Id> class_ids_;
  mutable bool class_ids_dirty_ = true;
  mutable FlatHashMap<typename L::Discriminant, std::vector<Id>> classes_by_op_;
  mutable bool classes_by_op_dirty_ = true;

  std::vector<UnionEvent> union_events_;
  std::vector<ExplanationNode> explanation_nodes_;
  size_t live_node_count_ = 0;
  bool clean_ = false;
  bool explanations_enabled_ = false;
  bool optimize_explanation_lengths_ = true;
};

} // namespace lotus::egraph

#include "Solvers/EGraph/Pattern.h"

namespace lotus::egraph {

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct LanguageMapper {
  using SourceGraph = EGraph<SrcL, SrcA>;
  using TargetGraph = EGraph<DstL, DstA>;
  using TargetData = typename DstA::Data;

  virtual ~LanguageMapper() = default;

  virtual DstL mapNode(const SrcL &node) const = 0;
  virtual typename DstL::Discriminant
  mapDiscriminant(const typename SrcL::Discriminant &discriminant) const = 0;
  virtual DstA mapAnalysis(const SrcA &analysis) const = 0;
  virtual TargetData mapData(const typename SrcA::Data &data) const = 0;

  EClass<DstL, TargetData>
  mapEClass(const EClass<SrcL, typename SrcA::Data> &src_eclass) const {
    EClass<DstL, TargetData> result;
    result.id = src_eclass.id;
    result.nodes.reserve(src_eclass.nodes.size());
    for (const auto &node : src_eclass.nodes) {
      result.nodes.push_back(mapNode(node));
    }
    result.data = mapData(src_eclass.data);
    result.parents = src_eclass.parents;
    result.rebuildMatchingIndex();
    result.nodes_dirty = false;
    return result;
  }

  TargetGraph mapEGraph(const SourceGraph &src_egraph) const {
    src_egraph.ensureNodesMaterialized();
    TargetGraph dst_egraph(mapAnalysis(src_egraph.analysis_));
    dst_egraph.union_find_ = src_egraph.union_find_;
    if constexpr (TargetGraph::kIsNoAnalysis) {
      dst_egraph.orig_entries_.reserve(src_egraph.nodes_.size());
      dst_egraph.general_orig_nodes_.reserve(src_egraph.nodes_.size());
      for (const auto &node : src_egraph.nodes_) {
        uint32_t gidx =
            static_cast<uint32_t>(dst_egraph.general_orig_nodes_.size());
        dst_egraph.general_orig_nodes_.push_back(mapNode(node));
        dst_egraph.orig_entries_.push_back(
            typename TargetGraph::OrigEntry{
                TargetGraph::OrigEntry::kGeneralBit | gidx, Id(0), Id(0)});
      }
    } else {
      dst_egraph.nodes_.reserve(src_egraph.nodes_.size());
      for (const auto &node : src_egraph.nodes_) {
        dst_egraph.nodes_.push_back(mapNode(node));
      }
    }

    dst_egraph.enode_meta_.reserve(src_egraph.enode_meta_.size());
    dst_egraph.epoch_ = src_egraph.epoch_;
    dst_egraph.completed_version_ = src_egraph.completed_version_;

    for (uint32_t i = 0; i < src_egraph.enode_meta_.size(); ++i) {
      const auto &sm = src_egraph.enode_meta_[i];
      if (sm.isDead()) {
        typename TargetGraph::EnodeMeta dead_m(sm.owner,
                                               TargetGraph::EnodeMeta::kDeadBit,
                                               0, sm.next_in_cls);
        dst_egraph.enode_meta_.push_back(dead_m);
        continue;
      }
      DstL mapped = mapNode(src_egraph.materializeEnode(i));
      const uint32_t ver =
          src_egraph.op_tables_[sm.opId()].rows[sm.op_row].version;
      uint32_t op_id =
          dst_egraph.getOrCreateOpId(mapped.discriminant(), &mapped);
      auto &table = dst_egraph.op_tables_[op_id];
      const uint32_t row_idx = static_cast<uint32_t>(table.rows.size());
      Id canon_own = dst_egraph.find(sm.owner);
      const uint32_t flags =
          op_id | (table.use_binary_memo ? TargetGraph::EnodeMeta::kBinaryBit : 0u);
      dst_egraph.enode_meta_.emplace_back(canon_own, flags, row_idx,
                                          sm.next_in_cls);
      table.rows.push_back(TargetGraph::makeOpRow(canon_own, i, ver, mapped));
      ++table.live_count;
      if (table.use_binary_memo) {
        const auto &ch = mapped.children();
        uint64_t key = BinaryMemoTable::packKey(ch[0], ch[1]);
        uint64_t bh = fastMix64(key);
        auto [_, ins] = table.binary_memo.findOrInsert(
            key, bh,
            [&]() -> std::pair<uint32_t, Id> { return {i, canon_own}; });
        if (ins) {
          ++dst_egraph.total_memo_entries_;
        }
      } else {
        uint64_t h = TargetGraph::hashNode(mapped);
        dst_egraph.op_payloads_[op_id].general_nodes.emplace_back(
            h, std::move(mapped));
        auto [_, ins] = dst_egraph.memo_index_.findOrInsert(
            h,
            [&](uint32_t j) {
              const auto &om = dst_egraph.enode_meta_[j];
              const auto &og =
                  dst_egraph.op_payloads_[om.opId()].general_nodes[om.op_row];
              const auto &cur =
                  dst_egraph.op_payloads_[op_id].general_nodes[row_idx].node;
              return og.hash == h && og.node == cur;
            },
            [&]() { return i; },
            [&](uint32_t j) {
              const auto &om = dst_egraph.enode_meta_[j];
              return dst_egraph.op_payloads_[om.opId()]
                  .general_nodes[om.op_row]
                  .hash;
            });
        if (ins) {
          ++dst_egraph.total_memo_entries_;
        }
      }
    }

    dst_egraph.class_meta_.resize(src_egraph.class_meta_.size());
    for (size_t i = 0; i < src_egraph.class_meta_.size(); ++i) {
      dst_egraph.class_meta_[i].node_head = src_egraph.class_meta_[i].node_head;
      dst_egraph.class_meta_[i].node_count_alive =
          src_egraph.class_meta_[i].node_count_alive;
    }

    if constexpr (!TargetGraph::kIsNoAnalysis) {
      src_egraph.ensureParentEdgesMaterialized();
      dst_egraph.parent_meta_.resize(src_egraph.parent_meta_.size());
      for (size_t i = 0; i < src_egraph.parent_meta_.size(); ++i) {
        dst_egraph.parent_meta_[i].parent_head =
            src_egraph.parent_meta_[i].parent_head;
        dst_egraph.parent_meta_[i].parent_count =
            src_egraph.parent_meta_[i].parent_count;
      }
      dst_egraph.parent_edge_pool_.resize(src_egraph.parent_edge_pool_.size());
      for (size_t i = 0; i < src_egraph.parent_edge_pool_.size(); ++i) {
        dst_egraph.parent_edge_pool_[i].nidx =
            src_egraph.parent_edge_pool_[i].nidx;
        dst_egraph.parent_edge_pool_[i].next =
            src_egraph.parent_edge_pool_[i].next;
      }
      dst_egraph.parent_indexed_until_ =
          static_cast<uint32_t>(dst_egraph.enode_meta_.size());
    } else {
      dst_egraph.parent_indexed_until_ = 0;
    }
    dst_egraph.has_unions_since_rebuild_ = src_egraph.has_unions_since_rebuild_;

    dst_egraph.dirty_enodes_ = src_egraph.dirty_enodes_;
    dst_egraph.rebuilt_enodes_ = src_egraph.rebuilt_enodes_;
    dst_egraph.owner_changed_enodes_ = src_egraph.owner_changed_enodes_;
    for (const auto &id : src_egraph.analysis_pending_) {
      dst_egraph.analysis_pending_.insert(id);
    }
    dst_egraph.clean_ = src_egraph.clean_;
    dst_egraph.live_node_count_ = src_egraph.live_node_count_;
    dst_egraph.active_class_ids_ = src_egraph.active_class_ids_;
    dst_egraph.active_class_positions_ = src_egraph.active_class_positions_;
    dst_egraph.class_ids_dirty_ = true;
    dst_egraph.classes_by_op_dirty_ = true;
    dst_egraph.explanations_enabled_ = false;
    dst_egraph.optimize_explanation_lengths_ = true;
    dst_egraph.class_count_ = src_egraph.class_count_;

    if constexpr (!TargetGraph::kIsNoAnalysis) {
      dst_egraph.classes_.resize(src_egraph.class_meta_.size());
      for (Id id : src_egraph.active_class_ids_) {
        const auto &klass = src_egraph.classAtConst(id);
        dst_egraph.classes_[id.index()] = mapEClass(klass);
      }
    }

    return dst_egraph;
  }
};

template <typename SrcL, typename SrcA, typename DstL, typename DstA>
struct SimpleLanguageMapper final : LanguageMapper<SrcL, SrcA, DstL, DstA> {
  DstL mapNode(const SrcL &node) const override { return DstL(node); }

  typename DstL::Discriminant mapDiscriminant(
      const typename SrcL::Discriminant &discriminant) const override {
    static_assert(std::is_constructible_v<typename DstL::Discriminant,
                                          typename SrcL::Discriminant>,
                  "SimpleLanguageMapper requires a constructible target "
                  "discriminant conversion");
    return typename DstL::Discriminant(discriminant);
  }

  DstA mapAnalysis(const SrcA &analysis) const override {
    static_assert(std::is_constructible_v<DstA, SrcA>,
                  "SimpleLanguageMapper requires a constructible target "
                  "analysis conversion");
    return DstA(analysis);
  }

  typename DstA::Data mapData(const typename SrcA::Data &data) const override {
    static_assert(
        std::is_constructible_v<typename DstA::Data, typename SrcA::Data>,
        "SimpleLanguageMapper requires a constructible target "
        "analysis data conversion");
    return typename DstA::Data(data);
  }
};

template <typename L, typename AnalysisT>
inline Id EGraph<L, AnalysisT>::addInstantiation(const PatternAst<L> &pat,
                                                 const Subst &subst) {
  return find(addInstantiationNoncanonical(pat, subst));
}

template <typename L, typename AnalysisT>
inline std::pair<Id, bool> EGraph<L, AnalysisT>::unionInstantiations(
    const PatternAst<L> &from_pat, const PatternAst<L> &to_pat,
    const Subst &subst, const Symbol &reason) {
  Id from = addInstantiationNoncanonical(from_pat, subst);
  Id to = addInstantiationNoncanonical(to_pat, subst);
  return uniteImpl(from, to, Justification::ruleJustification(reason));
}

template <typename L, typename AnalysisT>
inline std::pair<Pattern<L>, Subst> EGraph<L, AnalysisT>::idToPattern(
    Id id, const std::unordered_map<Id, Id> &substitutions) const {
  RecExpr<ENodeOrVar<L>> pat;
  Subst subst;
  FlatHashMap<Id, Id> cache;
  idToPatternInternal(pat, id, substitutions, subst, cache);
  return {Pattern<L>(std::move(pat)), subst};
}

template <typename L, typename AnalysisT>
inline Id
EGraph<L, AnalysisT>::addInstantiationNoncanonical(const PatternAst<L> &pat,
                                                   const Subst &subst) {
  llvm::SmallVector<Id, 16> ids;
  ids.reserve(pat.size());
  for (const auto &node : pat.items()) {
    if (node.isVar()) {
      ids.push_back(find(subst.at(node.var())));
    } else {
      ids.push_back(addUncanonical(
          node.node().mapChildren([&](Id id) { return ids[id.index()]; })));
    }
  }
  return ids.back();
}

template <typename L, typename AnalysisT>
inline Id EGraph<L, AnalysisT>::idToPatternInternal(
    RecExpr<ENodeOrVar<L>> &pat, Id id,
    const std::unordered_map<Id, Id> &substitutions, Subst &subst,
    FlatHashMap<Id, Id> &cache) const {
  if (auto cached = cache.find(id); cached != cache.end()) {
    return cached->second;
  }

  if (auto sub_it = substitutions.find(id); sub_it != substitutions.end()) {
    Var var("?" + std::to_string(id.value()));
    subst.insert(var, sub_it->second);
    Id added = pat.add(ENodeOrVar<L>(var));
    cache.try_emplace(id, added);
    return added;
  }

  ensureNodesMaterialized();
  const auto &node = nodes_.at(id.index());
  std::vector<Id> children;
  children.reserve(node.children().size());
  for (Id child : node.children()) {
    children.push_back(
        idToPatternInternal(pat, child, substitutions, subst, cache));
  }
  auto rebuilt =
      node.mapChildren([&](Id child) { return children[child.index()]; });
  Id added = pat.add(ENodeOrVar<L>(rebuilt));
  cache.try_emplace(id, added);
  return added;
}

} // namespace lotus::egraph
