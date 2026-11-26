//===- MemorySSADump.cpp - textual sparse MemorySSA dump -----------------===//
#include "IR/SVFG/MemorySSADump.h"

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace lotus {
namespace analysis {
namespace {

enum class MemoryRole { Phi, Mu, Chi };

const char *roleName(MemoryRole role) {
  switch (role) {
  case MemoryRole::Phi:
    return "PHI";
  case MemoryRole::Mu:
    return "MU";
  case MemoryRole::Chi:
    return "CHI";
  }
  llvm_unreachable("unhandled memory role");
}

bool classifyMemoryNode(const SVFGNode *node, MemoryRole &role) {
  switch (node->getNodeKind()) {
  case SVFGK::MPhi:
  case SVFGK::MIntraPhi:
  case SVFGK::MInterPhi:
    role = MemoryRole::Phi;
    return true;
  case SVFGK::FormalOut:
  case SVFGK::ActualIn:
  case SVFGK::LoadMu:
  case SVFGK::CallMu:
  case SVFGK::RetMu:
    role = MemoryRole::Mu;
    return true;
  case SVFGK::FormalIn:
  case SVFGK::ActualOut:
  case SVFGK::StoreChi:
  case SVFGK::CallChi:
  case SVFGK::EntryChi:
    role = MemoryRole::Chi;
    return true;
  case SVFGK::Load:
    // The runtime sparse builder attaches the load MU directly to Load.
    role = MemoryRole::Mu;
    return node->getMemReg() != 0;
  case SVFGK::Store:
    // The runtime sparse builder attaches the store CHI directly to Store.
    role = MemoryRole::Chi;
    return node->getMemReg() != 0;
  default:
    return false;
  }
}

const char *kindName(SVFGK kind) {
  switch (kind) {
  case SVFGK::MPhi: return "MemoryPhi";
  case SVFGK::MIntraPhi: return "MemoryPhi";
  case SVFGK::MInterPhi: return "MemoryPhi";
  case SVFGK::FormalIn: return "FormalIn";
  case SVFGK::FormalOut: return "FormalOut";
  case SVFGK::ActualIn: return "ActualIn";
  case SVFGK::ActualOut: return "ActualOut";
  case SVFGK::LoadMu: return "LoadMu";
  case SVFGK::StoreChi: return "StoreChi";
  case SVFGK::CallMu: return "CallMu";
  case SVFGK::CallChi: return "CallChi";
  case SVFGK::RetMu: return "RetMu";
  case SVFGK::EntryChi: return "EntryChi";
  case SVFGK::Load: return "Load";
  case SVFGK::Store: return "Store";
  default: return "Memory";
  }
}

const llvm::Instruction *memoryInstruction(const SVFGNode *node) {
  return node->getInstruction();
}

const llvm::Function *ownerFunction(const SVFGNode *node) {
  if (const llvm::Function *function = node->getFunction())
    return function;
  if (const ::ICFGNode *icfgNode = node->getICFGNode())
    if (const llvm::Function *function = icfgNode->getFunction())
      return function;
  if (const llvm::CallBase *call = node->getCallSite())
    return call->getFunction();
  return nullptr;
}

const llvm::BasicBlock *ownerBlock(const SVFGNode *node) {
  if (const ::ICFGNode *icfgNode = node->getICFGNode())
    if (const llvm::BasicBlock *block = icfgNode->getBasicBlock())
      return block;
  if (const llvm::Instruction *instruction = memoryInstruction(node))
    return instruction->getParent();
  return nullptr;
}

void printPointsTo(const SVFGNode *node, llvm::raw_ostream &output) {
  const SVFGNodeBS *pointsTo = node->getPointsTo();
  output << '{';
  if (pointsTo) {
    bool first = true;
    for (uint32_t object : *pointsTo) {
      if (!first)
        output << ',';
      output << object;
      first = false;
    }
  }
  output << '}';
}

void printOperands(const SVFGNode *node, llvm::raw_ostream &output) {
  const auto *phi = llvm::dyn_cast<MSSAPhiSVFGNode>(node);
  if (!phi)
    return;
  output << " operands={";
  bool first = true;
  for (auto it = phi->opVerBegin(), end = phi->opVerEnd(); it != end; ++it) {
    const uint32_t position = it->first;
    const auto &operand = it->second;
    if (!first)
      output << ',';
    output << position << ":mem" << operand.memReg << '.' << operand.version;
    first = false;
  }
  output << '}';
}

void printMemoryNode(const SVFGNode *node, MemoryRole role,
                     llvm::raw_ostream &output) {
  output << "  " << roleName(role) << " node=" << node->getId()
         << " kind=" << kindName(node->getNodeKind())
         << " memReg=" << node->getMemReg()
         << " version=" << node->getSSAVersion() << " points-to=";
  printPointsTo(node, output);
  printOperands(node, output);
  if (const llvm::Instruction *instruction = memoryInstruction(node)) {
    output << " inst=";
    instruction->print(output);
  } else if (const llvm::CallBase *call = node->getCallSite()) {
    output << " call=";
    call->print(output);
  }
  output << '\n';
}

} // namespace

bool dumpMemorySSA(const SVFG &graph, const llvm::Module &module,
                   llvm::raw_ostream &output, llvm::StringRef functionFilter,
                   std::string *error) {
  using NodeList = std::vector<std::pair<const SVFGNode *, MemoryRole>>;
  std::map<const llvm::Function *, std::map<const llvm::BasicBlock *, NodeList>>
      functionNodes;
  NodeList moduleNodes;
  std::set<const SVFGNode *> emitted;

  for (const auto &entry : graph) {
    const SVFGNode *node = entry.second;
    MemoryRole role;
    if (!classifyMemoryNode(node, role))
      continue;
    const llvm::Function *function = ownerFunction(node);
    const llvm::BasicBlock *block = ownerBlock(node);
    if (function && block) {
      functionNodes[function][block].push_back({node, role});
      emitted.insert(node);
    } else if (!function) {
      moduleNodes.push_back({node, role});
      emitted.insert(node);
    }
  }

  bool filterMatched = functionFilter.empty();
  if (!functionFilter.empty()) {
    const llvm::Function *function = module.getFunction(functionFilter);
    filterMatched = function && !function->isDeclaration();
  }

  // A node whose ICFG location cannot be attributed to a BB/function is still
  // shown rather than silently dropped.
  std::map<const llvm::Function *, NodeList> unmatchedFunctionNodes;
  for (const auto &entry : graph) {
    const SVFGNode *node = entry.second;
    MemoryRole role;
    if (!classifyMemoryNode(node, role) || emitted.count(node))
      continue;
    if (const llvm::Function *function = ownerFunction(node))
      unmatchedFunctionNodes[function].push_back({node, role});
    else
      moduleNodes.push_back({node, role});
  }

  auto roleOrder = [](MemoryRole role) {
    switch (role) {
    case MemoryRole::Phi: return 0;
    case MemoryRole::Mu: return 1;
    case MemoryRole::Chi: return 2;
    }
    return 3;
  };
  auto sortNodes = [&](NodeList &nodes) {
    std::stable_sort(nodes.begin(), nodes.end(), [&](const auto &lhs,
                                                     const auto &rhs) {
      if (roleOrder(lhs.second) != roleOrder(rhs.second))
        return roleOrder(lhs.second) < roleOrder(rhs.second);
      return lhs.first->getId() < rhs.first->getId();
    });
  };
  sortNodes(moduleNodes);
  for (auto &[function, nodes] : unmatchedFunctionNodes)
    sortNodes(nodes);
  for (auto &[function, blocks] : functionNodes)
    for (auto &[block, nodes] : blocks)
      sortNodes(nodes);

  if (!moduleNodes.empty() &&
      (functionFilter.empty() || functionFilter == "<module>")) {
    filterMatched = true;
    output << "==========FUNCTION: <module>==========\n"
           << "<global-init>\n";
    for (const auto &[node, role] : moduleNodes)
      printMemoryNode(node, role, output);
  }

  for (const llvm::Function &function : module) {
    if (function.isDeclaration())
      continue;
    if (!functionFilter.empty() && functionFilter != function.getName())
      continue;
    filterMatched = true;
    output << "==========FUNCTION: " << function.getName()
           << "==========\n";
    auto functionIt = functionNodes.find(&function);
    for (const llvm::BasicBlock &block : function) {
      output << (block.hasName() ? block.getName() : "<unnamed>") << "\n";
      if (functionIt != functionNodes.end()) {
        auto blockIt = functionIt->second.find(&block);
        if (blockIt != functionIt->second.end())
          for (const auto &[node, role] : blockIt->second)
            printMemoryNode(node, role, output);
      }
    }
    auto unmatchedIt = unmatchedFunctionNodes.find(&function);
    if (unmatchedIt != unmatchedFunctionNodes.end()) {
      output << "<unattributed>\n";
      for (const auto &[node, role] : unmatchedIt->second)
        printMemoryNode(node, role, output);
    }
  }

  if (!filterMatched) {
    if (error)
      *error = "function '" + functionFilter.str() + "' was not found";
    return false;
  }
  return true;
}

} // namespace analysis
} // namespace lotus
