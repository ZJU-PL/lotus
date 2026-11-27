/// \file Function.h
/// \brief FiTx per-function analysis state: per-block info, return-code
/// mapping, value collection, and alias (paper §4.2, 4.3).
#pragma once
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Frontend/BasicBlock.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

#define NO_ERROR 0

namespace fitx {

class FunctionInformation {
public:
  using WeakBasicBlockSet =
      std::set<std::weak_ptr<fitx::BasicBlock>,
               std::owner_less<std::weak_ptr<fitx::BasicBlock>>>;

  enum AnalysisStat { UNANALYZED, IN_PROGRESS, DIRTY, ANALYZED };
  static constexpr int kErrorCode = -1;
  static constexpr int kSuccessCode = 0;

  static constexpr int kMaxPredCheckDepth = 4;

  FunctionInformation(std::shared_ptr<fitx::Function> function,
                      AnalysisStat stat = AnalysisStat::UNANALYZED);
  void setAnalysisStat(AnalysisStat stat);

  void setAnayzingBasicBlock(std::shared_ptr<fitx::BasicBlock> basic_block);

  std::shared_ptr<fitx::BasicBlock> currentBasicBlock() {
    return current_basicblock_;
  }

  std::shared_ptr<BasicBlockInformation> getCurrentBasicBlockInformation();

  std::shared_ptr<BasicBlockInformation>
  getBasicBlockInformation(std::shared_ptr<fitx::BasicBlock> basic_block);

  std::shared_ptr<BasicBlockInformation> getBasicBlockPrevInformation(
      std::shared_ptr<fitx::BasicBlock> basic_block);

  std::shared_ptr<BasicBlockInformation>
  createBasicBlockInfo(std::shared_ptr<fitx::BasicBlock> basic_block,
                       const std::set<State> &states);

  void addValue(std::shared_ptr<fitx::Value> value);
  void addValues(const ValueCollection &value);
  const ValueCollection &GetValueCollection() { return value_collection_; }

  bool basicBlockInfoExists(std::shared_ptr<fitx::BasicBlock> basic_block);
  bool basicBlockPrevInfoExists(std::shared_ptr<fitx::BasicBlock> basic_block);

  AnalysisStat Stat() { return stat_; }

  bool basicBlockInfoChanged(std::shared_ptr<fitx::BasicBlock> block);

  const WeakBasicBlockSet &getErrorBlocks(int64_t error_code);
  const WeakBasicBlockSet &getSuccessBlock();
  void addReturnValueInfo(int64_t value,
                          std::weak_ptr<fitx::BasicBlock> block_info);
  void addReturnValueInfo(int64_t value, WeakBasicBlockSet block_info);

  const std::map<int64_t, WeakBasicBlockSet> &getReturnValueInfo() {
    return return_info_;
  }

  std::shared_ptr<fitx::Function> Function() { return framework_function_; }

  AliasValues &getAliasValues() { return alias_info_; }

  bool existsInRefcountFunctions(std::shared_ptr<fitx::Function> function);
  void addRefcountFunction(std::shared_ptr<fitx::Function> function);

  /// Release per-block intra-procedural state once function analysis completes.
  /// When `keep_summary` is true, keeps only the `arg_value_states_` on blocks
  /// referenced in `return_info_` (plus `value_collection_`) for caller
  /// summaries; otherwise releases all per-function state.
  void releaseAnalysisMemory(bool keep_summary);

private:
  std::shared_ptr<fitx::Function> framework_function_;
  AnalysisStat stat_;

  ValueCollection value_collection_;
  AliasValues alias_info_;

  std::shared_ptr<fitx::BasicBlock> current_basicblock_;
  std::shared_ptr<fitx::BasicBlock> return_block_;

  std::vector<std::shared_ptr<fitx::Function>> called_refcount_functions_;

  std::map<std::shared_ptr<fitx::BasicBlock>,
           std::shared_ptr<BasicBlockInformation>>
      basic_block_info_;

  std::map<std::shared_ptr<fitx::BasicBlock>,
           std::shared_ptr<BasicBlockInformation>>
      prev_basic_block_info_;

  std::map<int64_t, WeakBasicBlockSet> return_info_;
};

}; // namespace fitx
