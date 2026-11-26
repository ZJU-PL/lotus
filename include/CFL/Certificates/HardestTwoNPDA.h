#pragma once
// Explicit finite transition table for the fixed hardest 2NPDA described in
// Claim 32 / Appendix G of Chistikov-Majumdar-Schepper, arXiv:2102.13095.
// The paper gives an operational construction, not a printed state table;
// this header compiles that construction into ordinary 2NPDA transitions.
#include "CFL/Certificates/TwoNPDA.h"

#include <array>
#include <deque>
#include <optional>
#include <ostream>
#include <unordered_set>

namespace cflcert::hardest {

constexpr int LEFT = 256;  // left tape endmarker, not in Sigma_0
constexpr int RIGHT = 257; // right tape endmarker, not in Sigma_0
constexpr int BOTTOM = 0;
constexpr int ROUND = 1;
constexpr int SQUARE = 2;
constexpr int COUNTER = 3;
constexpr int STACK_ALPHABET_SIZE = 4;

enum class State : int {
  VStart,
  VNeedHash,
  VAfterHash,
  VAfterLabel,
  VPosRun,
  VNegNeedOne,
  VNegRun,
  VNeedLabel,
  Rewind,
  MainStart,
  AtVertex,
  CheckFinal,
  SelectEdge,
  SkipOffset,
  SkipNegNeedOne,
  SkipRun,
  ParseR,
  ParseS,
  ParseCR,
  ParseCS,
  PosRunR,
  PosRunS,
  PosRunCR,
  PosRunCS,
  NegNeedR,
  NegNeedS,
  NegNeedCR,
  NegNeedCS,
  NegRunR,
  NegRunS,
  NegRunCR,
  NegRunCS,
  ZeroSeekR,
  ZeroSeekS,
  ZeroSeekCR,
  ZeroSeekCS,
  SeekStartR,
  SeekStartS,
  SeekStartCR,
  SeekStartCS,
  MoveRightR,
  MoveRightS,
  MoveRightCR,
  MoveRightCS,
  AfterPopRightR,
  AfterPopRightS,
  AfterPopRightCR,
  AfterPopRightCS,
  AfterPopRightReflectR,
  AfterPopRightReflectS,
  AfterPopRightReflectCR,
  AfterPopRightReflectCS,
  MoveLeftR,
  MoveLeftS,
  MoveLeftCR,
  MoveLeftCS,
  AfterPopLeftR,
  AfterPopLeftS,
  AfterPopLeftCR,
  AfterPopLeftCS,
  AfterPopLeftReflectR,
  AfterPopLeftReflectS,
  AfterPopLeftReflectCR,
  AfterPopLeftReflectCS,
  ApplyR,
  ApplyS,
  ApplyCR,
  ApplyCS,
  Accept,
  Count
};

struct Rule {
  State from{};
  int read = 0;
  int top = 0;
  State to{};
  // Standard PDA replacement word, top-first. The old top is replaced.
  std::vector<int> replaceTopFirst;
  int move = 0;
};

struct TransitionTable {
  State initial = State::VStart;
  State accepting = State::Accept;
  std::vector<Rule> rules;
  void validate() const {
    for (const auto &r : rules) {
      if (r.from >= State::Count || r.to >= State::Count || r.top < 0 ||
          r.top >= STACK_ALPHABET_SIZE || r.move < -1 || r.move > 1)
        throw std::logic_error("invalid fixed-2NPDA transition");
      for (int z : r.replaceTopFirst)
        if (z < 0 || z >= STACK_ALPHABET_SIZE)
          throw std::logic_error("invalid replacement stack symbol");
      if (r.top == BOTTOM) {
        if (r.replaceTopFirst.empty() || r.replaceTopFirst.back() != BOTTOM ||
            std::count(r.replaceTopFirst.begin(), r.replaceTopFirst.end(),
                       BOTTOM) != 1)
          throw std::logic_error("bottom marker must be retained exactly once");
      } else if (std::find(r.replaceTopFirst.begin(), r.replaceTopFirst.end(),
                           BOTTOM) != r.replaceTopFirst.end()) {
        throw std::logic_error(
            "bottom marker cannot be pushed above the stack");
      }
    }
  }
};

inline TransitionTable makeTransitionTable() {
  TransitionTable T;
  auto add = [&](State q, int a, int z, State p, std::vector<int> repl, int d) {
    T.rules.push_back({q, a, z, p, std::move(repl), d});
  };
  auto preserve = [&](State q, int a, State p, int d) {
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      add(q, a, z, p, {z}, d);
  };
  auto pushCounter = [&](State q, int a, State p) {
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      add(q, a, z, p, {COUNTER, z}, +1);
  };
  auto pushDyck = [&](State q, State p, int sym) {
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      if (z != COUNTER)
        add(q, '#', z, p, {sym, z}, 0);
  };

  // (1) Deterministic syntax-validation sweep. Grammar:
  // # (label offset (* label offset)*) # ... ; offset := 1* | -1* | epsilon.
  add(State::VStart, LEFT, BOTTOM, State::VNeedHash, {BOTTOM}, +1);
  add(State::VNeedHash, '#', BOTTOM, State::VAfterHash, {BOTTOM}, +1);
  for (int a : {'(', ')', '[', ']'})
    add(State::VAfterHash, a, BOTTOM, State::VAfterLabel, {BOTTOM}, +1);
  add(State::VAfterHash, '#', BOTTOM, State::VAfterHash, {BOTTOM},
      +1); // empty block
  add(State::VAfterHash, RIGHT, BOTTOM, State::Rewind, {BOTTOM}, -1);
  add(State::VAfterLabel, '1', BOTTOM, State::VPosRun, {BOTTOM}, +1);
  add(State::VAfterLabel, '-', BOTTOM, State::VNegNeedOne, {BOTTOM}, +1);
  add(State::VAfterLabel, '*', BOTTOM, State::VNeedLabel, {BOTTOM}, +1);
  add(State::VAfterLabel, '#', BOTTOM, State::VAfterHash, {BOTTOM}, +1);
  add(State::VAfterLabel, RIGHT, BOTTOM, State::Rewind, {BOTTOM}, -1);
  add(State::VPosRun, '1', BOTTOM, State::VPosRun, {BOTTOM}, +1);
  add(State::VPosRun, '*', BOTTOM, State::VNeedLabel, {BOTTOM}, +1);
  add(State::VPosRun, '#', BOTTOM, State::VAfterHash, {BOTTOM}, +1);
  add(State::VPosRun, RIGHT, BOTTOM, State::Rewind, {BOTTOM}, -1);
  add(State::VNegNeedOne, '1', BOTTOM, State::VNegRun, {BOTTOM}, +1);
  add(State::VNegRun, '1', BOTTOM, State::VNegRun, {BOTTOM}, +1);
  add(State::VNegRun, '*', BOTTOM, State::VNeedLabel, {BOTTOM}, +1);
  add(State::VNegRun, '#', BOTTOM, State::VAfterHash, {BOTTOM}, +1);
  add(State::VNegRun, RIGHT, BOTTOM, State::Rewind, {BOTTOM}, -1);
  for (int a : {'(', ')', '[', ']'})
    add(State::VNeedLabel, a, BOTTOM, State::VAfterLabel, {BOTTOM}, +1);
  for (int a : {'(', ')', '[', ']', '#', '1', '-', '*'})
    preserve(State::Rewind, a, State::Rewind, -1);
  preserve(State::Rewind, RIGHT, State::Rewind, -1);
  add(State::Rewind, LEFT, BOTTOM, State::MainStart, {BOTTOM}, +1);
  add(State::MainStart, '#', BOTTOM, State::AtVertex, {BOTTOM}, 0);

  // (2) At a vertex, guess either that it is the target or an outgoing edge.
  for (int z = 0; z < STACK_ALPHABET_SIZE; ++z) {
    add(State::AtVertex, '#', z, State::CheckFinal, {z}, +1);
    add(State::AtVertex, '#', z, State::SelectEdge, {z}, +1);
  }
  // The final branch accepts exactly at the right endmarker with only bottom.
  for (int a : {'(', ')', '[', ']', '1', '-', '*'})
    preserve(State::CheckFinal, a, State::CheckFinal, +1);
  add(State::CheckFinal, RIGHT, BOTTOM, State::Accept, {BOTTOM}, 0);

  // (3) Nondeterministically choose an edge or skip it and inspect the next.
  const std::pair<int, State> labels[] = {{'(', State::ParseR},
                                          {'[', State::ParseS},
                                          {')', State::ParseCR},
                                          {']', State::ParseCS}};
  for (auto [a, p] : labels)
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z) {
      add(State::SelectEdge, a, z, p, {z}, +1); // take this edge
      add(State::SelectEdge, a, z, State::SkipOffset, {z},
          +1); // skip this edge
    }
  for (int z = 0; z < STACK_ALPHABET_SIZE; ++z) {
    add(State::SkipOffset, '1', z, State::SkipRun, {z}, +1);
    add(State::SkipOffset, '-', z, State::SkipNegNeedOne, {z}, +1);
    add(State::SkipOffset, '*', z, State::SelectEdge, {z}, +1);
    add(State::SkipRun, '1', z, State::SkipRun, {z}, +1);
    add(State::SkipRun, '*', z, State::SelectEdge, {z}, +1);
    add(State::SkipNegNeedOne, '1', z, State::SkipRun, {z}, +1);
  }

