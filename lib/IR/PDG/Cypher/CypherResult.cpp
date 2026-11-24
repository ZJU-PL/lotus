#include "IR/PDG/Cypher/CypherResult.h"

#include <sstream>

namespace pdg {

// ============================================================================
// CypherResult implementation
// ============================================================================

std::string CypherResult::toString() const {
  std::ostringstream oss;

  switch (type_) {
  case ResultType::NODES:
    oss << "Result(" << nodes_.size() << " nodes)";
    break;
  case ResultType::RELATIONSHIPS:
    oss << "Result(" << relationships_.size() << " relationships)";
    break;
  case ResultType::PATHS:
    oss << "Result(paths)";
    break;
  case ResultType::SCALAR:
    oss << scalarValue_;
    break;
  case ResultType::INTEGER:
    oss << integerValue_;
    break;
  case ResultType::BOOLEAN:
    oss << (booleanValue_ ? "true" : "false");
    break;
  }

  return oss.str();
}

} // namespace pdg
