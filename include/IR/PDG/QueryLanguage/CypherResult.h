#pragma once

#include "IR/PDG/QueryLanguage/CypherAST.h"

namespace pdg {

/**
 * @brief Result of query execution
 */
class CypherResult {
public:
  enum class ResultType {
    NODES,
    RELATIONSHIPS,
    PATHS,
    SCALAR,
    INTEGER,
    BOOLEAN
  };

  CypherResult(ResultType type = ResultType::NODES) : type_(type) {}

  void addNode(Node *node) { nodes_.push_back(node); }

  void addEdge(Edge *edge) { relationships_.push_back(edge); }

  void addRelationship(Edge *edge) { relationships_.push_back(edge); }

  void setScalarValue(const std::string &value) {
    type_ = ResultType::SCALAR;
    scalarValue_ = value;
  }

  void setIntegerValue(int64_t value) {
    type_ = ResultType::INTEGER;
    integerValue_ = value;
  }

  void setBooleanValue(bool value) {
    type_ = ResultType::BOOLEAN;
    booleanValue_ = value;
  }

  ResultType getType() const { return type_; }
  const std::vector<Node *> &getNodes() const { return nodes_; }
  const std::vector<Edge *> &getRelationships() const { return relationships_; }
  const std::string &getScalarValue() const { return scalarValue_; }
  int64_t getIntegerValue() const { return integerValue_; }
  bool getBooleanValue() const { return booleanValue_; }

  std::string toString() const;
  bool isEmpty() const {
    return nodes_.empty() && relationships_.empty() && scalarValue_.empty() &&
           integerValue_ == 0 && !booleanValue_;
  }

  size_t getCount() const {
    switch (type_) {
    case ResultType::NODES:
      return nodes_.size();
    case ResultType::RELATIONSHIPS:
      return relationships_.size();
    case ResultType::INTEGER:
      return 1;
    case ResultType::BOOLEAN:
      return 1;
    default:
      return scalarValue_.empty() ? 0 : 1;
    }
  }

private:
  ResultType type_;
  std::vector<Node *> nodes_;
  std::vector<Edge *> relationships_;
  std::string scalarValue_;
  int64_t integerValue_ = 0;
  bool booleanValue_ = false;
};

} // namespace pdg
