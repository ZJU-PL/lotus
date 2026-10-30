#include "Verification/TraceAbstraction/TraceAbstractionEngine.h"

#include "Verification/Automata/NwaReduction.h"
#include "Verification/TraceAbstraction/InterpolantAutomatonBuilder.h"

namespace lotus::verification::traceabstraction {
namespace {
std::shared_ptr<nwa::OperationContext>
makeContext(const TraceAbstractionOptions &options) {
  auto context = std::make_shared<nwa::OperationContext>();
  context->deadline = std::chrono::steady_clock::now() + options.timeout;
  return context;
}
VerificationResult unknown(const std::string &reason) {
  VerificationResult result;
  result.reason = reason;
  return result;
}
} // namespace

TraceAbstractionEngine::TraceAbstractionEngine(TraceAbstractionOptions options)
    : options_(options) {}

VerificationResult
TraceAbstractionEngine::verify(const llvm::Function &function,
                               const llvm::Instruction *targetError) {
  try {
    auto context = makeContext(options_);
    context->check();
    return run(IcfgToNwa::build(function, targetError, context), context);
  } catch (const std::exception &e) {
    return unknown(e.what());
  }
}

VerificationResult TraceAbstractionEngine::verify(const ICFG &icfg,
                                                  const ICFGNode *errorNode) {
  try {
    auto context = makeContext(options_);
    context->check();
    return run(IcfgToNwa::build(icfg, errorNode, context), context);
  } catch (const std::exception &e) {
    return unknown(e.what());
  }
}

VerificationResult TraceAbstractionEngine::run(const ProgramAutomaton &program,
                                               nwa::Context context) {
  VerificationResult result;
  try {
    nwa::Provider abstraction = program.automaton;
    TraceChecker checker(program, context);
    while (true) {
      context->check();
      auto search = nwa::isEmpty(abstraction, {}, context);
      if (search.empty()) {
        result.status = VerificationResult::Safe;
        return result;
      }
      if (result.iterations >= options_.maxIterations) {
        result.reason = "CEGAR iteration limit reached";
        return result;
      }
      ++result.iterations;
      const auto &trace = *search.run;
      auto checked = checker.check(trace);
      if (checked.status == TraceStatus::Unknown) {
        result.reason = checked.reason;
        return result;
      }
      if (checked.status == TraceStatus::Feasible) {
        result.status = VerificationResult::Unsafe;
        // Keep synthetic steps so positions in matching remain meaningful.
        for (auto letter : trace.word)
          result.counterexample.push_back(
              program.statements.at(letter).instruction);
        result.matching = trace.matching;
        result.assignments = std::move(checked.assignments);
        return result;
      }
      auto interpolant = InterpolantAutomatonBuilder::build(
          program, trace, checked, checker,
          options_.enableInductiveGeneralization, context);
      if (!interpolant) {
        result.reason = "interpolant automaton certificate validation failed";
        return result;
      }
      abstraction = nwa::difference(abstraction, interpolant, {}, context);
      // Pruning consumes an explicit NWA. Materialize only the reachable
      // portion of the lazy difference using the existing saturation API.
      auto materialized = nwa::reachable(abstraction, context).automaton;
      nwa::DelayedDeadEndRemoval pruner(materialized, context);
      pruner.stage();
      abstraction = pruner.commit();
    }
  } catch (const std::exception &e) {
    result.reason = e.what();
    return result;
  }
}
} // namespace lotus::verification::traceabstraction
