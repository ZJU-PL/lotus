#include "IR/PDG/QueryLanguage/Execution/CypherExecutor.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include "IR/PDG/Analysis/LibraryModels.h"
#include "IR/PDG/Analysis/Query.h"
#include "IR/PDG/Analysis/ValueFacts.h"
#include "IR/PDG/Support/DebugInfoUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sstream>

namespace pdg {

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string unquoteStringToken(std::string token) {
  if (token.size() >= 2 && ((token.front() == '"' && token.back() == '"') ||
                            (token.front() == '\'' && token.back() == '\''))) {
    token = token.substr(1, token.size() - 2);
  }
  return token;
}

static const char *graphNodeTypeName(GraphNodeType t) {
  switch (t) {
  case GraphNodeType::INST_FUNCALL:
    return "INST_FUNCALL";
  case GraphNodeType::INST_RET:
    return "INST_RET";
  case GraphNodeType::INST_BR:
    return "INST_BR";
  case GraphNodeType::INST_OTHER:
    return "INST_OTHER";
  case GraphNodeType::FUNC_ENTRY:
    return "FUNC_ENTRY";
  case GraphNodeType::PARAM_FORMALIN:
    return "PARAM_FORMALIN";
  case GraphNodeType::PARAM_FORMALOUT:
    return "PARAM_FORMALOUT";
  case GraphNodeType::PARAM_ACTUALIN:
    return "PARAM_ACTUALIN";
  case GraphNodeType::PARAM_ACTUALOUT:
    return "PARAM_ACTUALOUT";
  case GraphNodeType::VAR_STATICALLOCGLOBALSCOPE:
    return "VAR_STATICALLOCGLOBALSCOPE";
  case GraphNodeType::VAR_STATICALLOCMODULESCOPE:
    return "VAR_STATICALLOCMODULESCOPE";
  case GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE:
    return "VAR_STATICALLOCFUNCTIONSCOPE";
  case GraphNodeType::VAR_OTHER:
    return "VAR_OTHER";
  case GraphNodeType::FUNC:
    return "FUNC";
  case GraphNodeType::CLASS:
    return "CLASS";
  case GraphNodeType::ANNO_VAR:
    return "ANNO_VAR";
  case GraphNodeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case GraphNodeType::ANNO_OTHER:
    return "ANNO_OTHER";
  }
  return "UNKNOWN";
}

static const char *edgeTypeName(EdgeType t) {
  switch (t) {
  case EdgeType::IND_CALL:
    return "IND_CALL";
  case EdgeType::CONTROLDEP_CALLINV:
    return "CONTROLDEP_CALLINV";
  case EdgeType::CONTROLDEP_CALLRET:
    return "CONTROLDEP_CALLRET";
  case EdgeType::CONTROLDEP_ENTRY:
    return "CONTROLDEP_ENTRY";
  case EdgeType::CONTROLDEP_BR:
    return "CONTROLDEP_BR";
  case EdgeType::CONTROLDEP_IND_BR:
    return "CONTROLDEP_IND_BR";
  case EdgeType::DATA_DEF_USE:
    return "DATA_DEF_USE";
  case EdgeType::DATA_RAW:
    return "DATA_RAW";
  case EdgeType::DATA_READ:
    return "DATA_READ";
  case EdgeType::DATA_ALIAS:
    return "DATA_ALIAS";
  case EdgeType::DATA_RET:
    return "DATA_RET";
  case EdgeType::PARAMETER_IN:
    return "PARAMETER_IN";
  case EdgeType::PARAMETER_OUT:
    return "PARAMETER_OUT";
  case EdgeType::PARAMETER_FIELD:
    return "PARAMETER_FIELD";
  case EdgeType::GLOBAL_DEP:
    return "GLOBAL_DEP";
  case EdgeType::VAL_DEP:
    return "VAL_DEP";
  case EdgeType::CLS_MTH:
    return "CLS_MTH";
  case EdgeType::ANNO_VAR:
    return "ANNO_VAR";
  case EdgeType::ANNO_GLOBAL:
    return "ANNO_GLOBAL";
  case EdgeType::ANNO_OTHER:
    return "ANNO_OTHER";
  case EdgeType::TYPE_OTHEREDGE:
    return "TYPE_OTHEREDGE";
  }
  return "UNKNOWN";
}

// ============================================================================
// CypherQueryExecutor implementation
// ============================================================================

std::unique_ptr<CypherResult>
CypherQueryExecutor::execute(const CypherQuery &query) {
  CypherQueryStats stats;
  return execute(query, stats);
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::execute(const CypherQuery &query,
                             CypherQueryStats &stats) {
  // IR edits need not change the graph epoch; each execution observes fresh
  // semantic facts and shares them across its predicates and projections.
  boundsFacts_.clear();
  stateFacts_.clear();
  auto startTime = std::chrono::high_resolution_clock::now();
  lastStats_ = CypherQueryStats();

  // Bindings are per-query; don't let interactive sessions leak state.
  boundVariables_.clear();
  boundRelationships_.clear();

  auto splitVarProp = [](const std::string &expr) {
    std::string var = expr;
    std::string prop;
    const auto dot = expr.find('.');
    if (dot != std::string::npos) {
      var = expr.substr(0, dot);
      prop = expr.substr(dot + 1);
    }
    return std::pair<std::string, std::string>(std::move(var), std::move(prop));
  };

  auto applyNodePattern = [&](Node *n, const CypherNodePattern *pat) {
    if (!pat || !n) {
      return false;
    }

    if (!pat->getLabel().empty()) {
      auto labelMatches = matchNodes(pat->getLabel(), "");
      bool found = false;
      if (labelMatches) {
        for (auto *cand : labelMatches->getNodes()) {
          if (cand == n) {
            found = true;
            break;
          }
        }
      }
      if (!found) {
        return false;
      }
    }

    for (const auto &kv : pat->getProperties()) {
      if (getNodeProperty(n, kv.first) != kv.second) {
        return false;
      }
    }
    return true;
  };

  auto edgeMatchesType = [](EdgeType t, const std::string &upperType) {
    if (upperType.empty()) {
      return true;
    }

    std::vector<std::string> alts;
    size_t startPos = 0;
    while (startPos <= upperType.size()) {
      size_t bar = upperType.find('|', startPos);
      if (bar == std::string::npos) {
        alts.push_back(upperType.substr(startPos));
        break;
      }
      alts.push_back(upperType.substr(startPos, bar - startPos));
      startPos = bar + 1;
    }

    for (const auto &alt : alts) {
      if (alt.empty())
        return true;
      if (alt == "CONTROL_DEP") {
        if (t == EdgeType::CONTROLDEP_ENTRY || t == EdgeType::CONTROLDEP_BR ||
            t == EdgeType::CONTROLDEP_IND_BR ||
            t == EdgeType::CONTROLDEP_CALLINV ||
            t == EdgeType::CONTROLDEP_CALLRET) {
          return true;
        }
      } else if (alt == "CALL") {
        if (t == EdgeType::CONTROLDEP_CALLINV ||
            t == EdgeType::CONTROLDEP_CALLRET || t == EdgeType::IND_CALL) {
          return true;
        }
      } else if (alt == "DATA_DEP") {
        if (t == EdgeType::DATA_DEF_USE) {
          return true;
        }
      } else {
        static const std::unordered_map<std::string, EdgeType> typeMap = {
            {"DATA_DEF_USE", EdgeType::DATA_DEF_USE},
            {"DATA_RAW", EdgeType::DATA_RAW},
            {"DATA_READ", EdgeType::DATA_READ},
            {"DATA_ALIAS", EdgeType::DATA_ALIAS},
            {"DATA_RET", EdgeType::DATA_RET},
            {"CONTROLDEP_ENTRY", EdgeType::CONTROLDEP_ENTRY},
            {"CONTROLDEP_BR", EdgeType::CONTROLDEP_BR},
            {"CONTROLDEP_IND_BR", EdgeType::CONTROLDEP_IND_BR},
            {"CONTROLDEP_CALLINV", EdgeType::CONTROLDEP_CALLINV},
            {"CONTROLDEP_CALLRET", EdgeType::CONTROLDEP_CALLRET},
            {"CALL_INV", EdgeType::CONTROLDEP_CALLINV},
            {"CALL_RET", EdgeType::CONTROLDEP_CALLRET},
            {"IND_CALL", EdgeType::IND_CALL},
            {"PARAM_IN", EdgeType::PARAMETER_IN},
            {"PARAM_OUT", EdgeType::PARAMETER_OUT},
            {"PARAMETER_IN", EdgeType::PARAMETER_IN},
            {"PARAMETER_OUT", EdgeType::PARAMETER_OUT},
        };
        auto it = typeMap.find(alt);
        if (it != typeMap.end() && t == it->second) {
          return true;
        }
      }
    }
    return false;
  };

  auto expandFromNode = [&](Node *start, const CypherRelationshipPattern *rel) {
    std::vector<std::pair<Node *, Edge *>> out;
    if (!start || !rel) {
      return out;
    }

    int minHops = std::max(1, rel->getMinHops());
    int maxHops = rel->hasVariableLength() ? rel->getMaxHops() : 1;
    if (maxHops < 0) {
      maxHops = unboundedMaxHops_;
    }
    maxHops = std::max(1, maxHops);

    std::string upperType = rel->getType();
    std::transform(upperType.begin(), upperType.end(), upperType.begin(),
                   ::toupper);

    std::vector<Node *> frontier{start};
    for (int hop = 0; hop < maxHops && !frontier.empty(); ++hop) {
      std::vector<Node *> next;
      for (auto *node : frontier) {
        if (rel->getDirection() == CypherRelationshipPattern::Direction::OUT ||
            rel->getDirection() == CypherRelationshipPattern::Direction::BOTH) {
          for (auto *e : node->getOutEdgeSet()) {
            if (!edgeMatchesType(e->getEdgeType(), upperType))
              continue;
            Node *nbr = e->getDstNode();
            if (hop + 1 >= minHops) {
              out.push_back({nbr, e});
            }
            next.push_back(nbr);
          }
        }
        if (rel->getDirection() == CypherRelationshipPattern::Direction::IN ||
            rel->getDirection() == CypherRelationshipPattern::Direction::BOTH) {
          for (auto *e : node->getInEdgeSet()) {
            if (!edgeMatchesType(e->getEdgeType(), upperType))
              continue;
            Node *nbr = e->getSrcNode();
            if (hop + 1 >= minHops) {
              out.push_back({nbr, e});
            }
            next.push_back(nbr);
          }
        }
      }
      frontier = std::move(next);
    }
    return out;
  };

  auto collectChain = [](const CypherPatternElement *root) {
    std::vector<const CypherPatternElement *> chain;
    const CypherPatternElement *cur = root;
    while (cur) {
      chain.push_back(cur);
      if (cur->getNextElements().empty()) {
        break;
      }
      cur = cur->getNextElements()[0].get();
    }
    return chain;
  };

  std::vector<MatchRow> rows(1); // one empty row

  for (const auto &pattern : query.getPatterns()) {
    const auto chain = collectChain(pattern.get());
    if (chain.empty()) {
      continue;
    }

    std::vector<MatchRow> nextRows;
    for (const auto &baseRow : rows) {
      std::vector<std::pair<MatchRow, Node *>> states;
      const CypherNodePattern *startPat = chain[0]->getStartNode();
      if (!startPat) {
        continue;
      }

      std::vector<Node *> startCandidates;
      if (!startPat->getVariable().empty()) {
        auto it = baseRow.nodes.find(startPat->getVariable());
        if (it != baseRow.nodes.end()) {
          startCandidates.push_back(it->second);
        }
      }
      if (startCandidates.empty()) {
        auto matched = matchNodes(startPat->getLabel(), "");
        if (!matched) {
          continue;
        }
        startCandidates = matched->getNodes();
      }

      for (auto *cand : startCandidates) {
        if (!applyNodePattern(cand, startPat)) {
          continue;
        }
        MatchRow seeded = baseRow;
        if (!startPat->getVariable().empty()) {
          auto it = seeded.nodes.find(startPat->getVariable());
          if (it != seeded.nodes.end() && it->second != cand) {
            continue;
          }
          seeded.nodes[startPat->getVariable()] = cand;
        }
        states.push_back({std::move(seeded), cand});
      }

      for (const auto *elem : chain) {
        std::vector<std::pair<MatchRow, Node *>> advanced;
        const auto *startNodePat = elem->getStartNode();
        const auto *rel = elem->getRelationship();
        const auto *endNodePat = elem->getEndNode();

        for (auto &st : states) {
          MatchRow row = st.first;
          Node *curNode = st.second;

          if (startNodePat && !applyNodePattern(curNode, startNodePat)) {
            continue;
          }
          if (startNodePat && !startNodePat->getVariable().empty()) {
            auto it = row.nodes.find(startNodePat->getVariable());
            if (it != row.nodes.end() && it->second != curNode) {
              continue;
            }
            row.nodes[startNodePat->getVariable()] = curNode;
          }

          if (!rel) {
            advanced.push_back({std::move(row), curNode});
            continue;
          }

          auto expansions = expandFromNode(curNode, rel);
          for (const auto &exp : expansions) {
            Node *dst = exp.first;
            Edge *edge = exp.second;
            if (endNodePat && !applyNodePattern(dst, endNodePat)) {
              continue;
            }

            MatchRow row2 = row;
            if (!rel->getVariable().empty()) {
              auto it = row2.rels.find(rel->getVariable());
              if (it != row2.rels.end() && it->second != edge) {
                continue;
              }
              row2.rels[rel->getVariable()] = edge;
            }
            if (endNodePat && !endNodePat->getVariable().empty()) {
              auto it = row2.nodes.find(endNodePat->getVariable());
              if (it != row2.nodes.end() && it->second != dst) {
                continue;
              }
              row2.nodes[endNodePat->getVariable()] = dst;
            }
            advanced.push_back({std::move(row2), dst});
          }
        }

        states = std::move(advanced);
        if (states.empty()) {
          break;
        }
      }

      for (auto &st : states) {
        nextRows.push_back(std::move(st.first));
      }
    }
    rows = std::move(nextRows);
    if (rows.empty()) {
      break;
    }
  }

  if (query.getWhereClause()) {
    std::vector<MatchRow> filteredRows;
    filteredRows.reserve(rows.size());
    for (const auto &row : rows) {
      if (evaluateCondition(*query.getWhereClause(), row)) {
        filteredRows.push_back(row);
      }
    }
    rows = std::move(filteredRows);
  }

  // Aggregations (currently: COUNT) return a scalar result.
  if (query.getReturnItems().size() == 1 &&
      query.getReturnItems()[0]->getKind() == CypherReturnItem::Kind::COUNT) {
    const auto &item = *query.getReturnItems()[0];

    auto result =
        std::make_unique<CypherResult>(CypherResult::ResultType::INTEGER);

    auto splitVarProp = [](const std::string &expr) {
      std::string var = expr;
      std::string prop;
      auto dot = expr.find('.');
      if (dot != std::string::npos) {
        var = expr.substr(0, dot);
        prop = expr.substr(dot + 1);
      }
      return std::pair<std::string, std::string>(std::move(var),
                                                 std::move(prop));
    };

    const auto argParts = splitVarProp(item.getAggArg());
    const std::string &argVar = argParts.first;
    const std::string &argProp = argParts.second;
    const bool wantDistinct = item.isAggDistinct();

    int64_t count = 0;

    if (argVar == "*") {
      count = static_cast<int64_t>(rows.size());
    } else if (argProp.empty()) {
      if (wantDistinct) {
        std::unordered_set<uintptr_t> uniq;
        for (const auto &row : rows) {
          auto itN = row.nodes.find(argVar);
          if (itN != row.nodes.end())
            uniq.insert(reinterpret_cast<uintptr_t>(itN->second));
          auto itR = row.rels.find(argVar);
          if (itR != row.rels.end())
            uniq.insert(reinterpret_cast<uintptr_t>(itR->second));
        }
        count = static_cast<int64_t>(uniq.size());
      } else {
        for (const auto &row : rows) {
          if (row.nodes.find(argVar) != row.nodes.end() ||
              row.rels.find(argVar) != row.rels.end()) {
            ++count;
          }
        }
      }
    } else {
      if (wantDistinct) {
        std::unordered_set<std::string> uniq;
        for (const auto &row : rows) {
          auto itN = row.nodes.find(argVar);
          if (itN != row.nodes.end()) {
            const auto v = getNodeProperty(itN->second, argProp);
            if (!v.empty())
              uniq.insert(v);
            continue;
          }
          auto itR = row.rels.find(argVar);
          if (itR != row.rels.end()) {
            const auto v = getEdgeProperty(itR->second, argProp);
            if (!v.empty())
              uniq.insert(v);
          }
        }
        count = static_cast<int64_t>(uniq.size());
      } else {
        for (const auto &row : rows) {
          auto itN = row.nodes.find(argVar);
          if (itN != row.nodes.end()) {
            if (!getNodeProperty(itN->second, argProp).empty())
              ++count;
            continue;
          }
          auto itR = row.rels.find(argVar);
          if (itR != row.rels.end()) {
            if (!getEdgeProperty(itR->second, argProp).empty())
              ++count;
          }
        }
      }
    }

    result->setIntegerValue(count);
    lastStats_.resultsReturned = result->getCount();

    auto endTime = std::chrono::high_resolution_clock::now();
    lastStats_.executionTime =
        std::chrono::duration_cast<std::chrono::microseconds>(endTime -
                                                              startTime);
    stats = lastStats_;
    return result;
  }

  // ORDER BY applies to rows.
  if (query.getOrderBy() && !rows.empty()) {
    const auto &ob = *query.getOrderBy();
    const std::string prop =
        ob.getProperty().empty() ? "label" : ob.getProperty();

    auto tryParseInt = [](const std::string &s, int64_t &out) -> bool {
      if (s.empty())
        return false;
      char *end = nullptr;
      const long long v = std::strtoll(s.c_str(), &end, 10);
      if (!end || *end != '\0')
        return false;
      out = static_cast<int64_t>(v);
      return true;
    };

    auto rowKey = [&](const MatchRow &row) {
      auto itN = row.nodes.find(ob.getVariable());
      if (itN != row.nodes.end()) {
        return getNodeProperty(itN->second, prop);
      }
      auto itR = row.rels.find(ob.getVariable());
      if (itR != row.rels.end()) {
        return getEdgeProperty(itR->second, prop);
      }
      return std::string();
    };

    auto cmp = [&](const MatchRow &a, const MatchRow &b) {
      const std::string va = rowKey(a);
      const std::string vb = rowKey(b);
      int64_t ia = 0, ib = 0;
      const bool na = tryParseInt(va, ia);
      const bool nb = tryParseInt(vb, ib);
      if (na && nb)
        return ia < ib;
      return va < vb;
    };

    std::sort(rows.begin(), rows.end(), cmp);
    if (ob.getDirection() == CypherOrderBy::Direction::DESC) {
      std::reverse(rows.begin(), rows.end());
    }
  }

  if (query.hasLimit() && query.getLimit() > 0 &&
      rows.size() > static_cast<size_t>(query.getLimit())) {
    rows.resize(static_cast<size_t>(query.getLimit()));
  }

  // Rebuild bindings from final rows.
  boundVariables_.clear();
  boundRelationships_.clear();
  for (const auto &row : rows) {
    for (const auto &kv : row.nodes) {
      boundVariables_[kv.first].push_back(kv.second);
    }
    for (const auto &kv : row.rels) {
      boundRelationships_[kv.first].push_back(kv.second);
    }
  }

  std::vector<Node *> projectedNodes;
  std::vector<Edge *> projectedEdges;
  std::vector<std::string> projectedScalars;
  bool wantsNodeProjection = false;
  bool wantsEdgeProjection = false;
  bool wantsScalarProjection = false;
  std::unordered_set<std::string> declaredNodeVars;
  std::unordered_set<std::string> declaredRelVars;

  auto collectDeclaredVars = [&](const auto &self,
                                 const CypherPatternElement *elem) -> void {
    if (!elem)
      return;
    if (elem->getStartNode() && !elem->getStartNode()->getVariable().empty()) {
      declaredNodeVars.insert(elem->getStartNode()->getVariable());
    }
    if (elem->getEndNode() && !elem->getEndNode()->getVariable().empty()) {
      declaredNodeVars.insert(elem->getEndNode()->getVariable());
    }
    if (elem->getRelationship() &&
        !elem->getRelationship()->getVariable().empty()) {
      declaredRelVars.insert(elem->getRelationship()->getVariable());
    }
    for (const auto &next : elem->getNextElements()) {
      self(self, next.get());
    }
  };
  for (const auto &p : query.getPatterns()) {
    collectDeclaredVars(collectDeclaredVars, p.get());
  }

  if (query.getReturnItems().empty()) {
    for (const auto &row : rows) {
      for (const auto &kv : row.nodes)
        projectedNodes.push_back(kv.second);
      for (const auto &kv : row.rels)
        projectedEdges.push_back(kv.second);
    }
  } else {
    std::vector<std::vector<std::string>> tabular;
    tabular.reserve(rows.size());

    for (const auto &itemPtr : query.getReturnItems()) {
      const auto &item = *itemPtr;
      if (item.getKind() != CypherReturnItem::Kind::VARIABLE_OR_PROPERTY) {
        continue;
      }
      const auto varProp = splitVarProp(item.getVariable());
      const std::string &var = varProp.first;
      const std::string &prop = varProp.second;
      if (!prop.empty()) {
        wantsScalarProjection = true;
      } else if (declaredRelVars.find(var) != declaredRelVars.end()) {
        wantsEdgeProjection = true;
      } else if (declaredNodeVars.find(var) != declaredNodeVars.end()) {
        wantsNodeProjection = true;
      }
    }

    for (const auto &row : rows) {
      std::vector<std::string> columns;
      columns.reserve(query.getReturnItems().size());
      for (const auto &itemPtr : query.getReturnItems()) {
        const auto &item = *itemPtr;
        if (item.getKind() != CypherReturnItem::Kind::VARIABLE_OR_PROPERTY) {
          continue;
        }

        const auto varProp = splitVarProp(item.getVariable());
        const std::string &var = varProp.first;
        const std::string &prop = varProp.second;

        auto itN = row.nodes.find(var);
        if (itN != row.nodes.end()) {
          if (prop.empty()) {
            wantsNodeProjection = true;
            projectedNodes.push_back(itN->second);
            columns.push_back(getNodeProperty(itN->second, "label"));
          } else {
            wantsScalarProjection = true;
            const auto v = getNodeProperty(itN->second, prop);
            projectedScalars.push_back(v);
            columns.push_back(v);
          }
          continue;
        }

        auto itR = row.rels.find(var);
        if (itR != row.rels.end()) {
          if (prop.empty()) {
            wantsEdgeProjection = true;
            projectedEdges.push_back(itR->second);
            columns.push_back(getEdgeProperty(itR->second, "label"));
          } else {
            wantsScalarProjection = true;
            const auto v = getEdgeProperty(itR->second, prop);
            projectedScalars.push_back(v);
            columns.push_back(v);
          }
          continue;
        }

        columns.push_back("");
      }
      tabular.push_back(std::move(columns));
    }

    if (query.getReturnItems().size() > 1) {
      wantsScalarProjection = true;
      wantsNodeProjection = false;
      wantsEdgeProjection = false;
      projectedScalars.clear();
      for (const auto &cols : tabular) {
        std::ostringstream line;
        for (size_t i = 0; i < cols.size(); ++i) {
          if (i)
            line << ", ";
          line << cols[i];
        }
        projectedScalars.push_back(line.str());
      }
    }
  }

  std::unique_ptr<CypherResult> result;
  if (wantsScalarProjection && !wantsNodeProjection && !wantsEdgeProjection) {
    std::ostringstream oss;
    for (size_t i = 0; i < projectedScalars.size(); ++i) {
      if (i)
        oss << "\n";
      oss << projectedScalars[i];
    }
    result = std::make_unique<CypherResult>(CypherResult::ResultType::SCALAR);
    result->setScalarValue(oss.str());
  } else if (wantsEdgeProjection && !wantsNodeProjection &&
             !wantsScalarProjection) {
    result =
        std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);
    for (auto *edge : projectedEdges) {
      result->addEdge(edge);
    }
  } else {
    result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);
    for (auto *node : projectedNodes) {
      result->addNode(node);
    }
    for (auto *edge : projectedEdges) {
      result->addEdge(edge);
    }
  }