  struct L {
    State parse, posRun, negNeed, negRun, zero, seek, right, afterR, afterRR,
        left, afterL, afterLR, apply;
    int stack;
  };
  const L ls[] = {
      {State::ParseR, State::PosRunR, State::NegNeedR, State::NegRunR,
       State::ZeroSeekR, State::SeekStartR, State::MoveRightR,
       State::AfterPopRightR, State::AfterPopRightReflectR, State::MoveLeftR,
       State::AfterPopLeftR, State::AfterPopLeftReflectR, State::ApplyR, ROUND},
      {State::ParseS, State::PosRunS, State::NegNeedS, State::NegRunS,
       State::ZeroSeekS, State::SeekStartS, State::MoveRightS,
       State::AfterPopRightS, State::AfterPopRightReflectS, State::MoveLeftS,
       State::AfterPopLeftS, State::AfterPopLeftReflectS, State::ApplyS,
       SQUARE},
      {State::ParseCR, State::PosRunCR, State::NegNeedCR, State::NegRunCR,
       State::ZeroSeekCR, State::SeekStartCR, State::MoveRightCR,
       State::AfterPopRightCR, State::AfterPopRightReflectCR, State::MoveLeftCR,
       State::AfterPopLeftCR, State::AfterPopLeftReflectCR, State::ApplyCR,
       ROUND},
      {State::ParseCS, State::PosRunCS, State::NegNeedCS, State::NegRunCS,
       State::ZeroSeekCS, State::SeekStartCS, State::MoveRightCS,
       State::AfterPopRightCS, State::AfterPopRightReflectCS, State::MoveLeftCS,
       State::AfterPopLeftCS, State::AfterPopLeftReflectCS, State::ApplyCS,
       SQUARE}};
  for (const auto &x : ls) {
    // Positive offset, including one or more unary 1s.
    pushCounter(x.parse, '1', x.posRun);
    pushCounter(x.posRun, '1', x.posRun);
    for (int a : std::array<int, 3>{'*', '#', RIGHT})
      for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
        add(x.posRun, a, z, x.right, {z}, 0);
    // Empty offset means zero; back up to this block's own marker.
    for (int a : std::array<int, 3>{'*', '#', RIGHT})
      for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
        add(x.parse, a, z, x.zero, {z}, -1);
    // Negative offset must contain at least one 1.
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      add(x.parse, '-', z, x.negNeed, {z}, +1);
    pushCounter(x.negNeed, '1', x.negRun);
    pushCounter(x.negRun, '1', x.negRun);
    for (int a : std::array<int, 3>{'*', '#', RIGHT})
      for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
        add(x.negRun, a, z, x.seek, {z}, -1);
    // Zero-offset scan: delimiter was backed over once above, now find '#'.
    for (int a : {'(', ')', '[', ']', '1', '-', '*'})
      preserve(x.zero, a, x.zero, -1);
    preserve(x.zero, RIGHT, x.zero, -1);
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      add(x.zero, '#', z, x.apply, {z}, 0);
    // Negative offsets first return to the source block marker, then step
    // left once before counting the first predecessor/virtual marker.
    for (int a : {'(', ')', '[', ']', '1', '-', '*'})
      preserve(x.seek, a, x.seek, -1);
    preserve(x.seek, RIGHT, x.seek, -1);
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      add(x.seek, '#', z, x.left, {z}, -1);

    // Rightward counting. A boundary consumes one COUNTER. If it was the
    // last counter, apply the edge label at that real vertex marker.
    add(x.right, '#', COUNTER, x.afterR, {}, 0);
    add(x.right, RIGHT, COUNTER, x.afterRR, {}, 0);
    for (int a : {'(', ')', '[', ']', '1', '-', '*'})
      preserve(x.right, a, x.right, +1);
    add(x.afterR, '#', COUNTER, x.right, {COUNTER},
        +1); // more distance remains
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      if (z != COUNTER)
        add(x.afterR, '#', z, x.apply, {z}, 0);
    add(x.afterRR, RIGHT, COUNTER, x.left, {COUNTER},
        -1); // reflect, only if more remains
    // If the counter became empty at the virtual endpoint, there is no rule.

    // Leftward counting, used for negative offsets and right reflection.
    add(x.left, '#', COUNTER, x.afterL, {}, 0);
    add(x.left, LEFT, COUNTER, x.afterLR, {}, 0);
    for (int a : {'(', ')', '[', ']', '1', '-', '*'})
      preserve(x.left, a, x.left, -1);
    add(x.afterL, '#', COUNTER, x.left, {COUNTER}, -1);
    for (int z = 0; z < STACK_ALPHABET_SIZE; ++z)
      if (z != COUNTER)
        add(x.afterL, '#', z, x.apply, {z}, 0);
    add(x.afterLR, LEFT, COUNTER, x.right, {COUNTER},
        +1); // reflect, only if more remains

    // Apply the chosen bracket label at the destination marker.
    if (x.apply == State::ApplyR || x.apply == State::ApplyS) {
      pushDyck(x.apply, State::AtVertex, x.stack);
    } else {
      int expected = x.apply == State::ApplyCR ? ROUND : SQUARE;
      add(x.apply, '#', expected, State::AtVertex, {}, 0);
    }
  }

