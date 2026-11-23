#pragma once

#include "llvm/IR/Module.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pdg {

struct ProtocolRuleDescriptor {
  std::string id, severity, codeql_query, coverage;
};

struct ProtocolFinding {
  std::string rule_id, message;
  const llvm::Instruction *site;
  std::vector<const llvm::Instruction *> evidence;
};

struct ProtocolQueryResult {
  std::vector<ProtocolFinding> findings;
  bool incomplete = false;
};

/// Bounded, IR-visible protocol candidates. Thread launch overlap, concrete
/// shared objects and ordered CFG events are required; arbitrary dependence
/// reachability is never interpreted as a race or filesystem protocol.
class ProtocolQuery {
public:
  static const std::vector<ProtocolRuleDescriptor> &catalog();
  ProtocolQueryResult analyze(const llvm::Module &module,
                              size_t max_states = 4096) const;
};

} // namespace pdg