  lastStats_.resultsReturned = result->getCount();

  auto endTime = std::chrono::high_resolution_clock::now();
  lastStats_.executionTime =
      std::chrono::duration_cast<std::chrono::microseconds>(endTime -
                                                            startTime);
  stats = lastStats_;

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchPattern(const CypherPatternElement *pattern) {
  if (!pattern)
    return nullptr;

  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  auto dedupNodes = [](std::vector<Node *> &nodes) {
    std::sort(nodes.begin(), nodes.end());
    nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  };
  auto dedupEdges = [](std::vector<Edge *> &edges) {
    std::sort(edges.begin(), edges.end());
    edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  };

  auto bindNodes = [&](const std::string &var,
                       const std::vector<Node *> &nodes) {
    if (var.empty())
      return;
    auto &bucket = boundVariables_[var];
    bucket.insert(bucket.end(), nodes.begin(), nodes.end());
    dedupNodes(bucket);
  };
  auto bindEdges = [&](const std::string &var,
                       const std::vector<Edge *> &edges) {
    if (var.empty())
      return;
    auto &bucket = boundRelationships_[var];
    bucket.insert(bucket.end(), edges.begin(), edges.end());
    dedupEdges(bucket);
  };

  auto filterByNodePattern =
      [&](const std::vector<Node *> &candidates,
          const CypherNodePattern *nodePattern) -> std::vector<Node *> {
    if (!nodePattern)
      return candidates;

    std::unordered_set<Node *> labelAllowed;
    if (!nodePattern->getLabel().empty()) {
      auto labelNodes = matchNodes(nodePattern->getLabel(), "");
      if (labelNodes) {
        for (auto *n : labelNodes->getNodes()) {
          labelAllowed.insert(n);
        }
      }
    }

    std::vector<Node *> filtered;
    filtered.reserve(candidates.size());
    for (auto *n : candidates) {
      if (!nodePattern->getLabel().empty() && !labelAllowed.count(n))
        continue;
      bool ok = true;
      for (const auto &kv : nodePattern->getProperties()) {
        if (getNodeProperty(n, kv.first) != kv.second) {
          ok = false;
          break;
        }
      }
      if (ok) {
        filtered.push_back(n);
      }
    }
    dedupNodes(filtered);
    return filtered;
  };

  std::function<std::vector<Node *>(const CypherPatternElement *,
                                    const std::vector<Node *> &)>
      evalElement;
  evalElement =
      [&](const CypherPatternElement *elem,
          const std::vector<Node *> &inputStarts) -> std::vector<Node *> {
    if (!elem)
      return inputStarts;

    std::vector<Node *> starts =
        filterByNodePattern(inputStarts, elem->getStartNode());
    bindNodes(elem->getStartNode() ? elem->getStartNode()->getVariable() : "",
              starts);
    for (auto *n : starts) {
      result->addNode(n);
    }

    std::vector<Node *> terminals = starts;
    const auto *rel = elem->getRelationship();
    if (rel) {
      std::vector<Node *> endNodes;
      std::vector<Edge *> relEdges;
      for (auto *start : starts) {
        int maxHops = rel->hasVariableLength() ? rel->getMaxHops() : 1;
        if (maxHops < 0)
          maxHops = unboundedMaxHops_;
        auto traversed = traverse(start, *rel, maxHops);
        if (!traversed)
          continue;

        const auto &edges = traversed->getRelationships();
        relEdges.insert(relEdges.end(), edges.begin(), edges.end());
        for (auto *e : edges) {
          result->addEdge(e);
        }

        std::vector<Node *> traversedNodes = traversed->getNodes();
        if (elem->getEndNode()) {
          traversedNodes =
              filterByNodePattern(traversedNodes, elem->getEndNode());
        }
        endNodes.insert(endNodes.end(), traversedNodes.begin(),
                        traversedNodes.end());
      }
      dedupNodes(endNodes);
      dedupEdges(relEdges);
      bindEdges(rel->getVariable(), relEdges);
      if (elem->getEndNode()) {
        bindNodes(elem->getEndNode()->getVariable(), endNodes);
      }
      for (auto *n : endNodes) {
        result->addNode(n);
      }
      terminals = std::move(endNodes);
    }

    if (elem->getNextElements().empty()) {
      return terminals;
    }

    std::vector<Node *> allNextTerminals;
    for (const auto &next : elem->getNextElements()) {
      auto nextTerms = evalElement(next.get(), terminals);
      allNextTerminals.insert(allNextTerminals.end(), nextTerms.begin(),
                              nextTerms.end());
    }
    dedupNodes(allNextTerminals);
    return allNextTerminals;
  };

  const auto *startNodePattern = pattern->getStartNode();
  if (!startNodePattern)
    return result;
  auto allStartNodes = matchNodes(startNodePattern->getLabel(), "");
  if (!allStartNodes)
    return result;
  evalElement(pattern, allStartNodes->getNodes());
  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchNodes(const std::string &label,
                                const std::string &variable) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  static const std::unordered_map<std::string, GraphNodeType> labelMap = {
      {"INST_FUNCALL", GraphNodeType::INST_FUNCALL},
      {"INST_RET", GraphNodeType::INST_RET},
      {"INST_BR", GraphNodeType::INST_BR},
      {"INST_OTHER", GraphNodeType::INST_OTHER},
      {"FUNC_ENTRY", GraphNodeType::FUNC_ENTRY},
      {"PARAM_FORMALIN", GraphNodeType::PARAM_FORMALIN},
      {"PARAM_FORMALOUT", GraphNodeType::PARAM_FORMALOUT},
      {"PARAM_ACTUALIN", GraphNodeType::PARAM_ACTUALIN},
      {"PARAM_ACTUALOUT", GraphNodeType::PARAM_ACTUALOUT},
      {"VAR_OTHER", GraphNodeType::VAR_OTHER},
      {"VAR_STATICALLOCGLOBALSCOPE", GraphNodeType::VAR_STATICALLOCGLOBALSCOPE},
      {"VAR_STATICALLOCMODULESCOPE", GraphNodeType::VAR_STATICALLOCMODULESCOPE},
      {"VAR_STATICALLOCFUNCTIONSCOPE",
       GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE},
      {"FUNC", GraphNodeType::FUNC},
      {"CLASS", GraphNodeType::CLASS},
      {"ANNO_VAR", GraphNodeType::ANNO_VAR},
      {"ANNO_GLOBAL", GraphNodeType::ANNO_GLOBAL},
      {"ANNO_OTHER", GraphNodeType::ANNO_OTHER}};

  if (label.empty()) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
      result->addNode(*it);
    }
  } else {
    const std::string upperLabel = [&]() {
      std::string t = label;
      std::transform(t.begin(), t.end(), t.begin(), ::toupper);
      return t;
    }();

    auto matchesGroup = [&](GraphNodeType t) -> bool {
      if (upperLabel == "INST") {
        return t == GraphNodeType::INST_FUNCALL ||
               t == GraphNodeType::INST_RET || t == GraphNodeType::INST_BR ||
               t == GraphNodeType::INST_OTHER;
      }
      if (upperLabel == "VAR") {
        return t == GraphNodeType::VAR_OTHER ||
               t == GraphNodeType::VAR_STATICALLOCGLOBALSCOPE ||
               t == GraphNodeType::VAR_STATICALLOCMODULESCOPE ||
               t == GraphNodeType::VAR_STATICALLOCFUNCTIONSCOPE;
      }
      if (upperLabel == "PARAM") {
        return t == GraphNodeType::PARAM_FORMALIN ||
               t == GraphNodeType::PARAM_FORMALOUT ||
               t == GraphNodeType::PARAM_ACTUALIN ||
               t == GraphNodeType::PARAM_ACTUALOUT;
      }
      if (upperLabel == "ANNO") {
        return t == GraphNodeType::ANNO_VAR ||
               t == GraphNodeType::ANNO_GLOBAL ||
               t == GraphNodeType::ANNO_OTHER;
      }
      return false;
    };

    auto it = labelMap.find(upperLabel);
    for (auto iter = pdg_.begin(); iter != pdg_.end(); ++iter) {
      const auto t = (*iter)->getNodeType();
      if ((it != labelMap.end() && t == it->second) || matchesGroup(t)) {
        result->addNode(*iter);
      }
    }
  }

  if (!variable.empty()) {
    boundVariables_[variable] = result->getNodes();
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::matchEdges(const std::string &type,
                                const std::string &variable) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);

  static const std::unordered_map<std::string, EdgeType> typeMap = {
      {"DATA_DEF_USE", EdgeType::DATA_DEF_USE},
      {"DATA_RAW", EdgeType::DATA_RAW},
      {"DATA_READ", EdgeType::DATA_READ},
      {"DATA_ALIAS", EdgeType::DATA_ALIAS},
      {"DATA_RET", EdgeType::DATA_RET},
      {"CONTROLDEP_BR", EdgeType::CONTROLDEP_BR},
      {"CONTROLDEP_ENTRY", EdgeType::CONTROLDEP_ENTRY},
      {"CONTROLDEP_CALLINV", EdgeType::CONTROLDEP_CALLINV},
      {"CONTROLDEP_CALLRET", EdgeType::CONTROLDEP_CALLRET},
      {"IND_CALL", EdgeType::IND_CALL},
      {"PARAMETER_IN", EdgeType::PARAMETER_IN},
      {"PARAMETER_OUT", EdgeType::PARAMETER_OUT}};

  if (type.empty()) {
    for (auto it = pdg_.begin(); it != pdg_.end(); ++it) {
      for (auto *edge : (*it)->getOutEdgeSet()) {
        result->addEdge(edge);
      }
    }
  } else {
    auto it = typeMap.find(type);
    if (it != typeMap.end()) {
      for (auto iter = pdg_.begin(); iter != pdg_.end(); ++iter) {
        for (auto *edge : (*iter)->getOutEdgeSet()) {
          if (edge->getEdgeType() == it->second) {
            result->addEdge(edge);
          }
        }
      }
    }
  }

  if (!variable.empty()) {
    boundRelationships_[variable] = result->getRelationships();
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::traverse(Node *start, const CypherRelationshipPattern &rel,
                              int maxHops) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  if (!start)
    return result;

  const int minHops = std::max(1, rel.getMinHops());
  const int effectiveMaxHops = std::max(1, maxHops);

  // Support "TYPE1|TYPE2|..." (OR) for convenience.
  const std::string typeUpper = [&]() {
    std::string t = rel.getType();
    std::transform(t.begin(), t.end(), t.begin(), ::toupper);
    return t;
  }();

  std::vector<std::string> typeAlts;
  if (!typeUpper.empty()) {
    size_t startPos = 0;
    while (startPos <= typeUpper.size()) {
      size_t bar = typeUpper.find('|', startPos);
      if (bar == std::string::npos) {
        typeAlts.push_back(typeUpper.substr(startPos));
        break;
      }
      typeAlts.push_back(typeUpper.substr(startPos, bar - startPos));
      startPos = bar + 1;
    }
  }

  auto matchesAlt = [&](EdgeType t, const std::string &alt) -> bool {
    if (alt.empty())
      return true;
    if (alt == "CONTROL_DEP") {
      return t == EdgeType::CONTROLDEP_ENTRY || t == EdgeType::CONTROLDEP_BR ||
             t == EdgeType::CONTROLDEP_IND_BR ||
             t == EdgeType::CONTROLDEP_CALLINV ||
             t == EdgeType::CONTROLDEP_CALLRET;
    }
    if (alt == "CALL") {
      return t == EdgeType::CONTROLDEP_CALLINV ||
             t == EdgeType::CONTROLDEP_CALLRET || t == EdgeType::IND_CALL;
    }
    if (alt == "DATA_DEP") {
      return t == EdgeType::DATA_DEF_USE;
    }

    static const std::unordered_map<std::string, EdgeType> typeMap = {
        {"DATA_DEF_USE", EdgeType::DATA_DEF_USE},
        {"DATA_RAW", EdgeType::DATA_RAW},
        {"DATA_READ", EdgeType::DATA_READ},
        {"DATA_ALIAS", EdgeType::DATA_ALIAS},
        {"DATA_RET", EdgeType::DATA_RET},
        {"CONTROLDEP_ENTRY", EdgeType::CONTROLDEP_ENTRY},
        {"CONTROLDEP_BR", EdgeType::CONTROLDEP_BR},
        {"CONTROLDEP_IND_BR", EdgeType::CONTROLDEP_IND_BR},
        {"CONTROLDEP_CALLINV", EdgeType::CONTROLDEP_CALLINV},
        {"CONTROLDEP_CALLRET", EdgeType::CONTROLDEP_CALLRET},
        {"CALL_INV", EdgeType::CONTROLDEP_CALLINV},
        {"CALL_RET", EdgeType::CONTROLDEP_CALLRET},
        {"IND_CALL", EdgeType::IND_CALL},
        {"PARAM_IN", EdgeType::PARAMETER_IN},
        {"PARAM_OUT", EdgeType::PARAMETER_OUT},
        {"PARAMETER_IN", EdgeType::PARAMETER_IN},
        {"PARAMETER_OUT", EdgeType::PARAMETER_OUT},
    };

    auto it = typeMap.find(alt);
    return it != typeMap.end() && t == it->second;
  };

  auto edgeMatches = [&](EdgeType t) -> bool {
    if (typeAlts.empty())
      return true;
    for (const auto &alt : typeAlts) {
      if (matchesAlt(t, alt))
        return true;
    }
    return false;
  };

  std::unordered_map<Node *, int> dist;
  dist.emplace(start, 0);

  std::vector<Node *> frontier = {start};
  std::unordered_set<Edge *> visitedEdges;

  for (int hop = 0; hop < effectiveMaxHops && !frontier.empty(); ++hop) {
    std::vector<Node *> next;
    next.reserve(frontier.size() * 2);

    auto visitNeighbor = [&](Edge *edge, Node *neighbor) {
      if (!edgeMatches(edge->getEdgeType()))
        return;
      visitedEdges.insert(edge);
      auto it = dist.find(neighbor);
      if (it != dist.end())
        return;
      const int nd = hop + 1;
      dist.emplace(neighbor, nd);
      if (nd >= minHops)
        result->addNode(neighbor);
      next.push_back(neighbor);
    };

    for (auto *node : frontier) {
      if (rel.getDirection() == CypherRelationshipPattern::Direction::OUT ||
          rel.getDirection() == CypherRelationshipPattern::Direction::BOTH) {
        for (auto *edge : node->getOutEdgeSet()) {
          visitNeighbor(edge, edge->getDstNode());
        }
      }

      if (rel.getDirection() == CypherRelationshipPattern::Direction::IN ||
          rel.getDirection() == CypherRelationshipPattern::Direction::BOTH) {
        for (auto *edge : node->getInEdgeSet()) {
          visitNeighbor(edge, edge->getSrcNode());
        }
      }
    }

    frontier = std::move(next);
  }

  for (auto *e : visitedEdges)
    result->addEdge(e);

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::filterByWhere(const std::vector<Node *> &nodes,
                                   const CypherWhereClause &where) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  for (auto *node : nodes) {
    if (evaluateCondition(where, node)) {
      result->addNode(node);
    }
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::filterByWhere(const std::vector<Edge *> &edges,
                                   const CypherWhereClause &where) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::RELATIONSHIPS);

  for (auto *edge : edges) {
    if (evaluateCondition(where, edge)) {
      result->addEdge(edge);
    }
  }

  return result;
}