  T.validate();
  return T;
}

enum class RunResult { Reject, Accept, Inconclusive, InvalidInput };
struct Configuration {
  State state;
  int head;
  std::vector<int> stack;
};

inline std::string configurationKey(const Configuration &c) {
  std::string s;
  s.reserve(24 + c.stack.size() * 2);
  s.append(reinterpret_cast<const char *>(&c.state), sizeof(c.state));
  s.append(reinterpret_cast<const char *>(&c.head), sizeof(c.head));
  for (int z : c.stack)
    s.push_back(static_cast<char>(z));
  s.push_back('\xff');
  return s;
}

// Simulates the explicit table by breadth-first exploration. A finite budget is
// necessary: a 2NPDA may have infinite rejecting branches. Acceptance is exact
// when found; Reject means the reachable configuration graph was exhausted.
inline RunResult run(const TransitionTable &T, const std::string &word,
                     size_t configurationBudget = 500000) {
  T.validate();
  if (word.empty() || word.find_first_not_of("()[]#1-*") != std::string::npos)
    return RunResult::InvalidInput;
  std::vector<int> tape;
  tape.reserve(word.size() + 2);
  tape.push_back(LEFT);
  for (unsigned char c : word)
    tape.push_back(c);
  tape.push_back(RIGHT);
  std::deque<Configuration> todo;
  todo.push_back({T.initial, 0, {BOTTOM}});
  std::unordered_set<std::string> seen;
  seen.insert(configurationKey(todo.front()));
  size_t explored = 0;
  while (!todo.empty()) {
    if (++explored > configurationBudget)
      return RunResult::Inconclusive;
    Configuration c = std::move(todo.front());
    todo.pop_front();
    if (c.state == T.accepting)
      return RunResult::Accept;
    if (c.stack.empty() || c.head < 0 || c.head >= (int)tape.size())
      continue;
    int a = tape[c.head], z = c.stack.back();
    for (const Rule &r : T.rules)
      if (r.from == c.state && r.read == a && r.top == z) {
        int np = c.head + r.move;
        if (np < 0 || np >= (int)tape.size())
          continue;
        Configuration d{r.to, np, c.stack};
        d.stack.pop_back();
        for (auto it = r.replaceTopFirst.rbegin();
             it != r.replaceTopFirst.rend(); ++it)
          d.stack.push_back(*it);
        auto key = configurationKey(d);
        if (seen.insert(std::move(key)).second)
          todo.push_back(std::move(d));
      }
  }
  return RunResult::Reject;
}

