#include "CFL/InterleavedDyck/ILP/Solver.h"

#include "Backend.h"
#include "CFL/InterleavedDyck/LCL/Solver.h"
#include "Utils/Algorithms/PathExpressions/PathExpressionComputer.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::ilp {
namespace {

using Regex = lotus::pathexpressions::IRegex<std::string>;
using RegexRef = lotus::pathexpressions::RegexRef<std::string>;
using RegexGraph =
    lotus::pathexpressions::GenericLabeledGraph<Vertex, std::string>;

struct RegexBuilder final
    : lotus::pathexpressions::IRegexVisitor<std::string, int, std::nullptr_t> {
  explicit RegexBuilder(std::size_t max_nodes = 0) : max_nodes_(max_nodes) {}
  int visit(const lotus::pathexpressions::Union<std::string> &re,
            std::nullptr_t) override {
    return make(Kind::Or, build(re.getFirst()), build(re.getSecond()));
  }
  int visit(const lotus::pathexpressions::Concatenation<std::string> &re,
            std::nullptr_t) override {
    return make(Kind::Concat, build(re.getFirst()), build(re.getSecond()));
  }
  int visit(const lotus::pathexpressions::Star<std::string> &re,
            std::nullptr_t) override {
    return make(Kind::Star, build(re.getInner()));
  }
  int visit(const lotus::pathexpressions::Literal<std::string> &re,
            std::nullptr_t) override {
    return makeTerminal(Label::parse(re.getLetter()));
  }
  int visit(const lotus::pathexpressions::Epsilon<std::string> &,
            std::nullptr_t) override {
    return make(Kind::Epsilon);
  }
  int visit(const lotus::pathexpressions::EmptySet<std::string> &,
            std::nullptr_t) override {
    return make(Kind::Empty);
  }

  enum class Kind { Terminal, Concat, Or, Star, Epsilon, Empty };
  struct Node {
    Kind kind = Kind::Epsilon;
    int left = -1;
    int right = -1;
    Label label;
  };

  std::vector<Node> nodes;
  std::unordered_map<std::string, int> interned;
  std::unordered_map<const Regex *, int> converted;
  std::size_t max_nodes_ = 0;

  void checkNodeLimit() const {
    if (max_nodes_ != 0 && nodes.size() >= max_nodes_) {
      throw std::length_error("ILP: path-expression node limit exceeded");
    }
  }

  int build(const RegexRef &re) {
    auto found = converted.find(re.get());
    if (found != converted.end()) return found->second;
    const int id = re->accept(*this, nullptr);
    converted.emplace(re.get(), id);
    return id;
  }

  int make(Kind kind, int left = -1, int right = -1) {
    const std::string key = std::to_string(static_cast<unsigned>(kind)) + ":" +
                            std::to_string(left) + ":" + std::to_string(right);
    auto it = interned.find(key);
    if (it != interned.end()) return it->second;
    checkNodeLimit();
    const int id = static_cast<int>(nodes.size());
    nodes.push_back({kind, left, right, Label::neutral()});
    interned.emplace(key, id);
    return id;
  }

  int makeTerminal(Label label) {
    const std::string key = "T:" + label.str();
    auto it = interned.find(key);
    if (it != interned.end()) return it->second;
    checkNodeLimit();
    const int id = static_cast<int>(nodes.size());
    nodes.push_back({Kind::Terminal, -1, -1, label});
    interned.emplace(key, id);
    return id;
  }
};

class StandardForm {
public:
  explicit StandardForm(RegexBuilder &builder, std::size_t max_disjuncts)
      : b_(builder), max_disjuncts_(max_disjuncts) {}