bool CypherQueryExecutor::evaluateCondition(
    const CypherWhereClause &condition,
    const CypherQueryExecutor::MatchRow &row) {
  if (condition.isBooleanOp()) {
    if (condition.getBoolOp() == "NOT") {
      const auto *child = condition.getChild();
      return child && !evaluateCondition(*child, row);
    }

    const auto *left = condition.getLeft();
    const auto *right = condition.getRight();
    bool left_result = left ? evaluateCondition(*left, row) : true;
    bool right_result = right ? evaluateCondition(*right, row) : true;

    if (condition.getBoolOp() == "AND")
      return left_result && right_result;
    if (condition.getBoolOp() == "OR")
      return left_result || right_result;
    return false;
  }

  const std::string &var = condition.getVariableName();
  const std::string &prop = condition.getProperty();

  Node *bound_node = nullptr;
  Edge *bound_edge = nullptr;

  if (var.empty()) {
    if (row.nodes.size() == 1 && row.rels.empty())
      bound_node = row.nodes.begin()->second;
    else if (row.rels.size() == 1 && row.nodes.empty())
      bound_edge = row.rels.begin()->second;
  } else {
    auto it_node = row.nodes.find(var);
    if (it_node != row.nodes.end())
      bound_node = it_node->second;
    auto it_rel = row.rels.find(var);
    if (it_rel != row.rels.end())
      bound_edge = it_rel->second;
  }

  if (condition.isExists()) {
    if (prop.empty())
      return bound_node != nullptr || bound_edge != nullptr;
    if (bound_node != nullptr)
      return !getNodeProperty(bound_node, prop).empty();
    if (bound_edge != nullptr)
      return !getEdgeProperty(bound_edge, prop).empty();
    return false;
  }

  std::string lhs;
  if (bound_node != nullptr) {
    lhs = getNodeProperty(bound_node, prop);
  } else if (bound_edge != nullptr) {
    lhs = getEdgeProperty(bound_edge, prop);
  } else {
    return false;
  }

  if (condition.getComparisonOp() == CypherComparisonOp::IN) {
    for (const auto &v : condition.getListValues()) {
      if (lhs == v)
        return true;
    }
    return false;
  }

  if (condition.comparesProperty()) {
    std::string rhs;
    auto node = row.nodes.find(condition.getRightVariable());
    auto edge = row.rels.find(condition.getRightVariable());
    if (node != row.nodes.end())
      rhs = getNodeProperty(node->second, condition.getRightProperty());
    else if (edge != row.rels.end())
      rhs = getEdgeProperty(edge->second, condition.getRightProperty());
    else
      return false;
    // Missing facts are unknown, including under NOT_EQUALS.
    if (lhs.empty() || rhs.empty())
      return false;
    return applyComparison(lhs, condition.getComparisonOp(), rhs);
  }
  return applyComparison(lhs, condition.getComparisonOp(),
                         condition.getValue());
}

