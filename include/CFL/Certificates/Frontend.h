#pragma once
// Unified front end for arbitrary CFGs and arbitrary finite 2NPDAs.
// The generic CFG is normalized to binary form before using the certificate
// algorithms. A 2NPDA on a fixed word is product-compiled to an input-free PDA;
// arbitrary-length stack replacements are normalized to pushes of length <=2.
#include "CFL/Certificates/CFGCertificates.h"
#include "CFL/Certificates/TwoNPDA.h"
  
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cflcert::frontend {

// -------------------------- General CFG front end -------------------------
struct CFGNormalizationReport {
  cfg::Grammar grammar;
  size_t originalProductions = 0;
  size_t normalizedProductions = 0;
  int originalNonterminals = 0;
  int normalizedNonterminals = 0;
};

inline CFGNormalizationReport normalizeCFG(const cfg::GeneralGrammar &input) {
  input.validate();
  CFGNormalizationReport r;
  r.originalProductions = input.productions.size();
  r.originalNonterminals = input.nonterminals;
  r.grammar = cfg::normalize(input);
  r.normalizedProductions = r.grammar.productions.size();
  r.normalizedNonterminals = r.grammar.nonterminals;
  return r;
}

enum class ReachabilityStatus { Reachable, Unreachable };
struct CFGReachabilityResult {
  cfg::Grammar normalizedGrammar;
  ReachabilityStatus status = ReachabilityStatus::Unreachable;
  cfg::PositiveCertificate positive;
  cfg::NegativeCertificate negative;
};

// Solves one arbitrary-CFG, one-source/one-target CFL-reachability query.
// A positive result carries a straight-line triplet certificate; a negative
// result carries the generic CFG matrix separator certificate.
inline CFGReachabilityResult solveCFG(const cfg::GeneralGrammar &grammar,
                                      const cfg::Graph &graph) {
  CFGReachabilityResult result;
  result.normalizedGrammar = cfg::normalize(grammar);
  if (cfg::buildPositive(result.normalizedGrammar, graph, result.positive)) {
    if (!cfg::verifyPositive(result.normalizedGrammar, graph, result.positive))
      throw std::logic_error("internally constructed CFG positive certificate "
                             "failed verification");
    result.status = ReachabilityStatus::Reachable;
  } else {
    cfg::buildNegative(result.normalizedGrammar, graph, result.negative);
    if (!cfg::verifyNegative(result.normalizedGrammar, graph, result.negative))
      throw std::logic_error("internally constructed CFG negative certificate "
                             "failed verification");
    result.status = ReachabilityStatus::Unreachable;
  }
  return result;
}

inline bool verifyCFGResult(const cfg::GeneralGrammar &original,
                            const cfg::Graph &graph,
                            const CFGReachabilityResult &result) {
  const cfg::Grammar normalized = cfg::normalize(original);
  if (normalized.nonterminals != result.normalizedGrammar.nonterminals ||
      normalized.terminals != result.normalizedGrammar.terminals ||
      normalized.start != result.normalizedGrammar.start ||
      normalized.productions.size() !=
          result.normalizedGrammar.productions.size())
    return false;
  for (size_t i = 0; i < normalized.productions.size(); ++i) {
    const auto &a = normalized.productions[i];
    const auto &b = result.normalizedGrammar.productions[i];
    if (a.kind != b.kind || a.lhs != b.lhs || a.a != b.a || a.b != b.b ||
        a.terminal != b.terminal)
      return false;
  }
  if (result.status == ReachabilityStatus::Reachable)
    return cfg::verifyPositive(normalized, graph, result.positive);
  return cfg::verifyNegative(normalized, graph, result.negative);
}

// ------------------------- Arbitrary 2NPDA front end ----------------------
enum class AcceptanceMode {
  // Conventional final-state acceptance, with the input head on the right
  // endmarker. The stack may contain arbitrary symbols above its bottom.
  RightEndmarker,
  // Conventional final-state acceptance at any input position.
  AnyPosition
};

// A bounded-push equivalent of an input-free PDA. Replacement words use the
// repository convention: top-first (leftmost symbol becomes the new top).
inline pda::EmptinessPDA normalizePushLength(const pda::EmptinessPDA &input) {
  if (input.states <= 0 || input.stackAlphabet <= 0 || input.initialState < 0 ||
      input.initialState >= input.states || input.initialSymbol < 0 ||
      input.initialSymbol >= input.stackAlphabet)
    throw std::invalid_argument("invalid PDA emptiness machine");
  for (int f : input.acceptingStates)
    if (f < 0 || f >= input.states)
      throw std::invalid_argument("invalid PDA accepting state");
  for (const auto &t : input.transitions) {
    if (t.fromState < 0 || t.fromState >= input.states || t.toState < 0 ||
        t.toState >= input.states || t.popSymbol < 0 ||
        t.popSymbol >= input.stackAlphabet)
      throw std::invalid_argument("invalid PDA transition state/symbol");
    for (int x : t.pushWord)
      if (x < 0 || x >= input.stackAlphabet)
        throw std::invalid_argument("invalid PDA pushed symbol");
  }

  pda::EmptinessPDA out;
  out.states = input.states;
  out.stackAlphabet = input.stackAlphabet;
  out.initialState = input.initialState;
  out.initialSymbol = input.initialSymbol;
  out.acceptingStates = input.acceptingStates;
  for (const auto &t : input.transitions) {
    const size_t k = t.pushWord.size();
    if (k <= 2) {
      out.transitions.push_back(t);
      continue;
    }
    if (k - 1 >
        static_cast<size_t>(std::numeric_limits<int>::max() - out.states))
      throw std::length_error("PDA push normalization state overflow");
    // Build bottom-up. First put the bottommost replacement symbol on the
    // stack, then successively put the next symbol above it. This preserves
    // the exact top-first replacement word without accessing buried symbols.
    std::vector<int> mids(k - 1);
    for (size_t i = 0; i < k - 1; ++i)
      mids[i] = out.states++;
    out.transitions.push_back(
        {t.fromState, t.popSymbol, mids[0], {t.pushWord[k - 1]}});
    for (size_t step = 0; step < k - 1; ++step) {
      const size_t index = k - 2 - step;
      const int from = mids[step];
      const int to = (index == 0 ? t.toState : mids[step + 1]);
      const int currentTop = t.pushWord[index + 1];
      out.transitions.push_back(
          {from, currentTop, to, {t.pushWord[index], t.pushWord[index + 1]}});
    }
  }
  return out;
}

struct Compiled2NPDA {
  pda::EmptinessPDA
      emptiness; // accepts by reaching final state with bottom only
  pda::PDAInstance
      certificateInstance; // target P-automaton accepts exactly bottom
  int productStates = 0;
  int cleanupState = -1;
  int acceptingState = -1;
};

// Compile an arbitrary finite 2NPDA and fixed input word. The input head starts
// on LEFT; transitions may move -1/0/+1 but cannot leave the endmarker-bounded
// tape. Final-state acceptance is converted to bottom-only acceptance by a
// nondeterministic cleanup phase. Arbitrary-length push words are then
// normalized to length at most two, as required by Section 5's triplet CFG.
inline Compiled2NPDA
compile2NPDA(const twopda::Machine &machine, const std::vector<int> &word,
             AcceptanceMode mode = AcceptanceMode::RightEndmarker) {
  machine.validate();
  if (word.size() > static_cast<size_t>(std::numeric_limits<int>::max() - 2))
    throw std::length_error("2NPDA input too long");
  for (int x : word)
    if (x < 0 || x >= machine.alphabet || x == machine.leftMarker ||
        x == machine.rightMarker)
      throw std::invalid_argument(
          "input contains an invalid symbol or an endmarker");

  const int m = static_cast<int>(word.size());
  const int Q = machine.states;
  const int positions = m + 2;
  const long long product64 = static_cast<long long>(positions) * Q;
  if (product64 > std::numeric_limits<int>::max() - 2)
    throw std::length_error("2NPDA/input product state overflow");
  const int productStates = static_cast<int>(product64);
  const int cleanup = productStates;
  const int accept = productStates + 1;
  std::vector<int> tape;
  tape.reserve(static_cast<size_t>(positions));
  tape.push_back(machine.leftMarker);
  tape.insert(tape.end(), word.begin(), word.end());
  tape.push_back(machine.rightMarker);
  auto state = [&](int pos, int q) { return pos * Q + q; };

  pda::EmptinessPDA raw;
  raw.states = productStates + 2;
  raw.stackAlphabet = machine.stackAlphabet;
  raw.initialState = state(0, machine.initial);
  raw.initialSymbol = machine.bottom;
  raw.acceptingStates = {accept};

  for (int pos = 0; pos < positions; ++pos) {
    for (const auto &t : machine.transitions) {
      if (t.read != tape[pos])
        continue;
      const int nextPos = pos + t.move;
      if (nextPos < 0 || nextPos >= positions)
        continue;
      raw.transitions.push_back(
          {state(pos, t.from), t.pop, state(nextPos, t.to), t.push});
    }
  }

  auto isAcceptingPosition = [&](int pos) {
    return mode == AcceptanceMode::AnyPosition || pos == positions - 1;
  };
  for (int pos = 0; pos < positions; ++pos)
    if (isAcceptingPosition(pos)) {
      for (int f : machine.finals) {
        const int qf = state(pos, f);
        // The machine may commit to acceptance at this point. Pop all
        // symbols above bottom; then enter the unique accepting state.
        for (int top = 0; top < machine.stackAlphabet; ++top) {
          if (top == machine.bottom)
            raw.transitions.push_back({qf, top, accept, {machine.bottom}});
          else
            raw.transitions.push_back({qf, top, cleanup, {}});
        }
      }
    }
  for (int top = 0; top < machine.stackAlphabet; ++top) {
    if (top == machine.bottom)
      raw.transitions.push_back({cleanup, top, accept, {machine.bottom}});
    else
      raw.transitions.push_back({cleanup, top, cleanup, {}});
  }

  pda::EmptinessPDA bounded = normalizePushLength(raw);
  if (bounded.states == std::numeric_limits<int>::max())
    throw std::length_error("2NPDA certificate target-state overflow");
  Compiled2NPDA out;
  out.emptiness = bounded;
  out.productStates = productStates;
  out.cleanupState = cleanup;
  out.acceptingState = accept;

  // A P-automaton accepting exactly the one-symbol stack [bottom] from the
  // unique accepting control state. The sink is the sole final state.
  pda::PDAInstance cert;
  cert.controlStates = bounded.states;
  cert.stackAlphabet = bounded.stackAlphabet;
  cert.transitions = bounded.transitions;
  cert.initialState = bounded.initialState;
  cert.initialStackSymbol = bounded.initialSymbol;
  cert.target.states = bounded.states + 1;
  cert.target.edges.assign(static_cast<size_t>(cert.target.states), {});
  const int sink = bounded.states;
  cert.target.finals = {sink};
  cert.target.edges[accept].push_back({machine.bottom, sink});
  cert.targetIsNormalized = false;
  cert.validate();
  out.certificateInstance = std::move(cert);
  return out;
}

inline bool decide2NPDA(const twopda::Machine &machine,
                        const std::vector<int> &word,
                        AcceptanceMode mode = AcceptanceMode::RightEndmarker) {
  const auto compiled = compile2NPDA(machine, word, mode);
  const auto graph = pda::reducePDAEmptinessToDyck2(compiled.emptiness);
  return cflcert::decideCubic(graph);
}

inline bool buildPositive2NPDACertificate(
    const twopda::Machine &machine, const std::vector<int> &word,
    pda::PositiveCertificate &certificate,
    AcceptanceMode mode = AcceptanceMode::RightEndmarker) {
  const auto compiled = compile2NPDA(machine, word, mode);
  return pda::buildPositiveCertificate(compiled.certificateInstance,
                                       certificate);
}

inline void buildNegative2NPDACertificate(
    const twopda::Machine &machine, const std::vector<int> &word,
    pda::NegativeCertificate &certificate,
    AcceptanceMode mode = AcceptanceMode::RightEndmarker) {
  const auto compiled = compile2NPDA(machine, word, mode);
  certificate = pda::buildNegativeCertificate(compiled.certificateInstance);
}

inline bool verifyPositive2NPDACertificate(
    const twopda::Machine &machine, const std::vector<int> &word,
    const pda::PositiveCertificate &certificate,
    AcceptanceMode mode = AcceptanceMode::RightEndmarker) {
  const auto compiled = compile2NPDA(machine, word, mode);
  return pda::verifyPositive(compiled.certificateInstance, certificate);
}

inline bool verifyNegative2NPDACertificate(
    const twopda::Machine &machine, const std::vector<int> &word,
    const pda::NegativeCertificate &certificate,
    AcceptanceMode mode = AcceptanceMode::RightEndmarker) {
  const auto compiled = compile2NPDA(machine, word, mode);
  return pda::verifyNegativeDeterministic(compiled.certificateInstance,
                                          certificate);
}

} // namespace cflcert::frontend
