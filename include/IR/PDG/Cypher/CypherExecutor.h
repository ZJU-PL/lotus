#pragma once

#include "IR/PDG/Cypher/CypherAST.h"
#include "IR/PDG/Cypher/CypherResult.h"

#include <memory>

namespace pdg {

class BoundsAnalysis;

/**
 * @brief Query execution statistics
 */
struct CypherQueryStats {
  std::chrono::microseconds parseTime{0};
  std::chrono::microseconds executionTime{0};
  size_t nodesVisited = 0;
  size_t edgesVisited = 0;
  size_t resultsReturned = 0;
  bool usedCache = false;
  bool timedOut = false;
};

/**
 * @brief Cypher query executor with caching and optimization
 */
class CypherQueryExecutor {
public:
  explicit CypherQueryExecutor(ProgramGraph &pdg);
  ~CypherQueryExecutor();

  std::unique_ptr<CypherResult> execute(const CypherQuery &query);
  std::unique_ptr<CypherResult> execute(const CypherQuery &query,
                                        CypherQueryStats &stats);

  // PDG-specific operations mapped to Cypher
  std::unique_ptr<CypherResult> matchNodes(const std::string &label,
                                           const std::string &variable);
  std::unique_ptr<CypherResult> matchEdges(const std::string &type,
                                           const std::string &variable);
  std::unique_ptr<CypherResult>
  matchPattern(const CypherPatternElement *pattern);
  std::unique_ptr<CypherResult>
  traverse(Node *start, const CypherRelationshipPattern &rel, int maxHops);
  std::unique_ptr<CypherResult> filterByWhere(const std::vector<Node *> &nodes,
                                              const CypherWhereClause &where);
  std::unique_ptr<CypherResult> filterByWhere(const std::vector<Edge *> &edges,
                                              const CypherWhereClause &where);

  // Optimizer query functions
  std::unique_ptr<CypherResult> canMoveEarlier(Node *moving, Node *anchor);
  std::unique_ptr<CypherResult> canMoveLater(Node *moving, Node *anchor);
  std::unique_ptr<CypherResult> independent(Node *a, Node *b);
  std::unique_ptr<CypherResult> readySet(const std::vector<Node *> &region,
                                         const std::vector<Node *> &scheduled);
  std::unique_ptr<CypherResult> criticalPath(const std::vector<Node *> &region);

  // Utility
  ProgramGraph &getPDG() { return pdg_; }
  const ProgramGraph &getPDG() const { return pdg_; }

  // Introspection helpers (useful for tooling / result rendering)
  const std::vector<Node *> *getBoundVariable(const std::string &name) const {
    auto it = boundVariables_.find(name);
    if (it == boundVariables_.end())
      return nullptr;
    return &it->second;
  }

  const std::vector<Edge *> *
  getBoundRelationship(const std::string &name) const {
    auto it = boundRelationships_.find(name);
    if (it == boundRelationships_.end())
      return nullptr;
    return &it->second;
  }

  std::string getNodePropertyString(Node *node, const std::string &property) {
    return getNodeProperty(node, property);
  }

  std::string getEdgePropertyString(Edge *edge, const std::string &property) {
    return getEdgeProperty(edge, property);
  }

  void setError(const std::string &error) { lastError_ = error; }
  const std::string &getLastError() const { return lastError_; }

  // Caching
  // Also call this after changing the retained LLVM IR without rebuilding PDG.
  void clearCache();
  void setPreserveSemanticCache(bool preserve) {
    preserveSemanticCache_ = preserve;
  }
  void setCacheMaxSize(size_t maxSize) { cacheMaxSize_ = maxSize; }
  void setQueryTimeout(std::chrono::seconds timeout) {
    queryTimeout_ = timeout;
  }

  void setUnboundedMaxHops(int maxHops) {
    unboundedMaxHops_ = std::max(1, maxHops);
  }

  const CypherQueryStats &getLastStats() const { return lastStats_; }

private:
  struct MatchRow {
    std::unordered_map<std::string, Node *> nodes;
    std::unordered_map<std::string, Edge *> rels;
  };

  ProgramGraph &pdg_;
  std::string lastError_;
  CypherQueryStats lastStats_;

  // Query caching
  std::unordered_map<std::string, CypherCompiledQuery> queryCache_;
  size_t cacheMaxSize_ = 100;
  std::chrono::seconds queryTimeout_{30};

  std::unordered_map<std::string, std::vector<Node *>> boundVariables_;
  std::unordered_map<std::string, std::vector<Edge *>> boundRelationships_;
  int unboundedMaxHops_ = 5;

  // Semantic services are evaluated on demand, once per unchanged module.
  struct SemanticCache;
  std::unique_ptr<SemanticCache> semanticCache_;
  bool preserveSemanticCache_ = false;
  void syncSemanticFacts();
  BoundsAnalysis &getBoundsFacts(const llvm::Module &module);

  // Helper methods
  bool evaluateCondition(const CypherWhereClause &condition,
                         const MatchRow &row);
  bool evaluateCondition(const CypherWhereClause &condition, Node *node);
  bool evaluateCondition(const CypherWhereClause &condition, Edge *edge);
  std::string getNodeProperty(Node *node, const std::string &property);
  std::string getEdgeProperty(Edge *edge, const std::string &property);

  bool applyComparison(const std::string &nodeValue, CypherComparisonOp op,
                       const std::string &queryValue);

  // Caching helpers
  std::string generateCacheKey(const CypherQuery &query);
  std::unique_ptr<CypherResult> getFromCache(const std::string &key);
  void addToCache(const std::string &key, const CypherQuery &query,
                  std::unique_ptr<CypherResult> result);

  // Performance optimization
  bool shouldUseIndex(const std::string &label);
  std::vector<Node *> getNodesByLabel(const std::string &label);
};

} // namespace pdg
