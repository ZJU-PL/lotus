/**
 * @file DiffQuery.h
 * @brief Structural differencing queries for PDG scopes and query results.
 */

#pragma once

#include "IR/PDG/Query/Criteria.h"

namespace pdg {

enum class DiffKind { Added, Removed, Preserved };

enum class NodeMatchStrategy { PointerIdentity, CanonicalSource };

struct NodeDiffEntry {
  Node *node = nullptr;
  DiffKind kind = DiffKind::Preserved;
};

struct EdgeDiffEntry {
  Edge *edge = nullptr;
  DiffKind kind = DiffKind::Preserved;
};

struct DiffImpactSummary {
  std::unordered_map<std::string, size_t> functions;
  std::unordered_map<std::string, size_t> source_locations;
};

/// Structural diff result between two PDG subgraphs.
struct DiffQueryResult {
  std::vector<NodeDiffEntry> node_diffs;
  std::vector<EdgeDiffEntry> edge_diffs;
  DiffImpactSummary impact_summary;
  PDGQueryDiagnostics diagnostics;

  bool isIdentical() const;
};

/// Structural differencing for PDG query results or explicit scopes.
class DiffQuery {
public:
  explicit DiffQuery(
      ProgramGraph &pdg,
      NodeMatchStrategy strategy = NodeMatchStrategy::PointerIdentity)
      : pdg_(pdg), strategy_(strategy) {}

  DiffQueryResult diff(const PDGQueryResult &before, const PDGQueryResult &after,
                       const PDGQueryOptions &options = PDGQueryOptions()) const;

  DiffQueryResult diff(const PDGQueryScope &before, const PDGQueryScope &after,
                       const PDGQueryOptions &options = PDGQueryOptions()) const;

private:
  ProgramGraph &pdg_;
  NodeMatchStrategy strategy_;
};

} // namespace pdg