bool CypherQueryExecutor::evaluateCondition(const CypherWhereClause &condition,
                                            Node *node) {
  MatchRow row;
  if (!condition.getVariableName().empty())
    row.nodes[condition.getVariableName()] = node;
  else
    row.nodes[""] = node;
  return evaluateCondition(condition, row);
}

bool CypherQueryExecutor::evaluateCondition(const CypherWhereClause &condition,
                                            Edge *edge) {
  MatchRow row;
  if (!condition.getVariableName().empty())
    row.rels[condition.getVariableName()] = edge;
  else
    row.rels[""] = edge;
  return evaluateCondition(condition, row);
}

bool CypherQueryExecutor::applyComparison(const std::string &nodeValue,
                                          CypherComparisonOp op,
                                          const std::string &queryValue) {
  if (op == CypherComparisonOp::LESS_THAN ||
      op == CypherComparisonOp::LESS_THAN_OR_EQUAL ||
      op == CypherComparisonOp::GREATER_THAN ||
      op == CypherComparisonOp::GREATER_THAN_OR_EQUAL) {
    auto parseInteger =
        [](llvm::StringRef text) -> llvm::Optional<llvm::APInt> {
      bool negative = text.consume_front("-");
      llvm::APInt integer;
      if (text.empty() || text.getAsInteger(10, integer))
        return llvm::None;
      integer = integer.zext(integer.getBitWidth() + 1);
      return negative ? -integer : integer;
    };
    auto left = parseInteger(nodeValue), right = parseInteger(queryValue);
    if (left && right) {
      unsigned width = std::max(left->getBitWidth(), right->getBitWidth());
      auto a = left->sextOrTrunc(width), b = right->sextOrTrunc(width);
      if (op == CypherComparisonOp::LESS_THAN)
        return a.slt(b);
      if (op == CypherComparisonOp::LESS_THAN_OR_EQUAL)
        return a.sle(b);
      if (op == CypherComparisonOp::GREATER_THAN)
        return a.sgt(b);
      return a.sge(b);
    }
  }
  switch (op) {
  case CypherComparisonOp::EQUALS:
    return nodeValue == queryValue;
  case CypherComparisonOp::NOT_EQUALS:
    return nodeValue != queryValue;
  case CypherComparisonOp::LESS_THAN:
    try {
      return std::stoll(nodeValue) < std::stoll(queryValue);
    } catch (...) {
      return nodeValue < queryValue;
    }
  case CypherComparisonOp::LESS_THAN_OR_EQUAL:
    try {
      return std::stoll(nodeValue) <= std::stoll(queryValue);
    } catch (...) {
      return nodeValue <= queryValue;
    }
  case CypherComparisonOp::GREATER_THAN:
    try {
      return std::stoll(nodeValue) > std::stoll(queryValue);
    } catch (...) {
      return nodeValue > queryValue;
    }
  case CypherComparisonOp::GREATER_THAN_OR_EQUAL:
    try {
      return std::stoll(nodeValue) >= std::stoll(queryValue);
    } catch (...) {
      return nodeValue >= queryValue;
    }
  case CypherComparisonOp::IS_NULL:
    return nodeValue.empty();
  case CypherComparisonOp::IS_NOT_NULL:
    return !nodeValue.empty();
  case CypherComparisonOp::CONTAINS:
    return nodeValue.find(queryValue) != std::string::npos;
  case CypherComparisonOp::STARTS_WITH:
    return nodeValue.find(queryValue) == 0;
  case CypherComparisonOp::ENDS_WITH:
    return nodeValue.size() >= queryValue.size() &&
           nodeValue.substr(nodeValue.size() - queryValue.size()) == queryValue;
  case CypherComparisonOp::IN:
    return nodeValue == queryValue;
  }
  return true;
}

