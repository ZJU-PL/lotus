#include "Verification/Automata/NwaReduction.h"

#include <iostream>

using namespace lotus::verification::automata;

namespace {
std::shared_ptr<NestedWordAutomaton> read() {
  Alphabet alphabet;
  alphabet.intern("a", LetterKind::Call);
  alphabet.intern("b", LetterKind::Return);
  alphabet.intern("x", LetterKind::Internal);
  auto a = std::make_shared<NestedWordAutomaton>(alphabet);
  std::size_t states, transitions;
  std::cin >> states >> transitions;
  for (std::size_t q = 0; q < states; ++q) {
    bool initial, final;
    std::cin >> initial >> final;
    a->addState(initial, final);
  }
  for (std::size_t i = 0; i < transitions; ++i) {
    State source, target;
    Letter letter;
    std::int64_t hierarchy;
    std::cin >> source >> letter >> target >> hierarchy;
    a->addTransition(source, letter, target,
                     hierarchy < 0 ? Bottom : static_cast<State>(hierarchy));
  }
  return a;
}

std::vector<Letter> readWord() {
  std::size_t size;
  std::cin >> size;
  std::vector<Letter> result(size);
  for (auto &letter : result)
    std::cin >> letter;
  return result;
}

bool periodic(Provider a, const std::vector<Letter> &stem,
              const std::vector<Letter> &loop) {
  if (loop.empty())
    throw std::invalid_argument("empty omega loop");
  auto monitor = std::make_shared<NestedWordAutomaton>(a->alphabet());
  const auto count = stem.size() + loop.size();
  for (State q = 0; q < count; ++q)
    monitor->addState(q == 0, q >= stem.size());
  for (State q = 0; q < count; ++q) {
    auto letter = q < stem.size() ? stem[q] : loop[q - stem.size()];
    State target = q + 1 < count ? q + 1 : static_cast<State>(stem.size());
    monitor->addTransition(q, letter, target);
    if (a->alphabet().kind(letter) == LetterKind::Return)
      for (State h = 0; h < count; ++h)
        monitor->addTransition(q, letter, target, h);
  }
  auto product = intersection(a, monitor);
  auto result = buchiIsEmpty(product);
  if (!result.empty() && !validateLasso(*product, *result.lasso))
    throw std::logic_error("invalid Buchi lasso certificate");
  return !result.empty();
}
} // namespace

int main() {
  try {
    std::cin.exceptions(std::ios::failbit | std::ios::badbit);
    std::size_t cases;
    std::cin >> cases;
    for (std::size_t example = 0; example < cases; ++example) {
      auto a = read(), b = read();
      auto da = std::make_shared<PowersetDeterminizer>(a);
      auto db = std::make_shared<PowersetDeterminizer>(b);
      auto diff = difference(a, b);
      auto comp = complement(a);
      auto product = intersection(a, b);
      DelayedDeadEndRemoval prune(a);
      prune.stage();
      auto pruned = prune.commit();
      auto direct = reduceDirectSimulation(*a);
      auto delayed = reduceDelayedSimulation(*a, AcceptanceSemantics::Buchi);
      auto gameDirect = reduceSimulationGame(*a, AcceptanceSemantics::Finite);
      auto gameDelayed = reduceSimulationGame(*a, AcceptanceSemantics::Buchi);
      std::cout << "case " << example << ' ' << isEmpty(a).empty() << ' '
                << isEmpty(b).empty() << ' ' << isEmpty(diff).empty() << ' '
                << buchiIsEmpty(a).empty() << ' ' << buchiIsEmpty(b).empty()
                << '\n';
      const Provider operands[] = {a,    b,       da,     db,     diff,
                                   comp, product, pruned, direct, gameDirect};
      std::size_t queries;
      std::cin >> queries;
      std::vector<Letter> word;
      for (std::size_t i = 0; i < queries; ++i) {
        std::size_t length;
        std::cin >> length;
        word.resize(length);
        for (auto &letter : word)
          std::cin >> letter;
        for (const auto &operand : operands)
          std::cout << accepts(operand, word);
        std::cout << '\n';
      }
      std::size_t omegaQueries;
      std::cin >> omegaQueries;
      for (std::size_t i = 0; i < omegaQueries; ++i) {
        const auto stem = readWord();
        const auto loop = readWord();
        std::cout << "omega " << periodic(a, stem, loop)
                  << periodic(direct, stem, loop)
                  << periodic(delayed, stem, loop)
                  << periodic(gameDirect, stem, loop)
                  << periodic(gameDelayed, stem, loop) << '\n';
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "NWA oracle failed: " << e.what() << '\n';
    return 1;
  }
}
