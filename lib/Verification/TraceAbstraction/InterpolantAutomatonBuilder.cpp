#include "Verification/TraceAbstraction/InterpolantAutomatonBuilder.h"

#include "Verification/TraceAbstraction/TransitionFormula.h"

#include <map>
#include <set>
#include <tuple>

namespace lotus::verification::traceabstraction {
namespace {
// Counterpart of Ultimate's on-demand interpolant transition appender.
class InductiveProvider final : public nwa::SuccessorProvider {
  std::shared_ptr<nwa::NestedWordAutomaton> spine_;
  TraceCheckResult proof_;
  TraceChecker &checker_;
  nwa::Context context_;
  std::map<std::tuple<nwa::State, nwa::Letter, nwa::State>,
           std::vector<nwa::State>>
      cache_;
  size_t transitions_ = 0;
  bool conservative_ = false;
  std::map<nwa::Letter, std::set<nwa::State>> candidates_;

public:
  InductiveProvider(std::shared_ptr<nwa::NestedWordAutomaton> spine,
                    const TraceCheckResult &proof, TraceChecker &checker,
                    nwa::Context context)
      : spine_(std::move(spine)), proof_(proof), checker_(checker),
        context_(context) {
    const auto &inputs = checker_.semantics().inputs();
    for (unsigned j = 0; j < inputs.size(); ++j)
      conservative_ |= inputs[j].is_array();
    for (const auto &edge : spine_->transitions())
      candidates_[edge.letter].insert(edge.target);
  }
  const nwa::Alphabet &alphabet() const override { return spine_->alphabet(); }
  const std::vector<nwa::State> &initialStates() const override {
    return spine_->initialStates();
  }
  size_t stateCount() const override { return spine_->stateCount(); }
  bool isFinal(nwa::State q) const override { return spine_->isFinal(q); }
  const std::vector<nwa::State> &
  successors(nwa::State q, nwa::Letter letter,
             nwa::State hierarchy = nwa::Bottom) override {
    auto key = std::make_tuple(q, letter, hierarchy);
    auto found = cache_.find(key);
    if (found != cache_.end())
      return found->second;
    std::vector<nwa::State> successors;
    auto kind = alphabet().kind(letter);
    if (kind != nwa::LetterKind::Return || hierarchy != nwa::Bottom) {
      for (nwa::State target = 0; target < stateCount(); ++target) {
        // Ultimate's conservative successor selection: retain the spine,
        // try self-loops and true/false, and reuse targets of this statement.
        if (conservative_ && target != q && target != 0 && !isFinal(target) &&
            !candidates_[letter].count(target))
          continue;
        context_->check();
        bool valid =
            kind == nwa::LetterKind::Return
                ? checker_.isReturnInductive(proof_.interpolants.at(q),
                                             proof_.interpolants.at(hierarchy),
                                             letter,
                                             proof_.interpolants[target])
                : checker_.isInductive(proof_.interpolants.at(q), letter,
                                       proof_.interpolants[target]);
        if (valid) {
          if (++transitions_ > context_->maxTransitions)
            throw nwa::ResourceLimit();
          successors.push_back(target);
        }
      }
    }
    return cache_.emplace(key, std::move(successors)).first->second;
  }
};
} // namespace

nwa::Provider InterpolantAutomatonBuilder::build(
    const ProgramAutomaton &program, const nwa::NestedRun &run,
    const TraceCheckResult &proof, TraceChecker &checker, bool generalize,
    nwa::Context context) {
  auto nesting = run;
  nesting.computeMatching(program.automaton->alphabet());
  if (nesting.matching != run.matching ||
      proof.status != TraceStatus::Infeasible ||
      proof.interpolants.size() != run.word.size() + 1 ||
      !checker.isTrue(proof.interpolants.front()) ||
      !checker.isFalse(proof.interpolants.back()))
    return nullptr;
  auto spine =
      std::make_shared<nwa::NestedWordAutomaton>(program.automaton->alphabet());
  if (proof.interpolants.size() > context->maxStates)
    throw nwa::ResourceLimit();
  for (unsigned i = 0; i < proof.interpolants.size(); ++i)
    spine->addState(i == 0, i == run.word.size());
  for (unsigned i = 0; i < run.word.size(); ++i) {
    context->check();
    nwa::State hierarchy = nwa::Bottom;
    bool valid;
    if (spine->alphabet().kind(run.word[i]) == nwa::LetterKind::Return) {
      if (run.matching[i] < 0)
        return nullptr;
      hierarchy = run.matching[i];
      valid = checker.isReturnInductive(proof.interpolants[i],
                                        proof.interpolants.at(hierarchy),
                                        run.word[i], proof.interpolants[i + 1]);
    } else
      valid = checker.isInductive(proof.interpolants[i], run.word[i],
                                  proof.interpolants[i + 1]);
    if (!valid)
      return nullptr;
    spine->addTransition(i, run.word[i], i + 1, hierarchy);
  }
  nwa::Provider result = spine;
  if (generalize)
    result =
        std::make_shared<InductiveProvider>(spine, proof, checker, context);
  if (!nwa::accepts(result, run.word, nwa::WordSemantics::Open, context))
    return nullptr;
  return result;
}
} // namespace lotus::verification::traceabstraction