void CypherQueryExecutor::syncSemanticFacts() {
  if (semanticEpoch_ != pdg_.getEpoch()) {
    boundsFacts_.clear();
    stateFacts_.clear();
    semanticEpoch_ = pdg_.getEpoch();
  }
}

BoundsQuery &CypherQueryExecutor::getBoundsFacts(const llvm::Module &module) {
  syncSemanticFacts();
  auto &facts = boundsFacts_[&module];
  if (!facts)
    facts = std::make_unique<BoundsQuery>(const_cast<llvm::Module &>(module));
  return *facts;
}

std::string CypherQueryExecutor::getNodeProperty(Node *node,
                                                 const std::string &property) {
  if (!node)
    return "";

  const std::string prop = toLower(property);
  if (prop.empty())
    return "";

  if (const auto *inst =
          llvm::dyn_cast_or_null<llvm::Instruction>(node->getValue())) {
    if (prop == "known_access_count" || prop == "out_of_bounds_access_count" ||
        prop == "read_out_of_bounds" || prop == "write_out_of_bounds" ||
        prop == "access_bytes" || prop == "access_capacity_bytes" ||
        prop == "access_offset_bytes") {
      const auto accesses = getBoundsFacts(*inst->getModule()).accesses(*inst);
      if (prop == "known_access_count")
        return std::to_string(accesses.size());
      if (accesses.empty())
        return "";
      size_t bad = 0;
      bool badRead = false, badWrite = false;
      for (const auto &access : accesses) {
        if (access.outOfBounds()) {
          ++bad;
          (access.writes ? badWrite : badRead) = true;
        }
      }
      if (prop == "out_of_bounds_access_count")
        return std::to_string(bad);
      // Absence of a proven bad access is not a proof about unknown accesses.
      if (prop == "read_out_of_bounds")
        return badRead ? "true" : "false";
      if (prop == "write_out_of_bounds")
        return badWrite ? "true" : "false";
      if (accesses.size() == 1) {
        const auto &access = accesses.front();
        if (prop == "access_bytes")
          return std::to_string(access.bytes);
        if (prop == "access_capacity_bytes")
          return std::to_string(access.region.capacity);
        if (prop == "access_offset_bytes")
          return std::to_string(access.region.offset);
      }
      return "";
    }
    if (prop == "pointer_nullness" || prop == "pointee_initialization") {
      const llvm::Value *pointer = nullptr;
      if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(inst))
        pointer = load->getPointerOperand();
      else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(inst))
        pointer = store->getPointerOperand();
      if (!pointer)
        return "";
      syncSemanticFacts();
      auto found = stateFacts_.find(inst->getModule());
      if (found == stateFacts_.end())
        found = stateFacts_
                    .emplace(inst->getModule(),
                             StateQuery().analyze(*inst->getModule()))
                    .first;
      if (found->second.convergence_limit_hit)
        return "";
      const auto state = found->second.stateBefore(*inst, *pointer);
      if (prop == "pointer_nullness") {
        switch (state.nullness) {
        case PointerNullState::Null:
          return "null";
        case PointerNullState::NonNull:
          return "nonnull";
        case PointerNullState::Nullable:
          return "nullable";
        case PointerNullState::Unknown:
          return "unknown";
        }
      }
      switch (state.pointee_initialization) {
      case InitializationState::Uninitialized:
        return "uninitialized";
      case InitializationState::Initialized:
        return "initialized";
      case InitializationState::MaybeUninitialized:
        return "maybe-uninitialized";
      case InitializationState::Unknown:
        return "unknown";
      }
    }
  }

  if (prop == "type" || prop == "type_id" || prop == "node_type" ||
      prop == "node_type_id") {
    return std::to_string(static_cast<int>(node->getNodeType()));
  }

  if (prop == "label" || prop == "kind") {
    return graphNodeTypeName(node->getNodeType());
  }

  if (prop == "func" || prop == "function") {
    if (auto *func = node->getFunc())
      return func->getName().str();
    if (auto *v = node->getValue())
      if (auto *f = llvm::dyn_cast<llvm::Function>(v))
        return f->getName().str();
    return "";
  }

  if (prop == "name") {
    if (auto *v = node->getValue()) {
      if (v->hasName())
        return v->getName().str();
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (i->hasName())
          return i->getName().str();
      }
    }
    // Function-entry nodes have no LLVM value of their own; expose the
    // enclosing function name so documented `n.name = 'main'` queries match.
    if (node->getNodeType() == GraphNodeType::FUNC_ENTRY) {
      if (auto *func = node->getFunc())
        return func->getName().str();
    }
    return "";
  }

  if (prop == "opcode") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        return i->getOpcodeName();
      }
    }
    return "";
  }

  if (prop == "callee") {
    if (auto *v = node->getValue()) {
      if (auto *cb = llvm::dyn_cast<llvm::CallBase>(v)) {
        if (auto *f = ValueFacts::callee(*cb)) {
          return f->getName().str();
        }
        return "<indirect>";
      }
    }
    return "";
  }

  if (const auto *call =
          llvm::dyn_cast_or_null<llvm::CallBase>(node->getValue())) {
    if (prop == "format_output_max_bytes" ||
        prop == "format_output_small_float_max_bytes" ||
        prop == "format_output_value_flow" ||
        prop == "format_output_has_float" ||
        prop == "format_output_is_buffer" ||
        prop == "format_output_limit_arg") {
      const auto *target = ValueFacts::callee(*call);
      auto model = target ? LibraryModels::format(*target) : llvm::None;
      if (!model || model->scanf)
        return "";
      const auto facts = getBoundsFacts(*call->getModule()).formatBounds(*call);
      if (prop == "format_output_is_buffer")
        return facts.output_buffer ? "true" : "false";
      if (prop == "format_output_limit_arg")
        return facts.explicit_limit_argument
                   ? std::to_string(*facts.explicit_limit_argument)
                   : "";
      const auto *format = ValueFacts::argument(*call, model->format);
      auto literal = format ? ValueFacts::constantString(*format) : llvm::None;
      if (!literal || !parseFormat(*literal, false))
        return "";
      if (prop == "format_output_max_bytes")
        return facts.maximum_bytes ? std::to_string(*facts.maximum_bytes) : "";
      if (prop == "format_output_small_float_max_bytes")
        return facts.maximum_bytes_without_large_floats
                   ? std::to_string(*facts.maximum_bytes_without_large_floats)
                   : "";
      if (prop == "format_output_value_flow")
        return facts.value_flow ? "true" : "false";
      return facts.floating_conversion ? "true" : "false";
    }
    const auto *target = ValueFacts::callee(*call);
    if (target) {
      if (prop == "taint_source_kind") {
        auto model = LibraryModels::taint(*call);
        if (!model.sources.empty())
          return "user-input";
        return "";
      }
      if (prop == "taint_source_outputs") {
        auto model = LibraryModels::taint(*call);
        std::string outputs;
        for (const auto &source : model.sources) {
          if (!outputs.empty())
            outputs += ",";
          outputs +=
              source.argument < 0 ? "return" : std::to_string(source.argument);
          outputs += source.channel == TaintChannel::Value ? ":value"
                     : source.channel == TaintChannel::ObjectContent
                         ? ":object-content"
                         : ":content";
        }
        return outputs;
      }
      if (prop == "taint_process_args" || prop == "taint_command_args" ||
          prop == "taint_sql_args" || prop == "taint_path_args" ||
          prop == "taint_format_args" || prop == "taint_allocation_args" ||
          prop == "taint_wordexp_args") {
        TaintDomain domain = TaintDomain::Process;
        if (prop == "taint_command_args")
          domain = TaintDomain::Command;
        if (prop == "taint_sql_args")
          domain = TaintDomain::Sql;
        if (prop == "taint_path_args")
          domain = TaintDomain::Path;
        if (prop == "taint_format_args")
          domain = TaintDomain::Format;
        if (prop == "taint_allocation_args")
          domain = TaintDomain::Allocation;
        if (prop == "taint_wordexp_args")
          domain = TaintDomain::Wordexp;
        std::string positions;
        for (unsigned index : LibraryModels::taintSinks(*call, domain)) {
          if (!positions.empty())
            positions += ",";
          positions += std::to_string(index);
        }
        return positions;
      }
      if (prop == "allocation_kind") {
        switch (LibraryModels::allocation(*target)) {
        case AllocationKind::Malloc:
          return "malloc";
        case AllocationKind::New:
          return "new";
        case AllocationKind::NewArray:
          return "new[]";
        case AllocationKind::Unknown:
          return "";
        }
      }
      if (prop == "release_kind") {
        switch (LibraryModels::release(*target)) {
        case ReleaseKind::Free:
          return "free";
        case ReleaseKind::Delete:
          return "delete";
        case ReleaseKind::DeleteArray:
          return "delete[]";
        case ReleaseKind::Unknown:
          return "";
        }
      }
      auto copy = LibraryModels::copy(*target);
      if (copy) {
        if (prop == "copy_destination_arg")
          return std::to_string(copy->destination);
        if (prop == "copy_source_arg")
          return std::to_string(copy->source);
        if (prop == "copy_size_arg")
          return std::to_string(copy->size);
        if (prop == "copy_destination_bytes")
          if (const auto *argument =
                  ValueFacts::argument(*call, copy->destination))
            if (auto bytes = ValueFacts::objectBytes(*argument))
              return std::to_string(*bytes);
        if (prop == "copy_size_bytes") {
          const auto *argument = ValueFacts::argument(*call, copy->size);
          auto count = argument ? ValueFacts::integer(*argument) : llvm::None;
          if (count && !count->isNegative() && count->getActiveBits() <= 64 &&
              target->arg_size() > copy->destination) {
            auto *param =
                target->getFunctionType()->getParamType(copy->destination);
            if (param->isPointerTy() &&
                param->getPointerElementType()->isIntegerTy()) {
              uint64_t bytes =
                  call->getModule()->getDataLayout().getTypeAllocSize(
                      param->getPointerElementType());
              if (bytes && count->getZExtValue() <= UINT64_MAX / bytes)
                return std::to_string(count->getZExtValue() * bytes);
            }
          }
        }
      }
      auto model = LibraryModels::format(*target);
      if (model) {
        if (prop == "format_arg")
          return std::to_string(model->format);
        if (prop == "format_kind")
          return model->scanf ? "scanf" : "printf";
        if (prop == "format_given" && call->arg_size() >= model->first_argument)
          return std::to_string(call->arg_size() - model->first_argument);
        const auto *argument = ValueFacts::argument(*call, model->format);
        auto literal =
            argument ? ValueFacts::constantString(*argument) : llvm::None;
        auto format =
            literal ? parseFormat(*literal, model->scanf) : llvm::None;
        if (format) {
          if (prop == "format_expected")
            return std::to_string(format->required_arguments);
          if (prop == "format_unbounded_string")
            return format->unbounded_string ? "true" : "false";
        }
      }
    }
    if (prop == "arg_count")
      return std::to_string(call->arg_size());
    if (prop == "callee_param_count") {
      if (const auto *target = ValueFacts::callee(*call))
        return std::to_string(target->arg_size());
      return "";
    }
    // Parameterized properties such as arg2_int and arg0_object_bytes.
    llvm::StringRef suffix(prop);
    if (suffix.consume_front("arg")) {
      auto parts = suffix.split('_');
      unsigned index = 0;
      if (!parts.first.getAsInteger(10, index)) {
        if (const auto *argument = ValueFacts::argument(*call, index)) {
          if (parts.second == "int") {
            if (auto value = ValueFacts::integer(*argument)) {
              llvm::SmallString<40> text;
              value->toString(text, 10, true);
              return text.str().str();
            }
          }
          if (parts.second == "object_bytes")
            if (auto bytes = ValueFacts::objectBytes(*argument))
              return std::to_string(*bytes);
          if (parts.second == "string_termination" ||
              parts.second == "string_min_bytes" ||
              parts.second == "string_max_bytes") {
            if (!argument->getType()->isPointerTy())
              return "";
            const auto facts = getBoundsFacts(*call->getModule())
                                   .stringBounds(*argument, *call);
            if (parts.second == "string_min_bytes")
              return facts.minimum_bytes ? std::to_string(*facts.minimum_bytes)
                                         : "";
            if (parts.second == "string_max_bytes")
              return facts.maximum_bytes ? std::to_string(*facts.maximum_bytes)
                                         : "";
            switch (facts.termination) {
            case StringTermination::Terminated:
              return "terminated";
            case StringTermination::Unproven:
              return "unproven";
            case StringTermination::Unknown:
              return "unknown";
            }
          }
        }
      }
    }
  }

  if (prop == "src_file" || prop == "source_file") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return loc->getFilename().str();
        }
      }
    }
    return "";
  }

  if (prop == "src_line" || prop == "source_line") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return std::to_string(loc->getLine());
        }
      }
    }
    return "";
  }

  if (prop == "src_col" || prop == "source_col" || prop == "source_column") {
    if (auto *v = node->getValue()) {
      if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (const llvm::DebugLoc &dl = i->getDebugLoc()) {
          auto *loc = dl.get();
          if (!loc)
            return "";
          return std::to_string(loc->getColumn());
        }
      }
    }
    return "";
  }

  if (prop == "src" || prop == "source") {
    const std::string file = getNodeProperty(node, "src_file");
    const std::string line = getNodeProperty(node, "src_line");
    const std::string col = getNodeProperty(node, "src_col");
    if (file.empty() && line.empty() && col.empty())
      return "";
    std::ostringstream oss;
    oss << file;
    if (!line.empty())
      oss << ":" << line;
    if (!col.empty())
      oss << ":" << col;
    return oss.str();
  }

  if (prop == "di_type" || prop == "dtype" || prop == "type_name") {
    if (auto *dt = node->getDIType()) {
      return dbgutils::getSourceLevelTypeName(*dt);
    }
    return "";
  }

  if (prop == "llvm" || prop == "ir") {
    if (auto *v = node->getValue()) {
      std::string s;
      llvm::raw_string_ostream os(s);
      v->print(os);
      os.flush();
      return s;
    }
    return "";
  }

  return "";
}

