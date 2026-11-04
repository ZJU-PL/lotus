#pragma once

#include "Checker/Framework/LLVMValueTrace.h"
#include "IR/GVFG/GuardedValueFlowGraph.h"

namespace lotus {
namespace gvfg {
using llvm::dyn_cast;
using llvm::isa;
using llvm::raw_ostream;
using lotus::trace::LLVMValueTrace;
using lotus::trace::Trace;
using lotus::trace::TraceType;

class GuardedValueFlowTrace : public Trace<const GuardedValueFlowObject *> {
public:
  typedef uint64_t StepType;
  inline static constexpr StepType STY_VALUE = 0x1;
  inline static constexpr StepType STY_USE_SITE = 0x2;

  const StepType DEFAULT_STEP_TYPE = STY_VALUE | STY_USE_SITE;

private:
  std::vector<StepType> STys;

  // Additional conditions such as for NPD trace, we have ptr==null, and some
  // path choices are also modelled here
  std::vector<GuardedValueFlowRegionNode *> AdditionalConds;

protected:
  // Guess the source node index of the last effect value flow of the bug trace
  // if exist The sink of the last effect value flow is usually the key index
  // This can be used to override "getLEVFSrcIdx" function
  //
  // In Basic IR, a value can be accessed from the value cached in register or
  // by a load instruction accessing the main memory By setting isConsiderLoad =
  // true, we consider load instruction as a source of the last effective value
  // flow, or otherwise, we go past the load instruction and directly find the
  // store instruction
  int guessLEVFSrcIdx(bool IsConsiderLoad) const;

public:
  GuardedValueFlowTrace();

  GuardedValueFlowTrace(const GuardedValueFlowTrace &T);

  GuardedValueFlowTrace(
      const std::vector<const GuardedValueFlowObject *> &Trace);

  /// Create a trace that only contains \p Obj
  GuardedValueFlowTrace(const GuardedValueFlowObject *Obj);

  GuardedValueFlowTrace(const GuardedValueFlowObject *Obj1,
                        const GuardedValueFlowObject *Obj2);

  // Construct GVFG trace from Value Trace
  GuardedValueFlowTrace(const LLVMValueTrace *ValueTrace,
                        GuardedValueFlowGraphBuilderPass *GVFGs);

  GuardedValueFlowTrace &operator=(const GuardedValueFlowTrace &T);

  // Reset the trace with LLVM Value Trace
  virtual void resetWithLLVMValueTrace(const LLVMValueTrace *ValueTrace,
                                       GuardedValueFlowGraphBuilderPass *GVFGs);

  virtual ~GuardedValueFlowTrace();

  // Get the real start idx of the trace that matters
  // For some bug types, the first several steps shall be some preparation nodes
  // Usually, returns 0, needing override when a class of trace has special
  // needs that the first few steps are not considered
  virtual int getValidStartIdx() const;

  // Get the key step index in the trace where vulnerability occurs
  // SOURCE_SINK/SIMPLE_VALUE_FLOW/NON_VALUE_FLOW : return the last step
  // SOURCE_NO_SINK : return the first step indicating the source
  virtual int getKeyIdx() const;

  // Get the source node index of the last effect value flow of the bug trace if
  // exist The sink of the last effect value flow is usually the key index
  //
  // By default, this function returns as follows :
  // SOURCE_SINK/SIMPLE_VALUE_FLOW :
  //      guessLEVFSrcIdx(isConsiderLoad), which tries to guess the last value
  //      flow to the key step returned by getKeyIdx()
  // SOURCE_NO_SINK/NON_VALUE_FLOW
  //      the last step of the trace, meaning that a bug report does not have
  //      such last effect value flow
  //
  // Please extend GuardedValueFlowTrace by overriding this function to support
  // special last effect value flow source computation for e.g. domination
  // checking guessLEVFSrcIdx() can be used to guess the last effect value flow
  // index for common traces like NPD Trace, which can be used for the
  // overriding of this function
  //
  // In Basic IR, a value can be accessed from the value cached in register or
  // by a load instruction accessing the main memory By setting isConsiderLoad =
  // true, we consider load instruction as a source of the last effective value
  // flow, or otherwise, we go past the load instruction and directly find the
  // store instruction
  virtual int getLEVFSrcIdx(bool isConsiderLoad = false) const;

  // get the trace step type for the given StepIdx
  StepType getStepType(int StepIdx) const;

  // get the trace step type for the given StepIdx to STy
  void setStepType(int StepIdx, StepType STy);

  // Get the number of additional conditions for the trace
  int getNumAdditionalConds() const;

  // Get the additional condition with index idx ranges in [0,
  // getNumAdditionalConds)
  GuardedValueFlowRegionNode *getAdditionalCond(int idx) const;

  // Add an additional condition modelled using GVFG region node
  void addAdditionalCond(GuardedValueFlowRegionNode *cond);

  // Clear all the additional conditions
  void clearAdditionalCond();

  /// Searching the trace from the \p StartIndex until
  /// the predicate \p P is satisfied. Return the found
  /// GuardedValueFlowObject and its index.
  template <typename Predicate>
  std::pair<const GuardedValueFlowObject *, int> find(int StartIndex,
                                                      Predicate P) const {
    for (int I = StartIndex, E = get_length(); I < E; ++I) {
      auto *O = at(I);
      if (P(O)) {
        return std::make_pair(O, I);
      }
    }

    return std::make_pair(nullptr, get_length());
  }

  virtual void print(raw_ostream &O) override;
};

} // namespace gvfg
} // namespace lotus