inline RunResult run(const std::string &word, size_t budget = 500000) {
  static const TransitionTable table = makeTransitionTable();
  return run(table, word, budget);
}

// One-step reflected coordinate semantics. A destination at a virtual tape
// endmarker is not a graph vertex and therefore produces no edge.
inline std::optional<int> reflectedRealVertex(int source1Based, int offset,
                                              int n) {
  if (n <= 0 || source1Based < 1 || source1Based > n)
    throw std::invalid_argument("invalid reflected vertex coordinates");
  const int64_t period = 2LL * (static_cast<int64_t>(n) + 1);
  int64_t y =
      (static_cast<int64_t>(source1Based) + static_cast<int64_t>(offset)) %
      period;
  if (y < 0)
    y += period;
  if (y > n + 1)
    y = period - y;
  if (y == 0 || y == n + 1)
    return std::nullopt;
  return static_cast<int>(y);
}

// Independent semantic interpretation of the fixed machine's accepted
// language. It builds exactly the graph described in equation (5), with only
// real vertices 1..n; virtual endmarkers are used for offset reflection but
// are not graph vertices.
inline cflcert::twopda::Decision recognize(const std::string &word) {
  cflcert::twopda::EncodedGraph enc;
  if (!cflcert::twopda::parseHardestWord(word, enc))
    return cflcert::twopda::Decision::Invalid;
  if (enc.size() > 46340)
    return cflcert::twopda::Decision::Invalid;
  const int n = static_cast<int>(enc.size());
  cflcert::Instance g;
  g.n = n;
  g.source = 0;
  g.target = n - 1;
  for (int u = 1; u <= n; ++u)
    for (const auto &e : enc[u - 1]) {
      auto v = reflectedRealVertex(u, e.offset, n);
      if (!v)
        continue; // an edge ending on a virtual endmarker is not in V
      cflcert::Label l = e.label == '('   ? cflcert::Label::Open1
                         : e.label == ')' ? cflcert::Label::Close1
                         : e.label == '[' ? cflcert::Label::Open2
                                          : cflcert::Label::Close2;
      g.edges.push_back({u - 1, *v - 1, l});
    }
  return cflcert::decideCubic(g) ? cflcert::twopda::Decision::Accept
                                 : cflcert::twopda::Decision::Reject;
}