std::string CypherQueryExecutor::getEdgeProperty(Edge *edge,
                                                 const std::string &property) {
  if (!edge)
    return "";

  const std::string prop = toLower(property);
  if (prop == "type" || prop == "type_id" || prop == "edge_type" ||
      prop == "edge_type_id") {
    return std::to_string(static_cast<int>(edge->getEdgeType()));
  }
  if (prop == "label" || prop == "kind") {
    return edgeTypeName(edge->getEdgeType());
  }

  if (prop == "src") {
    return getNodeProperty(edge->getSrcNode(), "label");
  }
  if (prop == "dst") {
    return getNodeProperty(edge->getDstNode(), "label");
  }

  if (prop.rfind("src_", 0) == 0) {
    return getNodeProperty(edge->getSrcNode(), prop.substr(4));
  }
  if (prop.rfind("dst_", 0) == 0) {
    return getNodeProperty(edge->getDstNode(), prop.substr(4));
  }

  return "";
}

// ============================================================================
// Optimizer query functions
// ============================================================================

std::unique_ptr<CypherResult>
CypherQueryExecutor::canMoveEarlier(Node *moving, Node *anchor) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::BOOLEAN);
  if (!moving || !anchor) {
    result->setBooleanValue(false);
    return result;
  }

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  MotionCheckResult legality =
      query.canMoveEarlier(*moving, *anchor, llvm_context);
  result->setBooleanValue(legality.legal);
  return result;
}

