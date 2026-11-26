#include "Checker/Pulse/Checker/PulseChecker.h"

#include <llvm/IR/Constants.h>

namespace pulse {
ToplValue PulseChecker::toplValue(AbductiveDomain &state,
                                  const llvm::Value *value,
                                  const llvm::Instruction *location,
                                  const llvm::BasicBlock *pred) {
  ToplValue result;
  if (!value || value->getType()->isVoidTy())
    return result;
  if (auto *n = llvm::dyn_cast<llvm::ConstantInt>(value)) {
    if (n->getBitWidth() <= 64)
      result.constant = n->getSExtValue();
  } else if (llvm::isa<llvm::ConstantPointerNull>(value)) {
    result.constant = 0;
  }
  if (auto address = ops_.eval(state, value, location, pred))
    result.value = state.getCanonical(address->addr);
  return result;
}

void PulseChecker::recordToplEvent(AbductiveDomain &state, ToplEvent event) {
  state.getToplHistory().append(std::move(event));
}

void PulseChecker::reportTopl(const AbductiveDomain &state) {
  if (topl_program_.empty() || state.getPathFormula().isUnsat())
    return;
  if (toplTypeId_ < 0)
    toplTypeId_ = BugReportMgr::get_instance().register_bug_type(
        "TOPL Error", BugDescription::BI_HIGH, BugDescription::BC_ERROR);
  for (const auto &violation : topl_program_.evaluate(
           state.getToplHistory(), state.getPathFormula(),
           &state.getPostHeap())) {
    if (violation.trace.empty())
      continue;
    const auto *location = violation.trace.back().location;
    if (!topl_reported_.emplace(violation.property, location).second)
      continue;
    auto *report = new BugReport(toplTypeId_);
    report->set_conf_score(90);
    report->add_metadata("checker", "TOPL");
    report->add_metadata("property", violation.property);
    for (const auto &event : violation.trace) {
      int depth = 0;
      for (const auto *call : event.callingContext)
        report->append_step(const_cast<llvm::Instruction *>(call),
                            "Call leading to temporal event", depth++);
      report->append_step(const_cast<llvm::Instruction *>(event.location),
                          violation.property + ": " + event.name, depth);
    }
    report->append_step(const_cast<llvm::Instruction *>(location),
                        violation.property + ": " + violation.message);
    BugReportMgr::get_instance().insert_report(toplTypeId_, report, true);
  }
}
} // namespace pulse
