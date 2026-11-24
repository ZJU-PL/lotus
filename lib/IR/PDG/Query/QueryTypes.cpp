#include "IR/PDG/Query/QueryTypes.h"

#include "IR/PDG/Query/QuerySupport.h"
#include "IR/PDG/Support/PDGUtils.h"

#include <sstream>

namespace pdg {
using namespace llvm;
using namespace query_detail;

std::set<EdgeType> edgeTypesForPreset(PDGEdgePreset preset) {
  switch (preset) {
  case PDGEdgePreset::All:
    return {EdgeType::IND_CALL,
            EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY,
            EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR,
            EdgeType::DATA_DEF_USE,
            EdgeType::DATA_RAW,
            EdgeType::DATA_READ,
            EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD,
            EdgeType::GLOBAL_DEP,
            EdgeType::VAL_DEP,
            EdgeType::CLS_MTH,
            EdgeType::ANNO_VAR,
            EdgeType::ANNO_GLOBAL,
            EdgeType::ANNO_OTHER,
            EdgeType::TYPE_OTHEREDGE};
  case PDGEdgePreset::Data:
    return {EdgeType::DATA_DEF_USE,  EdgeType::DATA_RAW,
            EdgeType::DATA_READ,     EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,      EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT, EdgeType::PARAMETER_FIELD,
            EdgeType::GLOBAL_DEP,    EdgeType::VAL_DEP};
  case PDGEdgePreset::Control:
    return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR};
  case PDGEdgePreset::Parameter:
    return {EdgeType::PARAMETER_IN, EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD};
  case PDGEdgePreset::Interprocedural:
    return {EdgeType::IND_CALL,           EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET, EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN,       EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD};
  case PDGEdgePreset::ValueFlow:
    return {EdgeType::DATA_DEF_USE,    EdgeType::DATA_RAW,
            EdgeType::DATA_READ,       EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,        EdgeType::VAL_DEP,
            EdgeType::PARAMETER_IN,    EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD, EdgeType::GLOBAL_DEP};
  case PDGEdgePreset::TransformLegality:
    return {EdgeType::DATA_DEF_USE,
            EdgeType::DATA_RAW,
            EdgeType::DATA_READ,
            EdgeType::DATA_ALIAS,
            EdgeType::DATA_RET,
            EdgeType::PARAMETER_IN,
            EdgeType::PARAMETER_OUT,
            EdgeType::PARAMETER_FIELD,
            EdgeType::VAL_DEP,
            EdgeType::GLOBAL_DEP,
            EdgeType::CONTROLDEP_CALLINV,
            EdgeType::CONTROLDEP_CALLRET,
            EdgeType::CONTROLDEP_ENTRY,
            EdgeType::CONTROLDEP_BR,
            EdgeType::CONTROLDEP_IND_BR};
  }
  return std::set<EdgeType>();
}

std::string describeNode(Node *node) {
  if (node == nullptr)
    return "<null>";
  std::string description = pdgutils::getNodeTypeStr(node->getNodeType());
  if (node->getValue() != nullptr) {
    description += " ";
    description += stringifyValue(node->getValue());
  }
  return description;
}

std::string stableNodeKey(Node *node) {
  if (node == nullptr)
    return "<null>";

  std::ostringstream os;
  os << pdgutils::getNodeTypeStr(node->getNodeType()) << "|"
     << functionNameForNode(node) << "|";
  const std::string source = sourceKeyForNode(node);
  if (!source.empty())
    os << source;
  else if (node->getValue() != nullptr)
    os << stringifyValue(node->getValue());
  else
    os << pointerKey(node);
  return os.str();
}

} // namespace pdg
