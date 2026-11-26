#include "CFL/InterleavedDyck/AffineSPDS/Observer.h"

#include <algorithm>
#include <charconv>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace lotus::cfl::interleaved_dyck::affine {
bool EdgeLess::operator()(const Edge &a, const Edge &b) const {
  return std::tie(a.source, a.target, a.label.kind, a.label.id) <
         std::tie(b.source, b.target, b.label.kind, b.label.id);
}
HistoryObserver::HistoryObserver(std::size_t dimension)
    : identity_(Matrix::identity(dimension)), blocks_{{0, dimension}} {}
void HistoryObserver::set(const Edge &edge, const Matrix &matrix) {
  if (matrix.dimension() != dimension())
    throw std::invalid_argument("observer dimension mismatch");
  matrices_.insert_or_assign(edge, matrix);
}
const Matrix &HistoryObserver::matrix(const Edge &edge) const {
  auto found = matrices_.find(edge);
  return found == matrices_.end() ? identity_ : found->second;
}
void HistoryObserver::validate(const Graph &graph) const {
  const std::set<Edge, EdgeLess> edges(graph.edges().begin(),
                                       graph.edges().end());
  for (const auto &entry : matrices_)
    if (!edges.count(entry.first))
      throw std::invalid_argument(
          "observer refers to an edge absent from graph");
}
Matrix HistoryObserver::trace(const std::vector<Edge> &edges) const {
  Matrix result = identity_;
  for (const auto &edge : edges)
    result = result * matrix(edge);
  return result;
}
HistoryObserver HistoryObserver::parity(const std::vector<Edge> &events) {
  HistoryObserver result(2);
  Matrix toggle = Matrix::identity(2);
  toggle.set(0, 1);
  for (const auto &edge : events)
    result.set(edge, toggle);
  return result;
}
HistoryObserver HistoryObserver::orderedPair(const std::vector<Edge> &first,
                                             const std::vector<Edge> &second) {
  HistoryObserver result(3);
  for (const auto &edge : first) {
    Matrix m = result.matrix(edge);
    m.set(0, 1);
    result.set(edge, m);
  }
  for (const auto &edge : second) {
    Matrix m = result.matrix(edge);
    m.set(1, 2);
    result.set(edge, m);
  }
  return result;
}
HistoryObserver HistoryObserver::cyclic(const std::vector<Edge> &events,
                                        std::size_t modulus) {
  HistoryObserver result(modulus);
  Matrix shift(modulus);
  for (std::size_t i = 0; i < modulus; ++i)
    shift.set(i, (i + 1) % modulus);
  for (const auto &edge : events)
    result.set(edge, shift);
  return result;
}
HistoryObserver
HistoryObserver::directSum(const std::vector<HistoryObserver> &observers) {
  if (observers.empty())
    return HistoryObserver();
  std::size_t n = 0;
  std::set<Edge, EdgeLess> edges;
  for (const auto &o : observers) {
    if (o.dimension() > std::numeric_limits<std::size_t>::max() - n)
      throw std::length_error("observer direct sum overflow");
    n += o.dimension();
    for (const auto &entry : o.matrices_)
      edges.insert(entry.first);
  }
  HistoryObserver result(n);
  result.blocks_.clear();
  std::size_t offset = 0;
  for (const auto &o : observers) {
    for (const auto &block : o.blocks_)
      result.blocks_.emplace_back(offset + block.first, block.second);
    offset += o.dimension();
  }
  for (const auto &edge : edges) {
    std::vector<Matrix> blocks;
    blocks.reserve(observers.size());
    for (const auto &o : observers)
      blocks.push_back(o.matrix(edge));
    result.set(edge, Matrix::directSum(blocks));
  }
  return result;
}
HistoryObserver HistoryObserver::automatic(const Graph &graph,
                                           ObserverOptions options) {
  const auto count = std::min(options.max_events, graph.edges().size());
  const auto maximum = std::numeric_limits<std::size_t>::max();
  const auto first = count % 2 ? count : count / 2;
  const auto second = count < 2 ? 0 : (count % 2 ? (count - 1) / 2 : count - 1);
  const auto combinations =
      first && second > maximum / first ? maximum : first * second;
  const auto pair_count = std::min(options.max_order_pairs, combinations);
  if (count > maximum / 2 || pair_count > (maximum - 2 * count) / 3)
    throw std::length_error("automatic observer dimension overflow");
  const auto dimension = count ? 2 * count + 3 * pair_count : 1;
  if (options.max_matrix_dimension && dimension > options.max_matrix_dimension)
    throw std::length_error(
        "automatic observer matrix-dimension limit exceeded");
  std::map<Vertex, std::vector<Edge>> outgoing;
  std::vector<Edge> sorted = graph.edges();
  std::sort(sorted.begin(), sorted.end(), EdgeLess{});
  for (const auto &edge : sorted)
    outgoing[edge.source].push_back(edge);
  std::vector<Edge> selected;
  std::set<Edge, EdgeLess> seen;
  auto select = [&](const Edge &edge) {
    if (selected.size() < options.max_events && seen.insert(edge).second)
      selected.push_back(edge);
  };
  // One representative for each branch avoids spending the entire budget at
  // the first branch. Additional alternatives are considered in the next pass.
  for (const auto &entry : outgoing)
    if (entry.second.size() > 1)
      select(entry.second[1]);
  for (const auto &entry : outgoing)
    for (std::size_t i = 2; i < entry.second.size(); ++i)
      select(entry.second[i]);
  for (const auto &edge : sorted)
    select(edge);
  std::vector<HistoryObserver> components;
  components.reserve(count + pair_count);
  for (const auto &edge : selected)
    components.push_back(parity({edge}));
  std::size_t pairs = 0;
  for (std::size_t j = 1;
       j < selected.size() && pairs < options.max_order_pairs; ++j)
    for (std::size_t i = 0; i < j && pairs < options.max_order_pairs; ++i) {
      components.push_back(orderedPair({selected[i]}, {selected[j]}));
      ++pairs;
    }
  return directSum(components);
}
std::shared_ptr<const MatrixLayout> HistoryObserver::compactLayout() const {
  if (blocks_.size() < 2)
    return nullptr;
  auto layout = std::make_shared<const MatrixLayout>(dimension(), blocks_);
  for (const auto &entry : matrices_)
    if (!layout->contains(entry.second))
      return nullptr;
  return layout;
}
void HistoryObserver::write(std::ostream &out) const {
  out << "# AffineSPDS observer v1; unspecified original edges carry "
         "identity\n";
  out << "dimension " << dimension() << '\n';
  for (const auto &block : blocks_)
    out << "block " << block.first << ' ' << block.second << '\n';
  for (const auto &entry : matrices_)
    out << "edge " << entry.first.source << ' ' << entry.first.target << ' '
        << entry.first.label.str() << ' ' << entry.second.str() << '\n';
  if (!out)
    throw std::runtime_error("failed to write history observer");
}
namespace {
template <class T> T integer(const std::string &text) {
  T value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("invalid observer integer: " + text);
  return value;
}
} // namespace
HistoryObserver HistoryObserver::read(std::istream &input) {
  std::optional<HistoryObserver> result;
  std::string line;
  std::size_t line_number = 0;
  std::vector<std::pair<std::size_t, std::size_t>> blocks;
  while (std::getline(input, line)) {
    ++line_number;
    const auto comment = line.find('#');
    if (comment != std::string::npos)
      line.erase(comment);
    std::istringstream tokens(line);
    std::vector<std::string> fields;
    std::string token;
    while (tokens >> token)
      fields.push_back(token);
    if (fields.empty())
      continue;
    try {
      if (fields[0] == "dimension" && fields.size() == 2 && !result) {
        result.emplace(integer<std::size_t>(fields[1]));
      } else if (result && fields[0] == "block" && fields.size() == 3) {
        blocks.emplace_back(integer<std::size_t>(fields[1]),
                            integer<std::size_t>(fields[2]));
      } else if (result && fields[0] == "edge" && fields.size() == 5) {
        Edge edge{integer<Vertex>(fields[1]), integer<Vertex>(fields[2]),
                  Label::parse(fields[3])};
        if (result->matrices_.count(edge))
          throw std::invalid_argument("duplicate observer edge");
        result->set(edge, Matrix::parse(fields[4]));
      } else
        throw std::invalid_argument(
            "expected dimension, block, or edge directive");
    } catch (const std::exception &e) {
      throw std::invalid_argument(
          "observer line " + std::to_string(line_number) + ": " + e.what());
    }
  }
  if (input.bad())
    throw std::runtime_error("failed to read observer");
  if (!result)
    throw std::invalid_argument("observer has no dimension");
  if (!blocks.empty()) {
    std::size_t next = 0;
    for (const auto &block : blocks) {
      if (block.first != next || !block.second ||
          block.second > result->dimension() - next)
        throw std::invalid_argument(
            "observer blocks must partition the matrix diagonal");
      next += block.second;
    }
    if (next != result->dimension())
      throw std::invalid_argument("incomplete observer block partition");
    result->blocks_ = std::move(blocks);
  }
  return *result;
}
} // namespace lotus::cfl::interleaved_dyck::affine
