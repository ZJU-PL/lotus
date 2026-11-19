// A separately linked observer of the unmodified, locally built SVF solvers.
#include "Alias/InclusionBased/FlowSensitive/Support/ComparisonSampling.h"

#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/FlowSensitive.h"
#include "WPA/VersionedFlowSensitive.h"

#include <llvm/IR/InstIterator.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <memory>
#include <string>

static Option<unsigned> comparisonLimit(
    "comparison-limit", "Maximum pointer values in comparison JSON", 512);
static Option<unsigned> comparisonSeed(
    "comparison-sample-seed", "Stable hash query sample seed", 0);

int main(int argc, char **argv) {
  const auto inputs = OptionBase::parseOptions(
      argc, argv, "SVF flow-sensitive differential observer", "<input.bc>");
  if (inputs.size() != 1 ||
      SVF::Options::PASelected(SVF::PTATY::FSSPARSE_WPA) ==
          SVF::Options::PASelected(SVF::PTATY::VFS_WPA)) {
    llvm::errs() << "Specify one input and exactly one of -fspta/-vfspta\n";
    return 1;
  }
  SVF::LLVMModuleSet::buildSVFModule(inputs);
  auto *modules = SVF::LLVMModuleSet::getLLVMModuleSet();
  SVF::SVFIRBuilder builder;
  auto *pag = builder.build();
  std::unique_ptr<SVF::FlowSensitive> solver;
  if (SVF::Options::PASelected(SVF::PTATY::VFS_WPA))
    solver = std::make_unique<SVF::VersionedFlowSensitive>(pag);
  else
    solver = std::make_unique<SVF::FlowSensitive>(pag);
  solver->analyze();

  std::map<std::string, SVF::NodeID> values;
  auto addValue = [&](const llvm::Value &value) {
    if (!value.getType()->isPointerTy() || !modules->hasValueNode(&value))
      return;
    const llvm::Function *function = nullptr;
    if (auto *inst = llvm::dyn_cast<llvm::Instruction>(&value))
      function = inst->getFunction();
    else if (auto *arg = llvm::dyn_cast<llvm::Argument>(&value))
      function = arg->getParent();
    std::string key = function ? function->getName().str() : "@";
    key += "::";
    if (value.hasName())
      key += value.getName().str();
    else {
      llvm::raw_string_ostream stream(key);
      value.printAsOperand(stream, false);
    }
    values.emplace(std::move(key), modules->getValueNode(&value));
  };
  // SVF prepends extapi to the module list. There is exactly one input here.
  const auto &module = modules->getModuleRef(modules->getModuleNum() - 1);
  for (const auto &global : module.globals())
    addValue(global);
  for (const auto &function : module) {
    addValue(function);
    for (const auto &argument : function.args())
      addValue(argument);
    for (const auto &instruction : llvm::instructions(function))
      addValue(instruction);
  }
  const auto total = values.size();
  lotus::alias::flow_sensitive::sampleComparisonValues(
      values, comparisonLimit(), comparisonSeed());
  llvm::json::Array observations, aliases;
  for (auto left = values.begin(); left != values.end(); ++left) {
    observations.push_back(llvm::json::Object{
        {"key", left->first},
        {"points_to_size", static_cast<int64_t>(solver->getPts(left->second).count())}});
    for (auto right = std::next(left); right != values.end(); ++right)
      if (solver->alias(left->second, right->second) != SVF::AliasResult::NoAlias)
        aliases.push_back(llvm::json::Array{left->first, right->first});
  }
  llvm::outs() << "PTA_COMPARISON_JSON "
               << llvm::json::Value(llvm::json::Object{
                      {"schema", 1},
                      {"total_values", static_cast<int64_t>(total)},
                      {"truncated", total != values.size()},
                      {"values", std::move(observations)},
                      {"may_alias", std::move(aliases)}})
               << '\n';
  return 0;
}