  std::vector<int> expand(int id) {
    auto it = memo_.find(id);
    if (it != memo_.end()) return it->second;
    // Expansion interns new nodes and may reallocate b_.nodes; keep a value
    // copy rather than a reference across recursive calls/interning.
    const auto n = b_.nodes.at(static_cast<std::size_t>(id));
    std::vector<int> result;
    switch (n.kind) {
    case RegexBuilder::Kind::Empty:
      break;
    case RegexBuilder::Kind::Terminal:
    case RegexBuilder::Kind::Epsilon:
      result.push_back(id);
      break;
    case RegexBuilder::Kind::Or: {
      result = expand(n.left);
      auto rhs = expand(n.right);
      checkDisjunctLimit(result.size() + rhs.size());
      result.insert(result.end(), rhs.begin(), rhs.end());
      break;
    }
    case RegexBuilder::Kind::Concat: {
      const auto lhs = expand(n.left);
      const auto rhs = expand(n.right);
      for (int a : lhs) {
        for (int c : rhs) {
          checkDisjunctLimit(result.size() + 1);
          result.push_back(
              b_.make(RegexBuilder::Kind::Concat, a, c));
        }
      }
      break;
    }
    case RegexBuilder::Kind::Star: {
      const auto alternatives = expand(n.left);
      if (alternatives.empty()) {
        result.push_back(b_.make(RegexBuilder::Kind::Epsilon));
      } else {
        int product = b_.make(RegexBuilder::Kind::Epsilon);
        for (int alternative : alternatives) {
          const int starred = b_.make(RegexBuilder::Kind::Star, alternative);
          product = b_.make(RegexBuilder::Kind::Concat, product, starred);
        }
        result.push_back(b_.make(RegexBuilder::Kind::Star, product));
      }
      break;
    }
    }
    memo_.emplace(id, result);
    return result;
  }

private:
  void checkDisjunctLimit(std::size_t count) const {
    if (max_disjuncts_ != 0 && count > max_disjuncts_) {
      throw std::length_error("ILP: standard-form disjunct limit exceeded");
    }
  }
  RegexBuilder &b_;
  std::size_t max_disjuncts_ = 0;
  std::unordered_map<int, std::vector<int>> memo_;
};

struct Dimension {
  bool bracket = false;
  unsigned id = 0;
  bool operator==(const Dimension &o) const {
    return bracket == o.bracket && id == o.id;
  }
};
struct DimensionHash {
  std::size_t operator()(const Dimension &d) const {
    return (std::hash<unsigned>{}(d.id) << 1U) ^ std::hash<bool>{}(d.bracket);
  }
};

using Linear = std::unordered_map<int, std::int64_t>;
struct Count {
  std::int64_t constant = 0;
  Linear variables;
};
struct NodeCount {
  bool bad = false;
  std::vector<Count> cells;
};
struct Variable {
  bool nonnegative = false;
};

std::int64_t checkedAdd(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    throw std::overflow_error("ILP parenthesis-count overflow");
  }
  return left + right;
}

void addScaled(Count &to, const Count &from, std::int64_t scale = 1) {
  if (scale != 1 && scale != -1) {
    throw std::logic_error("ILP count scaling must be +1 or -1");
  }
  if (scale == 1) {
    to.constant = checkedAdd(to.constant, from.constant);
  } else {
    if (from.constant == std::numeric_limits<std::int64_t>::min()) {
      throw std::overflow_error("ILP parenthesis-count overflow");
    }
    to.constant = checkedAdd(to.constant, -from.constant);
  }
  for (const auto &entry : from.variables) {
    if (scale == -1 && entry.second == std::numeric_limits<std::int64_t>::min()) {
      throw std::overflow_error("ILP coefficient overflow");
    }
    const std::int64_t delta = scale == 1 ? entry.second : -entry.second;
    const auto found = to.variables.find(entry.first);
    const std::int64_t old = found == to.variables.end() ? 0 : found->second;
    const std::int64_t coefficient = checkedAdd(old, delta);
    if (coefficient == 0) to.variables.erase(entry.first);
    else to.variables[entry.first] = coefficient;
  }
}

class Encoder {
public:
  Encoder(const RegexBuilder &builder, const std::vector<Dimension> &dimensions,
          const std::vector<bool> &bad, bool allow_unmatched_opens,
          Statistics &stats, SolverBackend &backend)
      : b_(builder), dimensions_(dimensions), bad_(bad),
        allow_unmatched_opens_(allow_unmatched_opens), stats_(stats),
        backend_(backend) {}

  bool feasible(int root) {
    std::vector<int> order;
    std::vector<bool> seen(b_.nodes.size(), false);
    std::function<void(int)> visit = [&](int id) {
      if (seen.at(static_cast<std::size_t>(id))) return;
      seen[static_cast<std::size_t>(id)] = true;
      const auto &n = b_.nodes.at(static_cast<std::size_t>(id));
      if (n.left >= 0) visit(n.left);
      if (n.right >= 0) visit(n.right);
      order.push_back(id);
    };
    visit(root);

    for (int id : order) compute(id);
    const auto &final = counts_.at(static_cast<std::size_t>(root));
    if (final.bad) return false;

    LinearProgram program;
    program.variables.reserve(variable_info_.size());
    for (const Variable &variable : variable_info_) {
      program.variables.push_back({variable.nonnegative});
      if (variable.nonnegative) ++stats_.integer_constraints;
    }
    for (const Count &cell : final.cells) {
      LinearProgram::Constraint constraint;
      constraint.rhs = -cell.constant;
      constraint.sense = allow_unmatched_opens_
                             ? LinearProgram::Sense::GreaterEqual
                             : LinearProgram::Sense::Equal;
      constraint.coefficients.assign(cell.variables.begin(),
                                     cell.variables.end());
      program.constraints.push_back(std::move(constraint));
      ++stats_.integer_constraints;
    }
    ++stats_.ilp_disjuncts;
    stats_.integer_variables += variable_info_.size();
    switch (backend_.check(program)) {
    case Feasibility::Sat:
      return true;
    case Feasibility::Unsat:
      return false;
    case Feasibility::Unknown:
      throw std::runtime_error("ILP solver returned unknown");
    }
    throw std::logic_error("unhandled ILP feasibility result");
  }

private:
  const RegexBuilder &b_;
  const std::vector<Dimension> &dimensions_;
  const std::vector<bool> &bad_;
  bool allow_unmatched_opens_;
  Statistics &stats_;
  SolverBackend &backend_;
  std::vector<NodeCount> counts_ = std::vector<NodeCount>(b_.nodes.size());
  std::vector<Variable> variable_info_;

