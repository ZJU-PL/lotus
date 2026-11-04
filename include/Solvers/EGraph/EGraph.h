#pragma once

#include "Solvers/EGraph/Analysis.h"
#include "Solvers/EGraph/EClass.h"
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

template <typename L, typename AnalysisT> class EGraph {
public:
  using Analysis = AnalysisT;
  using Data = typename AnalysisT::Data;
  using Class = EClass<L, Data>;

  EGraph() { configureHashTables(); }
  explicit EGraph(AnalysisT analysis) : analysis_(std::move(analysis)) {
    configureHashTables();
  }

  AnalysisT &analysis() { return analysis_; }
  const AnalysisT &analysis() const { return analysis_; }

  std::vector<std::reference_wrapper<const Class>> classes() const {
    assertClean();
    std::vector<std::reference_wrapper<const Class>> out;
    out.reserve(class_count_);
    for (Id id : class_ids_) {
      out.emplace_back(classAt(id));
    }
    return out;
  }

  std::vector<std::reference_wrapper<Class>> classesMut() {
    std::vector<std::reference_wrapper<Class>> out;
    out.reserve(class_count_);
    for (auto &klass : classes_) {
      if (klass) {
        out.emplace_back(*klass);
      }
    }
    return out;
  }

  const std::vector<Id> &
  classesForOp(const typename L::Discriminant &op) const {
    assertClean();
    auto it = classes_by_op_.find(op);
    if (it == classes_by_op_.end()) {
      static const std::vector<Id> empty;
      return empty;
    }
    return it->second;
  }

  const std::vector<L> &nodes() const { return nodes_; }
  const L &originalNode(Id id) const { return nodes_.at(id.index()); }

  bool empty() const { return memo_.empty(); }
  bool clean() const { return clean_; }
  size_t totalSize() const { return live_node_count_; }
  size_t memoSize() const { return memo_.size(); }
  size_t totalNumberOfNodes() const { return live_node_count_; }
  size_t numberOfClasses() const { return class_count_; }

  Id find(Id id) const { return union_find_.find(id); }
  Id findMut(Id id) { return union_find_.findMut(id); }

  Class &operator[](Id id) { return classAt(findMut(id)); }
  const Class &operator[](Id id) const { return classAt(find(id)); }

  const std::vector<Id> &classIds() const {
    assertClean();
    return class_ids_;
  }

  EGraph withExplanationsEnabled() const {
    if (explanations_enabled_) {
      return *this;
    }
    if (!nodes_.empty()) {
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

  bool areExplanationsEnabled() const { return explanations_enabled_; }
  bool optimizeExplanationLengths() const {
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

    std::unordered_map<std::pair<Id, Id>, Id, PairHash> product_map;
    std::unordered_set<std::pair<Id, Id>, PairHash> visited_pairs;
    std::vector<std::pair<L, Id>> enodes;

    for (const auto &[discriminant, left_ids] : classes_by_op_) {
      auto right_it = other.classes_by_op_.find(discriminant);
      if (right_it == other.classes_by_op_.end()) {
        continue;
      }
      for (Id left : left_ids) {
        for (Id right : right_it->second) {
          if (visited_pairs.emplace(left, right).second) {
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

  Id add(const L &node) { return find(addUncanonical(node)); }

  Id addUncanonical(const L &node) {
    clean_ = false;

    L canonical = node;
    canonicalizeInPlace(canonical);
    if (auto existing = memoLookup(canonical)) {
      if (!explanations_enabled_) {
        return *existing;
      }

      auto uncanon_it = uncanonical_memo_.find(node);
      if (uncanon_it != uncanonical_memo_.end()) {
        return uncanon_it->second;
      }

      Id new_id = union_find_.makeSet();
      ensureNodeStorage(new_id, node);
      uncanonical_memo_.emplace(node, new_id);
      union_find_.unite(find(*existing), new_id);
      recordExplanation(*existing, new_id, Justification::congruence());
      recordUnion(*existing, new_id, Justification::congruence());
      return new_id;
    }

    return makeNewEClass(canonical, node);
  }

  Id addExpr(const RecExpr<L> &expr) { return find(addExprUncanonical(expr)); }

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
    std::optional<Justification> justification;
    if (reason != Symbol()) {
      justification = Justification::ruleJustification(reason);
    }
    auto [id, _] = uniteImpl(lhs, rhs, justification);
    return id;
  }

  std::pair<Id, bool> uniteChecked(Id lhs, Id rhs, const Symbol &reason = {}) {
    std::optional<Justification> justification;
    if (reason != Symbol()) {
      justification = Justification::ruleJustification(reason);
    }
    return uniteImpl(lhs, rhs, justification);
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

    std::unordered_set<Id> rhs_classes;
    rhs_classes.reserve(matches2.size());
    for (const auto &rhs : matches2) {
      rhs_classes.insert(find(rhs.eclass));
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

      json11::Json::array parents_json;
      parents_json.reserve(klass.parents.size());
      for (Id parent : klass.parents) {
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
        {"memo_size", static_cast<double>(memo_.size())},
        {"clean", clean_},
        {"explanations_enabled", explanations_enabled_},
    };
    return root;
  }

  static EGraph fromJson(const json11::Json &json,
                         AnalysisT analysis = AnalysisT()) {
    std::string error;
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

    std::unordered_map<Id, size_t> choices;
    std::unordered_set<Id> visiting;
    Id canonical = find(id);
    if (!chooseFiniteNode(canonical, choices, visiting)) {
      throw std::runtime_error(
          "E-class has no finite expression representative");
    }

    std::unordered_map<Id, Id> cache;
    RecExpr<L> expr;
    materializeFiniteExpr(expr, canonical, choices, cache);
    return expr;
  }

  RecExpr<L> originalExpr(Id id) const {
    std::unordered_map<Id, Id> cache;
    RecExpr<L> expr;
    originalExprInternal(expr, id, cache);
    return expr;
  }

  RecExpr<L> idToExprFlat(Id id) const {
    RecExpr<L> expr;
    const auto &node = classAt(find(id)).nodes.front();
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
    std::unordered_set<std::pair<Id, Id>, PairHash> edges;
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
        edges.emplace(a, b);
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
    auto &klass = classAt(canonical);
    klass.data = std::move(data);
    analysis_pending_.extend(klass.parents);
    analysis_.modify(*this, canonical);
  }

  size_t rebuild() {
    size_t unions = processPending();
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

  void assertClean() const {
    if (!clean_) {
      throw std::runtime_error(
          "Tried to query a dirty e-graph; call rebuild() first");
    }
  }

  void configureHashTables() {
    memo_.max_load_factor(0.7f);
    uncanonical_memo_.max_load_factor(0.7f);
    original_node_ids_.max_load_factor(0.7f);
    classes_by_op_.max_load_factor(0.7f);
  }

  bool hasClass(Id id) const {
    return id.index() < classes_.size() && classes_[id.index()].has_value();
  }

  Class &classAt(Id id) { return classes_.at(id.index()).value(); }
  const Class &classAt(Id id) const { return classes_.at(id.index()).value(); }

  static void insertSortedUnique(std::vector<Id> &ids, Id id) {
    auto position = std::lower_bound(ids.begin(), ids.end(), id);
    if (position == ids.end() || *position != id) {
      ids.insert(position, id);
    }
  }

  static void eraseSorted(std::vector<Id> &ids, Id id) {
    auto position = std::lower_bound(ids.begin(), ids.end(), id);
    if (position != ids.end() && *position == id) {
      ids.erase(position);
    }
  }

  std::vector<Id> canonicalChildren(const std::vector<L> &nodes) {
    std::vector<Id> children;
    for (const auto &node : nodes) {
      for (Id child : node.children()) {
        children.push_back(findMut(child));
      }
    }
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()),
                   children.end());
    return children;
  }

  std::vector<typename L::Discriminant>
  classDiscriminants(const std::vector<L> &nodes) const {
    std::vector<typename L::Discriminant> discriminants;
    for (const auto &node : nodes) {
      auto discriminant = node.discriminant();
      if (std::find(discriminants.begin(), discriminants.end(), discriminant) ==
          discriminants.end()) {
        discriminants.push_back(std::move(discriminant));
      }
    }
    return discriminants;
  }

  void addClassToOp(const typename L::Discriminant &op, Id id) {
    insertSortedUnique(classes_by_op_[op], id);
  }

  void removeClassFromOp(const typename L::Discriminant &op, Id id) {
    auto found = classes_by_op_.find(op);
    if (found == classes_by_op_.end()) {
      return;
    }
    eraseSorted(found->second, id);
    if (found->second.empty()) {
      classes_by_op_.erase(found);
    }
  }

  void ensureNodeStorage(Id id, const L &node) {
    if (nodes_.size() != id.index()) {
      throw std::runtime_error("EGraph node storage out of sync");
    }
    nodes_.push_back(node);
    if (explanations_enabled_) {
      original_node_ids_.try_emplace(node, id);
    }
    if (explanations_enabled_) {
      if (explanation_nodes_.size() != id.index()) {
        throw std::runtime_error("EGraph explanation node storage out of sync");
      }
      explanation_nodes_.push_back(ExplanationNode{
          {},
          ExplanationConnection{Justification::congruence(), false, id, id}});
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

  void canonicalizeInPlace(L &node) const {
    for (Id &child : node.childrenMut()) {
      child = find(child);
    }
  }

  std::optional<Id> memoLookup(const L &node) const {
    auto it = memo_.find(node);
    if (it == memo_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  static Id getProductId(
      Id class1, Id class2,
      std::unordered_map<std::pair<Id, Id>, Id, PairHash> &product_map) {
    auto key = std::make_pair(class1, class2);
    if (auto it = product_map.find(key); it != product_map.end()) {
      return it->second;
    }
    Id id = Id::fromIndex(product_map.size());
    product_map.emplace(key, id);
    return id;
  }

  enum class UnresolvedPolicy { Drop, Reject };

  static EGraph fromEnodes(std::vector<std::pair<L, Id>> enodes,
                           AnalysisT analysis,
                           UnresolvedPolicy unresolved_policy) {
    EGraph egraph(std::move(analysis));
    std::unordered_map<Id, Id> ids;
    std::unordered_map<Id, std::vector<size_t>> waiters;
    std::vector<size_t> missing(enodes.size(), 0);
    std::vector<bool> processed(enodes.size(), false);
    std::deque<size_t> ready;
    size_t remaining = enodes.size();

    for (size_t i = 0; i < enodes.size(); ++i) {
      std::vector<Id> dependencies(enodes[i].first.children().begin(),
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
      L mapped = enode.mapChildren([&](Id child) { return ids.at(child); });
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
      processed[index] = true;
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
      std::unordered_map<std::pair<Id, Id>, Id, PairHash> &product_map) const {
    const auto &left = classAt(class1);
    const auto &right = other.classAt(class2);
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

  Id makeNewEClass(const L &canonical, const L &original) {
    Id id = union_find_.makeSet();
    ensureNodeStorage(id, original);

    Class klass{id, {canonical}, AnalysisT::make(*this, original, id), {}, nullptr};
    klass.nodes_dirty = false;

    if (classes_.size() <= id.index()) {
      classes_.resize(id.index() + 1);
    }
    classes_[id.index()] = std::move(klass);
    if (active_class_positions_.size() <= id.index()) {
      active_class_positions_.resize(id.index() + 1,
                                     std::numeric_limits<uint32_t>::max());
    }
    active_class_positions_[id.index()] =
        static_cast<uint32_t>(active_class_ids_.size());
    active_class_ids_.push_back(id);
    ++class_count_;
    for (Id child : canonicalChildren(classes_[id.index()]->nodes)) {
      insertSortedUnique(classAt(child).parents, id);
    }
    addClassToOp(canonical.discriminant(), id);
    memo_[canonical] = id;
    ++live_node_count_;
    analysis_.modify(*this, id);
    return id;
  }

  Id addInstantiationNoncanonical(const PatternAst<L> &pat, const Subst &subst);

  std::pair<Id, bool>
  uniteImpl(Id lhs, Id rhs, const std::optional<Justification> &justification) {
    analysis_.preUnion(*this, lhs, rhs, justification);

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

    const auto &lhs_before = classAt(lhs_class);
    const auto &rhs_before = classAt(rhs_class);
    if (lhs_before.parents.size() < rhs_before.parents.size() ||
        (lhs_before.parents.size() == rhs_before.parents.size() &&
         lhs_before.nodes.size() < rhs_before.nodes.size())) {
      std::swap(lhs_class, rhs_class);
    }

    recordExplanation(original_lhs, original_rhs, justification);
    recordUnion(original_lhs, original_rhs, justification);
    union_find_.unite(lhs_class, rhs_class);

    auto right_class = std::move(classAt(rhs_class));
    classes_[rhs_class.index()].reset();
    size_t removed_position = active_class_positions_.at(rhs_class.index());
    Id moved_id = active_class_ids_.back();
    active_class_ids_[removed_position] = moved_id;
    active_class_positions_[moved_id.index()] =
        static_cast<uint32_t>(removed_position);
    active_class_ids_.pop_back();
    active_class_positions_[rhs_class.index()] =
        std::numeric_limits<uint32_t>::max();
    --class_count_;
    auto &left_class = classAt(lhs_class);

    pending_.extend(right_class.parents);
    DidMerge did_merge =
        analysis_.merge(left_class.data, std::move(right_class.data));
    if (did_merge.left_changed) {
      analysis_pending_.extend(left_class.parents);
    }
    if (did_merge.right_changed) {
      analysis_pending_.extend(right_class.parents);
    }

    size_t left_parent_count = left_class.parents.size();
    left_class.parents.reserve(left_parent_count + right_class.parents.size());
    left_class.parents.insert(left_class.parents.end(),
                              right_class.parents.begin(),
                              right_class.parents.end());
    std::inplace_merge(left_class.parents.begin(),
                       left_class.parents.begin() + left_parent_count,
                       left_class.parents.end());
    left_class.parents.erase(
        std::unique(left_class.parents.begin(), left_class.parents.end()),
        left_class.parents.end());

    for (const auto &op : classDiscriminants(right_class.nodes)) {
      removeClassFromOp(op, rhs_class);
      addClassToOp(op, lhs_class);
    }
    for (Id child : canonicalChildren(right_class.nodes)) {
      auto &parents = classAt(child).parents;
      eraseSorted(parents, rhs_class);
      insertSortedUnique(parents, lhs_class);
    }

    left_class.nodes.reserve(left_class.nodes.size() +
                             right_class.nodes.size());
    left_class.nodes.insert(left_class.nodes.end(),
                            std::make_move_iterator(right_class.nodes.begin()),
                            std::make_move_iterator(right_class.nodes.end()));
    left_class.matching_nodes.reset();
    left_class.nodes_dirty = true;
    rebuild_pending_.insert(lhs_class);

    analysis_.modify(*this, lhs_class);
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
    class_ids_ = active_class_ids_;
    std::sort(class_ids_.begin(), class_ids_.end());

    while (auto dirty_id = rebuild_pending_.pop()) {
      Id owner = findMut(*dirty_id);
      if (!hasClass(owner)) {
        continue;
      }
      auto &klass = classAt(owner);
      if (!klass.nodes_dirty) {
        continue;
      }

      size_t old_size = klass.nodes.size();
      for (auto &node : klass.nodes) {
        canonicalizeInPlace(node);
      }
      std::sort(klass.nodes.begin(), klass.nodes.end());
      klass.nodes.erase(std::unique(klass.nodes.begin(), klass.nodes.end()),
                        klass.nodes.end());
      live_node_count_ -= old_size - klass.nodes.size();
      klass.rebuildMatchingIndex();
      klass.nodes_dirty = false;
    }
  }

  size_t repairCongruence() {
    size_t unions = 0;
    UniqueQueue<Id> dirty;
    dirty.extend(pending_);
    pending_.clear();

    while (!dirty.empty() || !pending_.empty()) {
      if (!pending_.empty()) {
        dirty.extend(pending_);
        pending_.clear();
      }
      auto dirty_id = dirty.pop();
      if (!dirty_id) {
        continue;
      }

      Id owner = findMut(*dirty_id);
      if (!hasClass(owner)) {
        continue;
      }
      classAt(owner).nodes_dirty = true;
      rebuild_pending_.insert(owner);
      std::vector<L> nodes = classAt(owner).nodes;

      for (const auto &old_node : nodes) {
        auto old = memo_.find(old_node);
        if (old != memo_.end() && find(old->second) == owner) {
          memo_.erase(old);
        }
      }

      for (auto node : nodes) {
        canonicalizeInPlace(node);
        owner = findMut(owner);
        auto [memo_it, inserted] = memo_.try_emplace(node, owner);
        if (inserted) {
          continue;
        }
        Id existing = findMut(memo_it->second);
        if (existing == owner) {
          continue;
        }
        auto [merged_owner, changed] =
            uniteImpl(existing, owner, Justification::congruence());
        memo_it->second = merged_owner;
        owner = merged_owner;
        unions += changed ? 1 : 0;
      }
    }

    if (explanations_enabled_) {
      std::unordered_map<L, Id, LanguageHash<L>> explanation_memo;
      explanation_memo.reserve(memo_.size());
      for (const auto &[original_node, original] : original_node_ids_) {
        L node = original_node;
        canonicalizeInPlace(node);
        auto live = memo_.find(node);
        if (live == memo_.end()) {
          continue;
        }
        if (find(original) == find(live->second)) {
          explanation_memo.try_emplace(std::move(node), original);
        }
      }
      for (const auto &[node, id] : memo_) {
        explanation_memo.try_emplace(node, findMut(id));
      }
      memo_ = std::move(explanation_memo);
    }
    return unions;
  }

  size_t processPending() {
    size_t unions = 0;
    bool repair_needed = !clean_ || !pending_.empty();

    while (repair_needed || !analysis_pending_.empty()) {
      if (repair_needed) {
        unions += repairCongruence();
        rebuildClasses();
        pending_.clear();
        repair_needed = false;
      }

      while (!analysis_pending_.empty()) {
        auto class_id_opt = analysis_pending_.pop();
        if (!class_id_opt) {
          break;
        }

        Id canonical = findMut(*class_id_opt);
        if (!hasClass(canonical)) {
          continue;
        }

        auto nodes = classAt(canonical).nodes;
        bool data_changed = false;
        for (const auto &node : nodes) {
          Data node_data = AnalysisT::remake(*this, node, canonical);
          DidMerge did_merge =
              analysis_.merge(classAt(canonical).data, std::move(node_data));
          data_changed = data_changed || did_merge.left_changed;
        }

        if (data_changed) {
          analysis_pending_.extend(classAt(canonical).parents);
          analysis_.modify(*this, canonical);
        }
      }

      repair_needed = !pending_.empty();
    }

    return unions;
  }

  bool chooseFiniteNode(Id id, std::unordered_map<Id, size_t> &choices,
                        std::unordered_set<Id> &visiting) const {
    Id canonical = find(id);
    if (choices.count(canonical)) {
      return true;
    }
    if (!visiting.insert(canonical).second) {
      return false;
    }

    const auto &klass = classAt(canonical);
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
        choices.emplace(canonical, index);
        visiting.erase(canonical);
        return true;
      }
    }

    visiting.erase(canonical);
    return false;
  }

  Id materializeFiniteExpr(RecExpr<L> &expr, Id id,
                           const std::unordered_map<Id, size_t> &choices,
                           std::unordered_map<Id, Id> &cache) const {
    Id canonical = find(id);
    if (auto it = cache.find(canonical); it != cache.end()) {
      return it->second;
    }

    const auto &node = classAt(canonical).nodes[choices.at(canonical)];
    auto materialized = node.mapChildren([&](Id child) {
      return materializeFiniteExpr(expr, child, choices, cache);
    });
    Id added = expr.add(materialized);
    cache.emplace(canonical, added);
    return added;
  }

#ifndef NDEBUG
  void assertCongruenceInvariant() const {
    std::unordered_map<L, Id, LanguageHash<L>> owners;
    owners.reserve(totalNumberOfNodes());
    std::vector<std::vector<Id>> expected_parents(classes_.size());
    std::unordered_map<typename L::Discriminant, std::vector<Id>> expected_ops;

    for (Id class_id : active_class_ids_) {
      const auto &klass = classAt(class_id);
      Id id = klass.id;
      assert(find(id) == id);
      for (auto node : klass.nodes) {
        canonicalizeInPlace(node);
        auto [it, inserted] = owners.emplace(node, id);
        assert(inserted || find(it->second) == id);
        for (Id child : node.children()) {
          expected_parents[find(child).index()].push_back(id);
        }
        expected_ops[node.discriminant()].push_back(id);
      }
    }

    assert(owners.size() == memo_.size());
    for (const auto &[node, id] : memo_) {
      auto it = owners.find(node);
      assert(it != owners.end());
      assert(find(it->second) == find(id));
    }

    for (Id id : active_class_ids_) {
      auto &parents = expected_parents[id.index()];
      std::sort(parents.begin(), parents.end());
      parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
      assert(parents == classAt(id).parents);
    }
    for (auto &[_, ids] : expected_ops) {
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    assert(expected_ops == classes_by_op_);
  }
#endif

  Id originalExprInternal(RecExpr<L> &expr, Id id,
                          std::unordered_map<Id, Id> &cache) const {
    if (auto it = cache.find(id); it != cache.end()) {
      return it->second;
    }

    const auto &node = nodes_.at(id.index());
    auto materialized = node.mapChildren(
        [&](Id child) { return originalExprInternal(expr, child, cache); });
    Id added = expr.add(materialized);
    cache.emplace(id, added);
    return added;
  }

  Id idToPatternInternal(RecExpr<ENodeOrVar<L>> &pat, Id id,
                         const std::unordered_map<Id, Id> &substitutions,
                         Subst &subst, std::unordered_map<Id, Id> &cache) const;

  AnalysisT analysis_;
  UnionFind union_find_;
  std::vector<L> nodes_;
  std::unordered_map<L, Id, LanguageHash<L>> original_node_ids_;
  std::unordered_map<L, Id, LanguageHash<L>> memo_;
  std::unordered_map<L, Id, LanguageHash<L>> uncanonical_memo_;
  UniqueQueue<Id> pending_;
  UniqueQueue<Id> rebuild_pending_;
  UniqueQueue<Id> analysis_pending_;
  std::vector<std::optional<Class>> classes_;
  size_t class_count_ = 0;
  std::vector<Id> active_class_ids_;
  std::vector<uint32_t> active_class_positions_;
  std::vector<Id> class_ids_;
  std::unordered_map<typename L::Discriminant, std::vector<Id>> classes_by_op_;
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
    TargetGraph dst_egraph(mapAnalysis(src_egraph.analysis_));
    dst_egraph.union_find_ = src_egraph.union_find_;
    dst_egraph.nodes_.reserve(src_egraph.nodes_.size());
    for (const auto &node : src_egraph.nodes_) {
      dst_egraph.nodes_.push_back(mapNode(node));
    }

    for (const auto &[node, id] : src_egraph.memo_) {
      dst_egraph.memo_.emplace(mapNode(node), id);
    }
    dst_egraph.pending_ = src_egraph.pending_;
    dst_egraph.rebuild_pending_ = src_egraph.rebuild_pending_;
    for (const auto &id : src_egraph.analysis_pending_) {
      dst_egraph.analysis_pending_.insert(id);
    }
    dst_egraph.clean_ = src_egraph.clean_;
    dst_egraph.live_node_count_ = src_egraph.live_node_count_;
    dst_egraph.active_class_ids_ = src_egraph.active_class_ids_;
    dst_egraph.active_class_positions_ = src_egraph.active_class_positions_;
    dst_egraph.class_ids_ = src_egraph.class_ids_;
    dst_egraph.explanations_enabled_ = false;
    dst_egraph.optimize_explanation_lengths_ = true;

    dst_egraph.classes_.resize(src_egraph.classes_.size());
    for (Id id : src_egraph.active_class_ids_) {
      const auto &klass = src_egraph.classAt(id);
      dst_egraph.classes_[id.index()] = mapEClass(klass);
    }
    dst_egraph.class_count_ = src_egraph.class_count_;
    for (const auto &[discriminant, ids] : src_egraph.classes_by_op_) {
      auto &mapped_ids =
          dst_egraph.classes_by_op_[mapDiscriminant(discriminant)];
      mapped_ids.insert(mapped_ids.end(), ids.begin(), ids.end());
    }
    for (auto &[_, ids] : dst_egraph.classes_by_op_) {
      std::sort(ids.begin(), ids.end());
      ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
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
  std::unordered_map<Id, Id> cache;
  idToPatternInternal(pat, id, substitutions, subst, cache);
  return {Pattern<L>(std::move(pat)), subst};
}

template <typename L, typename AnalysisT>
inline Id
EGraph<L, AnalysisT>::addInstantiationNoncanonical(const PatternAst<L> &pat,
                                                   const Subst &subst) {
  std::vector<Id> ids;
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
    std::unordered_map<Id, Id> &cache) const {
  if (auto cached = cache.find(id); cached != cache.end()) {
    return cached->second;
  }

  if (auto sub_it = substitutions.find(id); sub_it != substitutions.end()) {
    Var var("?" + std::to_string(id.value()));
    subst.insert(var, sub_it->second);
    Id added = pat.add(ENodeOrVar<L>(var));
    cache.emplace(id, added);
    return added;
  }

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
  cache.emplace(id, added);
  return added;
}

} // namespace lotus::egraph
