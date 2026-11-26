#include "Checker/Pulse/Topl/PulseTopl.h"
#include "Checker/Pulse/Core/PulseMemory.h"

#include <algorithm>
#include <queue>
#include <set>
#include <sstream>

#include <llvm/Demangle/Demangle.h>

namespace pulse {
namespace {
std::string trim(llvm::StringRef s) { return s.trim().str(); }

std::vector<std::string> splitParts(llvm::StringRef text,
                                    llvm::StringRef delimiter) {
  std::vector<std::string> result;
  while (!text.empty()) {
    auto pieces = text.split(delimiter);
    result.push_back(trim(pieces.first));
    text = pieces.second;
  }
  return result;
}

bool identifier(const std::string &s) {
  static const std::regex re("[A-Za-z_][A-Za-z_0-9]*");
  return std::regex_match(s, re);
}

std::string unquote(const std::string &s) {
  // Preserve regex backslashes. Quotes are delimiters, not C string escapes.
  return s.size() >= 2 && s.front() == '"' && s.back() == '"'
             ? s.substr(1, s.size() - 2)
             : s;
}

std::optional<int64_t> integer(const std::string &s) {
  int64_t value;
  if (llvm::StringRef(s).getAsInteger(10, value))
    return std::nullopt;
  return value;
}

bool matches(const ToplTransition &t, ToplEvent::Kind kind,
             const std::string &name) {
  if (t.wildcard)
    return true;
  if (t.arrayWrite)
    return kind == ToplEvent::Kind::ArrayWrite;
  if (kind != ToplEvent::Kind::Call)
    return false;
  if (std::regex_match(name, t.matcher))
    return true;
  const std::string demangled = llvm::demangle(name);
  // LLVM symbols include ABI encodings; allow both a full demangled signature
  // and its qualified function name for source-level patterns.
  return std::regex_match(demangled, t.matcher) ||
         std::regex_match(demangled.substr(0, demangled.find('(')), t.matcher);
}

using Environment = std::map<std::string, ToplValue>;
std::optional<ToplValue> lookup(const std::string &name,
                                const Environment &env) {
  if (auto n = integer(name))
    return ToplValue{std::nullopt, n};
  auto it = env.find(name);
  return it == env.end() ? std::nullopt : std::optional<ToplValue>(it->second);
}

std::optional<int64_t> concrete(const ToplValue &v,
                                const PulseFormula &formula) {
  if (v.constant)
    return v.constant;
  if (v.value) {
    if (formula.isNull(*v.value))
      return 0;
    auto lo = formula.getLowerBound(*v.value);
    auto hi = formula.getUpperBound(*v.value);
    if (lo && hi && *lo == *hi)
      return lo;
  }
  return std::nullopt;
}

// Unknown predicates do not produce a violation. The symbolic event remains
// in the summary so that a caller with stronger facts can resolve it later.
static std::optional<bool> heapReachable(const ToplValue &src,
                                         const ToplValue &dst,
                                         const PulseFormula &f,
                                         const Heap *post_heap) {
  if (!post_heap || !src.value || !dst.value)
    return std::nullopt;
  AbstractValue start = f.getCanonical(*src.value);
  AbstractValue target = f.getCanonical(*dst.value);
  if (start == target)
    return true;
  std::set<AbstractValue> visited;
  std::queue<AbstractValue> q;
  visited.insert(start);
  q.push(start);
  while (!q.empty()) {
    AbstractValue cur = q.front();
    q.pop();
    auto it = post_heap->getEdges().find(cur);
    if (it == post_heap->getEdges().end())
      continue;
    for (const auto &ekv : it->second) {
      AbstractValue nxt = f.getCanonical(ekv.second.addr);
      if (nxt == target)
        return true;
      if (visited.insert(nxt).second)
        q.push(nxt);
    }
  }
  return false;
}

std::optional<bool> compare(const ToplValue &a, const std::string &op,
                            const ToplValue &b, const PulseFormula &f,
                            const Heap *post_heap) {
  if (op == "~~>" || op == "!~>") {
    auto r = heapReachable(a, b, f, post_heap);
    if (!r)
      return std::nullopt;
    return (op == "~~>") ? *r : !*r;
  }
  auto x = concrete(a, f), y = concrete(b, f);
  if (x && y) {
    if (op == "==")
      return *x == *y;
    if (op == "!=")
      return *x != *y;
    if (op == "<")
      return *x < *y;
    if (op == "<=")
      return *x <= *y;
    if (op == ">")
      return *x > *y;
    if (op == ">=")
      return *x >= *y;
  }
  auto lower = [&](const ToplValue &v, std::optional<int64_t> n) {
    return n ? n : v.value ? f.getLowerBound(*v.value) : std::nullopt;
  };
  auto upper = [&](const ToplValue &v, std::optional<int64_t> n) {
    return n ? n : v.value ? f.getUpperBound(*v.value) : std::nullopt;
  };
  auto alo = lower(a, x), ahi = upper(a, x);
  auto blo = lower(b, y), bhi = upper(b, y);
  if (op == "<" || op == "<=") {
    if (ahi && blo && (op == "<" ? *ahi < *blo : *ahi <= *blo))
      return true;
    if (alo && bhi && (op == "<" ? *alo >= *bhi : *alo > *bhi))
      return false;
  }
  if (op == ">" || op == ">=") {
    if (alo && bhi && (op == ">" ? *alo > *bhi : *alo >= *bhi))
      return true;
    if (ahi && blo && (op == ">" ? *ahi <= *blo : *ahi < *blo))
      return false;
  }
  if ((op == "==" || op == "!=") &&
      ((ahi && blo && *ahi < *blo) || (bhi && alo && *bhi < *alo)))
    return op == "!=";
  if (a.value && b.value) {
    if (f.areEqual(*a.value, *b.value)) {
      if (op == "==" || op == "<=" || op == ">=")
        return true;
      return false;
    }
    if (f.areDisequal(*a.value, *b.value)) {
      if (op == "==")
        return false;
      if (op == "!=")
        return true;
    }
  }
  if (a.value && y && *y == 0 && f.isNonNull(*a.value)) {
    if (op == "==")
      return false;
    if (op == "!=")
      return true;
  }
  if (b.value && x && *x == 0 && f.isNonNull(*b.value)) {
    if (op == "==")
      return false;
    if (op == "!=")
      return true;
  }
  // Fall back to PulseFormula relational / Z3 entailment query.
  return f.impliesComparison(a.value, x, op, b.value, y);
}
} // namespace

llvm::Expected<ToplProgram> ToplProgram::parse(llvm::StringRef text,
                                               llvm::StringRef source) {
  ToplProgram program;
  ToplProperty *property = nullptr;
  std::istringstream input(text.str());
  std::string line;
  unsigned lineNumber = 0;
  auto error = [&](const std::string &message) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), "%s:%u: %s",
                                   source.str().c_str(), lineNumber,
                                   message.c_str());
  };
  const std::regex transition(
      R"(^([A-Za-z_][A-Za-z_0-9]*)\s*->\s*([A-Za-z_][A-Za-z_0-9]*)\s*:\s*(.*)$)");
  const std::regex call(
      R"topl(^("[^"]*"|[A-Za-z_#][A-Za-z_0-9#]*)(?:\s*(\(([^)]*)\)))?\s*(.*)$)topl");
  const std::regex predicate(
      R"(^([A-Za-z_][A-Za-z_0-9]*|-?[0-9]+)\s*(==|!=|<=|>=|<|>|~~>|!~>)\s*([A-Za-z_][A-Za-z_0-9]*|-?[0-9]+)$)");
  const std::regex assignment(
      R"(^([a-z_][A-Za-z_0-9]*)\s*:=\s*([A-Z][A-Za-z_0-9]*)$)");
  while (std::getline(input, line)) {
    ++lineNumber;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"' && (i == 0 || line[i - 1] != '\\'))
        quoted = !quoted;
      if (!quoted && line.compare(i, 2, "//") == 0) {
        line.resize(i);
        break;
      }
    }
    line = trim(line);
    if (line.empty())
      continue;
    llvm::StringRef s(line);
    if (s.consume_front("property ")) {
      std::string name = trim(s);
      if (!identifier(name))
        return error("invalid property name");
      for (const auto &p : program.properties_)
        if (p.name == name)
          return error("duplicate property name");
      program.properties_.push_back({});
      property = &program.properties_.back();
      property->name = name;
      continue;
    }
    if (!property)
      return error("expected a property declaration");
    if (s.startswith("message ") || s.startswith("prefix ")) {
      bool message = s.consume_front("message ");
      if (!message)
        s.consume_front("prefix ");
      std::string value = trim(s);
      if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return error("expected a quoted string");
      if (message)
        property->message = unquote(value);
      else
        property->prefixes.push_back(unquote(value));
      continue;
    }
    std::smatch m;
    if (!std::regex_match(line, m, transition))
      return error("invalid transition (field and reachability predicates are "
                   "unsupported)");
    ToplTransition t;
    t.source = m[1];
    t.target = m[2];
    std::string label = trim(m[3].str());
    if (label == "*") {
      t.wildcard = true;
    } else {
      if (!std::regex_match(label, m, call))
        return error("invalid call pattern");
      t.pattern = unquote(m[1]);
      t.arrayWrite = t.pattern == "#ArrayWrite";
      t.anyArguments = !m[2].matched;
      std::string args = trim(m[3].str());
      t.bindings = splitParts(args, ",");
      std::set<std::string> bound;
      for (const auto &b : t.bindings) {
        if (b != "_" && (!identifier(b) || b.front() < 'A' || b.front() > 'Z'))
          return error(
              "transition bindings must start with an uppercase letter");
        bound.insert(b);
      }
      if (t.arrayWrite && t.bindings.size() != 2)
        return error("#ArrayWrite needs array and index bindings");
      std::string tail = trim(m[4].str());
      auto parts = llvm::StringRef(tail).split("=>");
      std::string conditions = trim(parts.first);
      if (!conditions.empty()) {
        llvm::StringRef conditionText(conditions);
        if (!conditionText.consume_front("when "))
          return error("expected 'when' or '=>'");
        for (const auto &g : splitParts(conditionText, "&&")) {
          if (!std::regex_match(g, m, predicate))
            return error("invalid guard");
          for (unsigned i : {1u, 3u}) {
            std::string operand = m[i];
            if (operand.front() >= 'A' && operand.front() <= 'Z' &&
                !bound.count(operand))
              return error("guard references an unbound transition variable");
          }
          t.guards.push_back({m[1], m[2], m[3]});
        }
      }
      if (tail.find("=>") != std::string::npos) {
        auto actions = splitParts(parts.second, ";");
        if (actions.empty())
          return error("expected an assignment");
        for (const auto &a : actions) {
          if (!std::regex_match(a, m, assignment) || !bound.count(m[2].str()))
            return error("invalid register assignment");
          t.assignments.emplace_back(m[1], m[2]);
        }
      }
    }
    property->transitions.push_back(std::move(t));
  }
  if (program.empty())
    return error("no properties");
  for (auto &p : program.properties_) {
    bool hasStart = false, hasError = false;
    for (auto &t : p.transitions) {
      hasStart |= t.source == "start";
      hasError |= t.target == "error";
      if (t.wildcard || t.arrayWrite)
        continue;
      std::string expression = t.pattern;
      if (!p.prefixes.empty()) {
        std::string prefixes;
        for (const auto &prefix : p.prefixes) {
          if (!prefixes.empty())
            prefixes += '|';
          prefixes += prefix;
        }
        expression = "(" + prefixes + ")" + expression;
      }
      try {
        t.matcher = std::regex(expression);
      } catch (const std::regex_error &) {
        return error("invalid regex in property " + p.name);
      }
    }
    if (!hasStart || !hasError)
      return error("property " + p.name + " needs start and error states");
  }
  return program;
}

bool ToplProgram::observes(ToplEvent::Kind kind, llvm::StringRef name) const {
  for (const auto &p : properties_)
    for (const auto &t : p.transitions)
      if ((t.wildcard && t.source != t.target) ||
          (!t.wildcard && matches(t, kind, name.str())))
        return true;
  return false;
}

std::vector<ToplViolation>
ToplProgram::evaluate(const ToplHistory &history, const PulseFormula &formula,
                      const Heap *post_heap) const {
  // The omitted suffix might contain cleanup transitions out of error.
  // Incomplete histories cannot establish a final-state violation.
  if (history.truncated)
    return {};
  struct Configuration {
    std::string state = "start";
    Environment registers;
    std::vector<ToplEvent> trace;
  };
  std::vector<ToplViolation> violations;
  for (const auto &p : properties_) {
    std::vector<Configuration> configurations(1);
    for (const auto &event : history.events) {
      std::vector<Configuration> next;
      for (const auto &config : configurations) {
        bool taken = false;
        bool unknownMatch = false;
        for (const auto &t : p.transitions) {
          if (t.source != config.state || !matches(t, event.kind, event.name))
            continue;
          Environment env = config.registers;
          bool valid = true;
          bool uncertain = false;
          if (!t.wildcard && !t.anyArguments) {
            if (t.bindings.size() != event.arguments.size())
              continue;
            Environment bindings;
            for (size_t i = 0; i < t.bindings.size(); ++i) {
              const auto &name = t.bindings[i];
              if (name == "_")
                continue;
              auto inserted = bindings.emplace(name, event.arguments[i]);
              if (!inserted.second) {
                auto decision = compare(inserted.first->second, "==",
                                        event.arguments[i], formula, post_heap);
                uncertain |= !decision.has_value();
                if (decision && !*decision)
                  valid = false;
              }
              env[name] = event.arguments[i];
            }
          }
          for (const auto &g : t.guards) {
            auto lhs = lookup(g.lhs, env), rhs = lookup(g.rhs, env);
            if (!lhs || !rhs) {
              valid = false;
              continue;
            }
            auto decision = compare(*lhs, g.op, *rhs, formula, post_heap);
            uncertain |= !decision.has_value();
            if (decision && !*decision)
              valid = false;
          }
          if (!valid)
            continue;
          if (uncertain) {
            unknownMatch = true;
            continue;
          }
          taken = true;
          Configuration successor = config;
          successor.state = t.target;
          for (const auto &a : t.assignments)
            successor.registers[a.first] = env.at(a.second);
          if (!t.wildcard || t.source != t.target)
            successor.trace.push_back(event);
          if (next.size() < 64) {
            // Identical state/register pairs need only one witness.
            bool duplicate = std::any_of(
                next.begin(), next.end(), [&](const Configuration &c) {
                  return c.state == successor.state &&
                         c.registers == successor.registers;
                });
            if (!duplicate)
              next.push_back(std::move(successor));
          }
        }
        // Stuttering after an unresolved cleanup guard would invent a witness
        // in which cleanup failed. Drop this monitor path; replay in a caller
        // with stronger facts can recover it.
        if (!taken && !unknownMatch && next.size() < 64)
          next.push_back(config);
      }
      configurations = std::move(next);
    }
    // Error is a distinguished state, not an absorbing state. Properties such
    // as Infer's LockUnlock can recover from error on an unlock event.
    for (const auto &config : configurations) {
      if (config.state == "error")
        violations.push_back(
            {p.name, p.message.empty() ? "property reaches error" : p.message,
             config.trace});
    }
  }
  return violations;
}
} // namespace pulse