  int fresh(bool nonnegative) {
    const int id = static_cast<int>(variable_info_.size());
    variable_info_.push_back({nonnegative});
    return id;
  }

  void compute(int id) {
    NodeCount &out = counts_.at(static_cast<std::size_t>(id));
    const auto &node = b_.nodes.at(static_cast<std::size_t>(id));
    if (bad_.at(static_cast<std::size_t>(id))) {
      out.bad = true;
      return;
    }
    out.cells.resize(dimensions_.size());
    switch (node.kind) {
    case RegexBuilder::Kind::Terminal: {
      const bool bracket = node.label.kind == LabelKind::OpenBracket ||
                           node.label.kind == LabelKind::CloseBracket;
      const bool is_open = node.label.kind == LabelKind::OpenBracket ||
                           node.label.kind == LabelKind::OpenParenthesis;
      if (node.label.kind == LabelKind::Neutral) break;
      const Dimension key{bracket, node.label.id};
      auto it = std::find(dimensions_.begin(), dimensions_.end(), key);
      if (it == dimensions_.end()) throw std::logic_error("missing ILP dimension");
      out.cells[static_cast<std::size_t>(it - dimensions_.begin())].constant =
          is_open ? 1 : -1;
      break;
    }
    case RegexBuilder::Kind::Epsilon:
      break;
    case RegexBuilder::Kind::Empty:
      out.bad = true;
      break;
    case RegexBuilder::Kind::Or:
      throw std::logic_error("or node survived ILP standard-form conversion");
    case RegexBuilder::Kind::Concat: {
      const NodeCount &left = counts_.at(static_cast<std::size_t>(node.left));
      const NodeCount &right = counts_.at(static_cast<std::size_t>(node.right));
      if (left.bad || right.bad) {
        out.bad = true;
        break;
      }
      std::unordered_map<int, int> left_rename, right_rename;
      auto copyWithFreshVariables = [&](const Count &src,
                                        std::unordered_map<int, int> &rename) {
        Count dst;
        dst.constant = src.constant;
        for (const auto &term : src.variables) {
          auto it = rename.find(term.first);
          if (it == rename.end()) {
            const bool nonnegative = variable_info_.at(
                static_cast<std::size_t>(term.first)).nonnegative;
            it = rename.emplace(term.first, fresh(nonnegative)).first;
          }
          Count contribution;
          contribution.variables.emplace(it->second, term.second);
          addScaled(dst, contribution);
        }
        return dst;
      };
      for (std::size_t i = 0; i < dimensions_.size(); ++i) {
        out.cells[i] = copyWithFreshVariables(left.cells[i], left_rename);
        const Count rhs = copyWithFreshVariables(right.cells[i], right_rename);
        addScaled(out.cells[i], rhs);
      }
      break;
    }
    case RegexBuilder::Kind::Star: {
      const NodeCount &child = counts_.at(static_cast<std::size_t>(node.left));
      if (child.bad) break; // (bad)* still contains epsilon.
      const int repetitions = fresh(true);
      for (std::size_t i = 0; i < dimensions_.size(); ++i) {
        if (!child.cells[i].variables.empty()) {
          const int abstraction = fresh(false);
          out.cells[i].variables.emplace(abstraction, 1);
        } else if (child.cells[i].constant != 0) {
          out.cells[i].variables.emplace(repetitions, child.cells[i].constant);
        }
      }
      break;
    }
    }
  }
};

struct Fragment { Vertex start = 0; Vertex end = 0; };

class DerivedGraphBuilder {
public:
  explicit DerivedGraphBuilder(const RegexBuilder &builder) : b_(builder) {}

  Graph build(int root, Vertex &start, Vertex &end) {
    const Fragment fragment = make(root);
    start = fragment.start;
    end = fragment.end;
    return std::move(graph_);
  }

private:
  const RegexBuilder &b_;
  Graph graph_;
  Vertex next_ = 0;

