/**
 * Lossy Finite Automaton Semiring
 * 
 * Models regular languages with lossy semantics where each symbol 'a'
 * can be treated as 'a | epsilon' (symbol can be optionally skipped).
 * 
 * NOTE: Requires libfa library to be installed.
 */

#ifndef FPSOLVE_LOSSY_FINITE_AUTOMATON_H
#define FPSOLVE_LOSSY_FINITE_AUTOMATON_H

#ifdef HAVE_LIBFA

#include "Solvers/FPSolve/Semirings/Semiring.h"
#include "Solvers/FPSolve/DataStructs/FiniteAutomaton.h"
#include "Solvers/FPSolve/DataStructs/Var.h"
#include <string>
#include <sstream>
#include <cctype>

namespace fpsolve {

/**
 * Lossy Finite Automaton
 * 
 * Regular language where equality and operations are with respect to
 * "lossified" versions where each alphabet symbol can be epsilon.
 */
class LossyFiniteAutomaton : public StarableSemiring<LossyFiniteAutomaton,
                                                       Commutativity::NonCommutative,
                                                       Idempotence::Idempotent> {
private:
  FiniteAutomaton language;
  static LossyFiniteAutomaton EMPTY;
  static LossyFiniteAutomaton EPSILON;

  LossyFiniteAutomaton(const FiniteAutomaton& fa) : language(fa) {}

public:
  // Default: epsilon (multiplicative identity)
  LossyFiniteAutomaton() : language(FiniteAutomaton::epsilon()) {}

  // From regular expression string
  LossyFiniteAutomaton(const std::string& regularExpression) 
      : language(FiniteAutomaton(regularExpression).minimize()) {}

  // From variable
  LossyFiniteAutomaton(VarId var) 
      : language(FiniteAutomaton(Var::GetVar(var).string()).minimize()) {}

  // Lossify: make all symbols optional (a -> a|ε)
  LossyFiniteAutomaton lossify() const {
    if (language.empty()) {
      return *this;
    }
    return LossyFiniteAutomaton(language.epsilonClosure()).minimize();
  }

  LossyFiniteAutomaton minimize() const {
    return LossyFiniteAutomaton(language.minimize());
  }

  static LossyFiniteAutomaton null() {
    return LossyFiniteAutomaton(FiniteAutomaton());
  }

  static LossyFiniteAutomaton one() {
    return LossyFiniteAutomaton(FiniteAutomaton::epsilon());
  }

  // Lossify a regular expression string
  static std::string lossifiedRegex(const std::string& regex) {
    std::stringstream ss;

    for (size_t i = 0; i < regex.size(); i++) {
      // Handle iteration groups {m,n}
      if (regex[i] == '{') {
        while (i < regex.size() && regex[i] != '}') {
          ss << regex[i];
          i++;
        }
        if (i < regex.size()) {
          ss << regex[i]; // '}'
        }
      }
      // Handle character sets [...]
      else if (regex[i] == '[') {
        ss << "(";
        while (i < regex.size() && regex[i] != ']') {
          ss << regex[i];
          i++;
        }
        if (i < regex.size()) {
          ss << regex[i] << "|())"; // ']'
        }
      }
      // Lossify alphanumeric characters
      else if (std::isalnum(regex[i])) {
        ss << "(" << regex[i] << "|())";
      }
      // Keep regex operators as-is
      else {
        ss << regex[i];
      }
    }

    return ss.str();
  }

  // Semiring operations
  LossyFiniteAutomaton operator+=(const LossyFiniteAutomaton& elem) {
    language = language.union_op(elem.language).minimize();
    return *this;
  }

  LossyFiniteAutomaton operator*=(const LossyFiniteAutomaton& elem) {
    language = language.concat(elem.language).minimize();
    return *this;
  }

  bool operator==(const LossyFiniteAutomaton& elem) const {
    // Equality with respect to lossified versions
    return lossify().language == elem.lossify().language;
  }

  LossyFiniteAutomaton star() const {
    return LossyFiniteAutomaton(language.star().minimize());
  }

  std::string string() const {
    try {
      return language.to_regexp();
    } catch (...) {
      return "<automaton>";
    }
  }

  // Get size (number of states)
  size_t size() const {
    return language.size();
  }

  // Check if language is empty
  bool is_empty() const {
    return language.empty();
  }

  // Check if contains epsilon
  bool contains_epsilon() const {
    return language.contains_epsilon();
  }

  const FiniteAutomaton& get_language() const {
    return language;
  }
};

} // namespace fpsolve

#endif // HAVE_LIBFA

#endif // FPSOLVE_LOSSY_FINITE_AUTOMATON_H

