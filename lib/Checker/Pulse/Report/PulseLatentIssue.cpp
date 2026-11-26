#include "Checker/Pulse/Report/PulseLatentIssue.h"

#include "Checker/Pulse/Interproc/PulseSummary.h"
#include "Checker/Pulse/Report/PulseOptions.h"

#include <llvm/IR/Argument.h>

namespace pulse {

bool LatentIssue::shouldReport(const PulseSummary &summary,
                               const LatentIssue & /*issue*/) {
  if (options::reportLatentIssues()) {
    return true;
  }
  const AbductiveDomain *post = summary.getPost();
  if (!post) {
    return false;
  }
  return isManifest(*post);
}

LatentIssue::IssueKind
LatentIssue::issueKindFromResult(OperationResult result) {
  switch (result) {
  case OperationResult::InvalidAccess:
    return IssueKind::InvalidAccess;
  case OperationResult::OutOfBounds:
    return IssueKind::OutOfBounds;
  case OperationResult::UseAfterFree:
    return IssueKind::UseAfterFree;
  case OperationResult::NullDereference:
    return IssueKind::NullDereference;
  case OperationResult::UninitializedRead:
    return IssueKind::UninitializedRead;
  case OperationResult::TaintError:
  case OperationResult::Success:
    return IssueKind::InvalidAccess;
  }
  return IssueKind::InvalidAccess;
}

bool LatentIssue::isManifest(const AbductiveDomain &astate) {
  if (options::reportLatentIssues()) {
    return true;
  }
  if (astate.hasUnknownValues() || astate.preHeapHasAssumptions()) {
    return false;
  }
  auto is_allocated = [&astate](AbstractValue v) {
    return astate.isAllocated(v);
  };
  return astate.getPathFormula().isManifest(is_allocated,
                                            /*check_depth_zero=*/true);
}

bool LatentIssue::isManifest(OperationResult diagnostic,
                             const AbductiveDomain &astate,
                             AbstractValue address) {
  if (options::reportLatentIssues()) {
    return true;
  }
  auto is_allocated = [&astate](AbstractValue v) {
    return astate.isAllocated(v);
  };

  if (diagnostic == OperationResult::OutOfBounds) {
    return !astate.preHeapHasAssumptions() &&
           astate.getPathFormula().isManifest(is_allocated,
                                              /*check_depth_zero=*/false);
  }

  if (diagnostic == OperationResult::UninitializedRead) {
    const llvm::Value *v = address.getValue();
    if (v && llvm::isa<llvm::Argument>(v)) {
      return false;
    }
    return !astate.preHeapHasAssumptions() &&
           astate.getPathFormula().isManifest(is_allocated,
                                              /*check_depth_zero=*/true);
  }

  if (astate.preHeapHasAssumptions()) {
    return false;
  }
  return astate.getPathFormula().isManifest(is_allocated,
                                            /*check_depth_zero=*/true);
}

} // namespace pulse