inline const char *stateName(State s) {
  static const char *names[] = {"VStart",
                                "VNeedHash",
                                "VAfterHash",
                                "VAfterLabel",
                                "VPosRun",
                                "VNegNeedOne",
                                "VNegRun",
                                "VNeedLabel",
                                "Rewind",
                                "MainStart",
                                "AtVertex",
                                "CheckFinal",
                                "SelectEdge",
                                "SkipOffset",
                                "SkipNegNeedOne",
                                "SkipRun",
                                "ParseR",
                                "ParseS",
                                "ParseCR",
                                "ParseCS",
                                "PosRunR",
                                "PosRunS",
                                "PosRunCR",
                                "PosRunCS",
                                "NegNeedR",
                                "NegNeedS",
                                "NegNeedCR",
                                "NegNeedCS",
                                "NegRunR",
                                "NegRunS",
                                "NegRunCR",
                                "NegRunCS",
                                "ZeroSeekR",
                                "ZeroSeekS",
                                "ZeroSeekCR",
                                "ZeroSeekCS",
                                "SeekStartR",
                                "SeekStartS",
                                "SeekStartCR",
                                "SeekStartCS",
                                "MoveRightR",
                                "MoveRightS",
                                "MoveRightCR",
                                "MoveRightCS",
                                "AfterPopRightR",
                                "AfterPopRightS",
                                "AfterPopRightCR",
                                "AfterPopRightCS",
                                "AfterPopRightReflectR",
                                "AfterPopRightReflectS",
                                "AfterPopRightReflectCR",
                                "AfterPopRightReflectCS",
                                "MoveLeftR",
                                "MoveLeftS",
                                "MoveLeftCR",
                                "MoveLeftCS",
                                "AfterPopLeftR",
                                "AfterPopLeftS",
                                "AfterPopLeftCR",
                                "AfterPopLeftCS",
                                "AfterPopLeftReflectR",
                                "AfterPopLeftReflectS",
                                "AfterPopLeftReflectCR",
                                "AfterPopLeftReflectCS",
                                "ApplyR",
                                "ApplyS",
                                "ApplyCR",
                                "ApplyCS",
                                "Accept"};
  auto i = static_cast<int>(s);
  return (i >= 0 && i < static_cast<int>(State::Count)) ? names[i]
                                                        : "<invalid-state>";
}
inline const char *stackSymbolName(int z) {
  switch (z) {
  case BOTTOM:
    return "Z0";
  case ROUND:
    return "R";
  case SQUARE:
    return "S";
  case COUNTER:
    return "C";
  default:
    return "?";
  }
}
inline void writeTransitionTable(const TransitionTable &T, std::ostream &out) {
  T.validate();
  out << "state\tread\ttop\tnext\treplace(top-first)\tmove\n";
  for (const auto &r : T.rules) {
    out << stateName(r.from) << '\t';
    if (r.read == LEFT)
      out << "<LEFT>";
    else if (r.read == RIGHT)
      out << "<RIGHT>";
    else
      out << static_cast<char>(r.read);
    out << '\t' << stackSymbolName(r.top) << '\t' << stateName(r.to) << '\t';
    if (r.replaceTopFirst.empty())
      out << "epsilon";
    else
      for (size_t i = 0; i < r.replaceTopFirst.size(); ++i) {
        if (i)
          out << ',';
        out << stackSymbolName(r.replaceTopFirst[i]);
      }
    out << '\t' << r.move << '\n';
  }
}