  Vertex vertex() {
    const Vertex result = next_++;
    graph_.addVertex(result);
    return result;
  }
  void epsilon(Vertex from, Vertex to) {
    graph_.addEdge(from, to, Label::neutral());
  }
  Fragment make(int id) {
    const auto &node = b_.nodes.at(static_cast<std::size_t>(id));
    if (node.kind == RegexBuilder::Kind::Terminal) {
      const Vertex s = vertex(), t = vertex();
      graph_.addEdge(s, t, node.label);
      return {s, t};
    }
    if (node.kind == RegexBuilder::Kind::Epsilon) {
      const Vertex s = vertex(), t = vertex();
      epsilon(s, t);
      return {s, t};
    }
    if (node.kind == RegexBuilder::Kind::Empty) {
      return {vertex(), vertex()};
    }
    if (node.kind == RegexBuilder::Kind::Concat) {
      const Fragment a = make(node.left), c = make(node.right);
      epsilon(a.end, c.start);
      return {a.start, c.end};
    }
    if (node.kind == RegexBuilder::Kind::Or) {
      const Fragment a = make(node.left), c = make(node.right);
      const Vertex s = vertex(), t = vertex();
      epsilon(s, a.start);
      epsilon(s, c.start);
      epsilon(a.end, t);
      epsilon(c.end, t);
      return {s, t};
    }
    const Fragment a = make(node.left);
    const Vertex s = vertex(), t = vertex();
    epsilon(s, t);
    epsilon(s, a.start);
    epsilon(a.end, t);
    epsilon(a.end, a.start);
    return {s, t};
  }
};

void validate(const Graph &graph) {
  for (const Edge &edge : graph.edges()) {
    switch (edge.label.kind) {
    case LabelKind::OpenParenthesis:
    case LabelKind::CloseParenthesis:
    case LabelKind::OpenBracket:
    case LabelKind::CloseBracket:
    case LabelKind::Neutral:
      break;
    default:
      throw std::invalid_argument("ILP: invalid interleaved-Dyck label kind");
    }
  }
}

} // namespace

Result Solver::analyze(const Graph &graph, Vertex source, Vertex target,
                       const Options &options) const {
  validate(graph);
  Result result;
  if (!graph.containsVertex(source) || !graph.containsVertex(target)) return result;

  RegexGraph regex_graph;
  for (Vertex vertex : graph.vertices()) regex_graph.addNode(vertex);
  for (const Edge &edge : graph.edges()) {
    regex_graph.addEdge(edge.source, edge.label.str(), edge.target);
  }
  lotus::pathexpressions::PathExpressionComputer<Vertex, std::string> computer(
      regex_graph);
  const RegexRef path = computer.exprBetween(source, target);
  RegexBuilder builder(options.max_expression_nodes);
  const int root = builder.build(path);
  StandardForm standard_form(builder, options.max_disjuncts);
  const std::vector<int> disjuncts = standard_form.expand(root);
  result.statistics.path_expression_nodes = builder.nodes.size();
  std::vector<bool> bad(builder.nodes.size(), false);
  if (options.enable_lcl_refinement) {
    const lcl::Solver lcl_solver;
    for (std::size_t id = 0; id < builder.nodes.size(); ++id) {
      Vertex start = 0, end = 0;
      DerivedGraphBuilder derived(builder);
      Graph derived_graph = derived.build(static_cast<int>(id), start, end);
      const auto lcl_result = lcl_solver.analyze(derived_graph);
      if (lcl_result.summary_pairs.count({start, end}) == 0U) {
        bad[id] = true;
        ++result.statistics.rejected_bad_subexpressions;
      }
    }
  }

  std::unordered_set<Dimension, DimensionHash> dimensions_set;
  for (const Edge &edge : graph.edges()) {
    switch (edge.label.kind) {
    case LabelKind::OpenParenthesis:
    case LabelKind::CloseParenthesis:
      dimensions_set.insert({false, edge.label.id});
      break;
    case LabelKind::OpenBracket:
    case LabelKind::CloseBracket:
      dimensions_set.insert({true, edge.label.id});
      break;
    case LabelKind::Neutral:
      break;
    }
  }
  std::vector<Dimension> dimensions(dimensions_set.begin(), dimensions_set.end());
  std::sort(dimensions.begin(), dimensions.end(),
            [](const Dimension &a, const Dimension &b) {
              return a.bracket != b.bracket ? a.bracket < b.bracket
                                            : a.id < b.id;
            });

  auto backend = makeBackend(options.backend);
  for (int disjunct : disjuncts) {
    Encoder encoder(builder, dimensions, bad, options.allow_unmatched_opens,
                    result.statistics, *backend);
    if (encoder.feasible(disjunct)) {
      result.may_reach = true;
      return result;
    }
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::ilp
