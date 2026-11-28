/**
 * @file JoinTargetAnalysis.cpp
 * @brief Join-target analysis facade and orchestration
 */

#include "Concurrency/Thread/Join/JoinTargetAnalysis.h"

#include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <llvm/IR/InstIterator.h>
#include <algorithm>

using namespace llvm;
using namespace lotus;

namespace mhp {

JoinTargetAnalysis::JoinTargetAnalysis(Module &module,
                                       AliasAnalysisWrapper *aliasAnalysis)
    : m_module(module), m_threadAPI(ThreadAPI::getThreadAPI()),
      m_aliasAnalysis(aliasAnalysis) {}

void JoinTargetAnalysis::analyze() {
  collectForksAndJoins();
  m_joinResolutions.clear();
  m_functionSummaries.clear();
  m_blockInStates.clear();
  m_blockOutStates.clear();

  if (m_joinInsts.empty()) {
    return;
  }

  if (!m_threadMultiplicity) {
    m_threadMultiplicity =
        std::make_unique<concurrency::ThreadMultiplicityAnalysis>(m_module);
  }

  buildFunctionSummaries();

  std::unordered_set<const Function *> joinFunctions;
  joinFunctions.reserve(m_joinInsts.size());
  for (const Instruction *joinInst : m_joinInsts) {
    if (joinInst && joinInst->getFunction() &&
        !joinInst->getFunction()->isDeclaration()) {
      joinFunctions.insert(joinInst->getFunction());
    }
  }

  for (const Function *func : joinFunctions) {
    analyzeFunction(*func);
  }

  for (const Instruction *joinInst : m_joinInsts) {
    auto it = m_joinResolutions.find(joinInst);
    if (it == m_joinResolutions.end()) {
      continue;
    }
    finalizeJoinResolution(joinInst, it->second);
  }
}

void JoinTargetAnalysis::collectForksAndJoins() {
  m_forkInsts.clear();
  m_joinInsts.clear();
  for (Function &F : m_module) {
    if (F.isDeclaration()) {
      continue;
    }
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      const Instruction *inst = &*I;
      if (m_threadAPI->isTDFork(inst)) {
        m_forkInsts.push_back(inst);
      } else if (m_threadAPI->isTDJoin(inst)) {
        m_joinInsts.push_back(inst);
      }
    }
  }
}

std::vector<const Instruction *>
JoinTargetAnalysis::getPossibleJoinedForks(const Instruction *joinInst) const {
  return getJoinResolution(joinInst).possible_forks;
}

std::vector<const Instruction *>
JoinTargetAnalysis::getFeasibleJoinedForks(const Instruction *joinInst) const {
  return getJoinResolution(joinInst).feasible_forks;
}

std::vector<ThreadInstance>
JoinTargetAnalysis::getPossibleJoinedInstances(const Instruction *joinInst) const {
  return getJoinResolution(joinInst).possible_instances;
}

std::vector<ThreadInstance>
JoinTargetAnalysis::getFeasibleJoinedInstances(const Instruction *joinInst) const {
  return getJoinResolution(joinInst).feasible_instances;
}

const Instruction *JoinTargetAnalysis::getDefiniteFeasibleJoinedFork(
    const Instruction *joinInst) const {
  if (!isUnambiguousJoin(joinInst)) {
    return nullptr;
  }
  auto feasible = getFeasibleJoinedForks(joinInst);
  if (feasible.size() != 1) {
    return nullptr;
  }
  return feasible.front();
}

bool JoinTargetAnalysis::isUnambiguousJoin(const Instruction *joinInst) const {
  return getJoinResolution(joinInst).unambiguous;
}

JoinResolution
JoinTargetAnalysis::getJoinResolution(const Instruction *joinInst) const {
  auto it = m_joinResolutions.find(joinInst);
  if (it != m_joinResolutions.end()) {
    return it->second;
  }
  return {};
}

JoinTargetAnalysis::CandidateCountKind JoinTargetAnalysis::classifyJoinForks(
    const std::vector<const Instruction *> &forks) const {
  if (forks.empty()) {
    return CandidateCountKind::Zero;
  }
  if (forks.size() == 1) {
    return CandidateCountKind::One;
  }
  return CandidateCountKind::Many;
}

} // namespace mhp