struct Theorem21Audit {
  size_t encodedVertices = 0;
  size_t encodedEdges = 0;
  bool syntaxValid = false;
  bool allEncodedEdgesHaveRealEndpoints = false;
  bool concatenationHomomorphism = false;
  bool composedReachabilityAgrees = false;
  bool hardestMachineAccepts = false;
};

// Expensive audit helper for tests/reproducibility, not part of the reduction.
// It checks the concrete homomorphic image against the independent composition
// Lemma 26 -> Lemma 27 -> restricted-D2 semantics for one nonempty input.
inline Theorem21Audit auditTheorem21(const cflcert::twopda::Machine &A,
                                     const std::vector<int> &input) {
  if (input.empty())
    throw std::invalid_argument("Theorem 21 statement is for nonempty words");
  cflcert::twopda::Theorem21Homomorphism h(A);
  const std::string image = h.apply(input);
  cflcert::twopda::EncodedGraph parsed;
  Theorem21Audit report;
  report.syntaxValid = cflcert::twopda::parseHardestWord(image, parsed);
  if (!report.syntaxValid)
    return report;
  report.encodedVertices = parsed.size();
  report.allEncodedEdgesHaveRealEndpoints = true;
  for (size_t u = 0; u < parsed.size(); ++u)
    for (const auto &e : parsed[u]) {
      ++report.encodedEdges;
      if (!reflectedRealVertex(static_cast<int>(u) + 1, e.offset,
                               static_cast<int>(parsed.size())))
        report.allEncodedEdgesHaveRealEndpoints = false;
    }
  std::string concatenated;
  for (int a : input)
    concatenated += h.image(a);
  report.concatenationHomomorphism = (concatenated == image);
  auto P = cflcert::twopda::compileInputToPDAEmptiness(A, input);
  auto G = cflcert::pda::reducePDAEmptinessToDyck2(P);
  const bool composed = cflcert::decideCubic(G);
  const bool fixed = (recognize(image) == cflcert::twopda::Decision::Accept);
  report.composedReachabilityAgrees = (composed == fixed);
  report.hardestMachineAccepts = fixed;
  return report;
}

} // namespace cflcert::hardest
