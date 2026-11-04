#include "Checker/GSAF/API/Trace.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"

namespace lotus::gsaf {
using namespace llvm;
using namespace lotus::gvfg;

VulnerabilityTrace::VulnerabilityTrace() = default;

VulnerabilityTrace::VulnerabilityTrace(const VulnerabilityTrace &T)
    : GuardedValueFlowTrace(T), Reported(T.Reported) {}

VulnerabilityTrace::~VulnerabilityTrace() = default;

VulnerabilityTrace &VulnerabilityTrace::operator=(const VulnerabilityTrace &T) {
  if (this != &T) {
    GuardedValueFlowTrace::operator=(T);
    Reported = T.Reported;
  }
  return *this;
}

VulnerabilityTrace::VulnerabilityTrace(const GuardedValueFlowObject *Obj)
    : GuardedValueFlowTrace(Obj) {}

VulnerabilityTrace::VulnerabilityTrace(const GuardedValueFlowObject *Obj1,
                                       const GuardedValueFlowObject *Obj2)
    : GuardedValueFlowTrace(Obj1, Obj2) {}

VulnerabilityTrace::VulnerabilityTrace(
    const std::vector<const GuardedValueFlowObject *> &T)
    : GuardedValueFlowTrace(T) {}

llvm::raw_ostream &operator<<(llvm::raw_ostream &Out,
                              const VulnerabilityTrace &T) {
  for (size_t I = 0, E = T.get_length(); I < E; I++) {
    if (auto *Obj = T[I])
      Out << "[" << I << "] " << *Obj << " ("
          << Obj->getGraph()->getBaseFunction()->getName() << ")\n";
    else
      Out << "[" << I << "] nullptr\n";
  }
  return Out;
}

VulnerabilityTraceBuilder::VulnerabilityTraceBuilder() = default;

VulnerabilityTraceBuilder::VulnerabilityTraceBuilder(
    const VulnerabilityTraceBuilder &) = default;

VulnerabilityTraceBuilder::~VulnerabilityTraceBuilder() = default;

void VulnerabilityTraceBuilder::add(std::shared_ptr<VulnerabilityTrace> Trace) {
  for (int I = 0; I < Trace->get_length(); I++) {
    add(Trace->at(I));
  }
}

void VulnerabilityTraceBuilder::add(const GuardedValueFlowObject *O) {
  PushPopVector<const GuardedValueFlowObject *>::add(O);
}

std::shared_ptr<VulnerabilityTrace>
VulnerabilityTraceBuilder::snapshot() const {
  auto Ret = std::make_shared<VulnerabilityTrace>();
  for (const GuardedValueFlowObject *Step : getCacheVector()) {
    Ret->push(Step);
  }
  return Ret;
}
} // namespace lotus::gsaf
