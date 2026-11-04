#pragma once

#include "Checker/Pulse/Core/PulseFormula.h"

#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

namespace llvm {
class Instruction;
} // namespace llvm

namespace pulse {

/// LLVM implementation of Infer's TOPL call/array-write property language.
/// Histories belong to individual Pulse disjuncts, and are replayed after
/// callee-to-caller substitution. This also resolves guards that were unknown
/// when a procedure was analyzed in isolation.
struct ToplValue {
  std::optional<AbstractValue> value;
  std::optional<int64_t> constant;
  bool operator==(const ToplValue &other) const {
    return value == other.value && constant == other.constant;
  }
};

struct ToplEvent {
  enum class Kind { Call, ArrayWrite };
  Kind kind = Kind::Call;
  std::string name;
  std::vector<ToplValue> arguments; // Calls include the return slot, even void.
  const llvm::Instruction *location = nullptr;
  std::vector<const llvm::Instruction *> callingContext;
  bool operator==(const ToplEvent &other) const {
    return kind == other.kind && name == other.name &&
           arguments == other.arguments && location == other.location &&
           callingContext == other.callingContext;
  }
};

struct ToplHistory {
  static constexpr size_t MaxEvents = 128;
  std::vector<ToplEvent> events;
  bool truncated = false;
  void append(ToplEvent event) {
    if (truncated)
      return;
    if (events.size() == MaxEvents) {
      truncated = true;
      return;
    }
    events.push_back(std::move(event));
  }
  bool operator==(const ToplHistory &other) const {
    return truncated == other.truncated && events == other.events;
  }
};

struct ToplGuard {
  std::string lhs, op, rhs;
};

struct ToplTransition {
  std::string source, target;
  bool wildcard = false;
  bool arrayWrite = false;
  bool anyArguments = false;
  std::string pattern;
  std::regex matcher;
  std::vector<std::string> bindings;
  std::vector<ToplGuard> guards;
  std::vector<std::pair<std::string, std::string>> assignments;
};

struct ToplProperty {
  std::string name, message;
  std::vector<std::string> prefixes;
  std::vector<ToplTransition> transitions;
};

struct ToplViolation {
  std::string property, message;
  std::vector<ToplEvent> trace;
};

class ToplProgram {
  std::vector<ToplProperty> properties_;

public:
  static llvm::Expected<ToplProgram> parse(llvm::StringRef text,
                                           llvm::StringRef source = "<topl>");
  bool empty() const { return properties_.empty(); }
  bool observes(ToplEvent::Kind kind, llvm::StringRef name) const;
  std::vector<ToplViolation> evaluate(const ToplHistory &history,
                                      const PulseFormula &formula) const;
};

} // namespace pulse
