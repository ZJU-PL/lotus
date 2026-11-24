#include "Analysis/Summary/ParameterSummary/ParameterEffectSummary.h"
#include "Analysis/Summary/ParameterSummary/ResourceTable.h"

#include "IR/ICFG/CallGraph.h"

#include <queue>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>

namespace lotus::analysis::parametersummary {
namespace {

const llvm::Value *normalizeValue(const llvm::Value *value) {
  return value == nullptr ? nullptr : value->stripPointerCasts();
}

bool lookupParamIndex(const llvm::DenseMap<const llvm::Value *, unsigned> &params,
                      const llvm::Value *value, unsigned &param_index) {
  auto it = params.find(normalizeValue(value));
  if (it == params.end())
    return false;
  param_index = it->second;
  return true;
}

llvm::DenseMap<const llvm::Value *, unsigned>
buildParamIndexMap(const llvm::Function &function) {
  llvm::DenseMap<const llvm::Value *, unsigned> params;
  for (const llvm::Argument &argument : function.args())
    params[normalizeValue(&argument)] = argument.getArgNo();
  return params;
}

llvm::SmallPtrSet<const llvm::Value *, 8>
collectReturnedValues(const llvm::Function &function) {
  llvm::SmallPtrSet<const llvm::Value *, 8> returned_values;
  for (const llvm::Instruction &instruction : llvm::instructions(function)) {
    auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction);
    if (!ret)
      continue;
    if (const llvm::Value *value = normalizeValue(ret->getReturnValue()))
      returned_values.insert(value);
  }
  return returned_values;
}

ParameterEffectSummary buildDeclarationSummary(
    const llvm::Function &function, const ResourceTable &table) {
  ParameterEffectSummary summary;
  summary.func = &function;

  llvm::StringRef name = function.getName();
  if (table.hasRole(name, ResourceRole::Deallocator) ||
      table.hasRole(name, ResourceRole::Release) ||
      table.hasRole(name, ResourceRole::Unlock)) {
    summary.paramFreed[0] = true;
  }
  if (table.hasRole(name, ResourceRole::Dereference))
    summary.paramDereferenced[0] = true;
  if (table.hasRole(name, ResourceRole::Allocator))
    summary.returnIsAllocated = true;

  return summary;
}

bool hasTopologicalOrdering(llvm::Module &module) {
  LTCallGraph call_graph(module);
  llvm::DenseMap<const llvm::Function *, unsigned> indegree;
  llvm::DenseMap<const llvm::Function *, std::vector<const llvm::Function *>>
      outgoing;

  for (const llvm::Function &function : module) {
    if (function.isIntrinsic())
      continue;
    indegree[&function] = 0;
  }

  for (const llvm::Function &function : module) {
    if (function.isIntrinsic())
      continue;

    auto *node = call_graph[&function];
    if (!node)
      continue;

    for (const auto &call_record : *node) {
      const llvm::Function *callee = call_record.second->getFunction();
      if (!callee || callee->isIntrinsic())
        continue;
      if (!indegree.count(callee))
        continue;
      outgoing[&function].push_back(callee);
      ++indegree[callee];
    }
  }

  std::queue<const llvm::Function *> ready;
  for (const auto &entry : indegree) {
    if (entry.second == 0)
      ready.push(entry.first);
  }

  size_t visited = 0;

  while (!ready.empty()) {
    const llvm::Function *function = ready.front();
    ready.pop();
    ++visited;

    auto out_it = outgoing.find(function);
    if (out_it == outgoing.end())
      continue;

    for (const llvm::Function *callee : out_it->second) {
      unsigned &count = indegree[callee];
      if (--count == 0)
        ready.push(callee);
    }
  }

  return visited == indegree.size();
}

ParameterEffectSummary computeDefinedFunctionSummary(
    const llvm::Function &function, const ParameterEffectSummaryMap &summaries,
    bool compose_transitively) {
  ParameterEffectSummary summary;
  summary.func = &function;

  auto params = buildParamIndexMap(function);
  auto returned_values = collectReturnedValues(function);

  for (const llvm::Instruction &instruction : llvm::instructions(function)) {
    if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction)) {
      unsigned param_index = 0;
      if (lookupParamIndex(params, load->getPointerOperand(), param_index))
        summary.paramDereferenced[param_index] = true;
      continue;
    }

    if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
      unsigned param_index = 0;
      if (lookupParamIndex(params, store->getPointerOperand(), param_index))
        summary.paramDereferenced[param_index] = true;
      continue;
    }

    const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
    if (!call || !compose_transitively)
      continue;

    const llvm::Function *callee = call->getCalledFunction();
    if (!callee)
      continue;

    auto callee_it = summaries.find(callee);
    if (callee_it == summaries.end())
      continue;

    const ParameterEffectSummary &callee_summary = callee_it->second;
    for (unsigned arg_index = 0; arg_index < call->arg_size(); ++arg_index) {
      unsigned param_index = 0;
      if (!lookupParamIndex(params, call->getArgOperand(arg_index), param_index))
        continue;

      if (callee_summary.paramFreed.lookup(arg_index))
        summary.paramFreed[param_index] = true;
      if (callee_summary.paramDereferenced.lookup(arg_index))
        summary.paramDereferenced[param_index] = true;
    }

    if (callee_summary.returnIsAllocated && !call->getType()->isVoidTy() &&
        returned_values.contains(normalizeValue(call))) {
      summary.returnIsAllocated = true;
    }
  }

  return summary;
}

} // namespace

ParameterEffectSummaryMap computeParameterEffectSummaries(
    llvm::Module &module, const ResourceTable &table) {
  ParameterEffectSummaryMap summaries;
  const bool compose_transitively = hasTopologicalOrdering(module);

  for (llvm::Function &function : module) {
    if (function.isIntrinsic())
      continue;
    if (function.isDeclaration()) {
      summaries[&function] = buildDeclarationSummary(function, table);
    }
  }

  for (llvm::Function &function : module) {
    if (function.isIntrinsic() || function.isDeclaration())
      continue;
    summaries[&function] =
        computeDefinedFunctionSummary(function, summaries, compose_transitively);
  }

  return summaries;
}

ParameterEffectSummaryMap computeParameterEffectSummaries(llvm::Module &module) {
  ResourceTable table = ResourceTable::fromModuleSpecs(module);
  return computeParameterEffectSummaries(module, table);
}

} // namespace lotus::analysis::parametersummary