std::unique_ptr<CypherResult> CypherQueryExecutor::canMoveLater(Node *moving,
                                                                Node *anchor) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::BOOLEAN);
  if (!moving || !anchor) {
    result->setBooleanValue(false);
    return result;
  }

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  MotionCheckResult legality =
      query.canMoveLater(*moving, *anchor, llvm_context);
  result->setBooleanValue(legality.legal);
  return result;
}

std::unique_ptr<CypherResult> CypherQueryExecutor::independent(Node *a,
                                                               Node *b) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::BOOLEAN);
  if (!a || !b) {
    result->setBooleanValue(false);
    return result;
  }

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  IndependenceCheckResult indep = query.independent(*a, *b, llvm_context);
  result->setBooleanValue(indep.independent);
  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::readySet(const std::vector<Node *> &region,
                              const std::vector<Node *> &scheduled) {
  auto result = std::make_unique<CypherResult>(CypherResult::ResultType::NODES);

  std::set<Node *> region_set(region.begin(), region.end());
  std::set<Node *> scheduled_set(scheduled.begin(), scheduled.end());

  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  PDGQueryResult ready = query.readySet(PDGQueryScope::nodeSet(region_set),
                                        scheduled_set, llvm_context);

  for (Node *n : ready.nodes) {
    result->addNode(n);
  }

  return result;
}

std::unique_ptr<CypherResult>
CypherQueryExecutor::criticalPath(const std::vector<Node *> &region) {
  auto result =
      std::make_unique<CypherResult>(CypherResult::ResultType::INTEGER);

  std::set<Node *> region_set(region.begin(), region.end());
  TransformQuery query(pdg_);
  LLVMQueryContext llvm_context;
  size_t length = query.criticalPathLength(PDGQueryScope::nodeSet(region_set),
                                           llvm_context);

  result->setIntegerValue(static_cast<int64_t>(length));
  return result;
}

} // namespace pdg
