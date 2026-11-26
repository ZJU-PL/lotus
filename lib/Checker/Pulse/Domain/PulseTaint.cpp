#include "Checker/Pulse/Domain/PulseTaint.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Report/PulseDiagnostic.h"

#include <queue>
#include <set>
#include <vector>

#include <llvm/IR/Instructions.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// TaintDomain Implementation
//===----------------------------------------------------------------------===//

void TaintDomain::add(AbstractValue v, TaintItem item) {
  taints_[v].insert(std::move(item));
}

void TaintDomain::remove(AbstractValue v) { taints_.erase(v); }

bool TaintDomain::has(AbstractValue v) const {
  auto it = taints_.find(v);
  return it != taints_.end() && !it->second.empty();
}

const std::set<TaintItem> &TaintDomain::get(AbstractValue v) const {
  auto it = taints_.find(v);
  static const std::set<TaintItem> empty;
  return (it != taints_.end()) ? it->second : empty;
}

void TaintDomain::join(const TaintDomain &other) {
  for (const auto &kv : other.taints_) {
    taints_[kv.first].insert(kv.second.begin(), kv.second.end());
  }
}

//===----------------------------------------------------------------------===//
// TaintOperations Implementation
//===----------------------------------------------------------------------===//

unsigned TaintOperations::global_timestamp_ = 1;
const TaintConfig *TaintOperations::config_ = nullptr;

namespace {

static bool itemSanitizedForPolicy(const TaintItem &item,
                                   const SinkPolicy &policy) {
  for (const auto &san : item.sanitizers) {
    // Only sanitizers applied at or after taint creation count.
    if (san.sanitizer_timestamp < item.timestamp) {
      continue;
    }
    // Generic Unknown sanitizer clears taint for all policies.
    if (san.sanitizer_kind == TaintKind::Unknown()) {
      return true;
    }
    if (policy.sanitizer_kinds.empty()) {
      return true;
    }
    for (const auto &pk : policy.sanitizer_kinds) {
      if (san.sanitizer_kind == pk) {
        return true;
      }
    }
  }
  return false;
}

static bool itemSanitizedGeneric(const TaintItem &item) {
  for (const auto &san : item.sanitizers) {
    if (san.sanitizer_timestamp >= item.timestamp) {
      return true;
    }
  }
  return false;
}

} // namespace

void TaintOperations::taint(AbductiveDomain &astate, AbstractValue v,
                            const std::vector<TaintKind> &kinds,
                            const llvm::Instruction *source) {
  AbstractValue canon = astate.getCanonical(v);
  const llvm::Function *func = nullptr;
  if (source) {
    if (const llvm::BasicBlock *bb = source->getParent())
      func = bb->getParent();
  }
  unsigned timestamp = getNextTimestamp();

  TaintValue value(TaintValue::Type::TaintProcedure,
                   func ? func->getName().str() : "");
  TaintValueTuple value_tuple(value, TaintOrigin::ReturnValue);

  TaintItem item(kinds, value_tuple, source, func, timestamp);
  item.history.addEvent(ValueHistory::EventKind::Unknown, source, func);
  astate.getTaintDomain().add(canon, std::move(item));
  astate.getPostAttrs().add(canon, Attribute::Tainted);
}

void TaintOperations::taint(AbductiveDomain &astate, AbstractValue v,
                            TaintKind kind, const std::string &procedure_name) {
  AbstractValue canon = astate.getCanonical(v);
  unsigned timestamp = getNextTimestamp();
  TaintValue value(TaintValue::Type::TaintProcedure, procedure_name);
  TaintValueTuple value_tuple(value, TaintOrigin::ReturnValue);
  TaintItem item({std::move(kind)}, value_tuple, nullptr, nullptr, timestamp);
  astate.getTaintDomain().add(canon, std::move(item));
  astate.getPostAttrs().add(canon, Attribute::Tainted);
}

bool TaintOperations::checkSink(AbductiveDomain &astate, AbstractValue v,
                                const std::string &sink_name,
                                const llvm::Instruction *sink_loc) {
  AbstractValue canon_v = astate.getCanonical(v);

  // Collect direct and heap-reachable values (Infer's fold_reachable_values).
  std::set<AbstractValue> visited;
  std::queue<AbstractValue> worklist;
  visited.insert(canon_v);
  worklist.push(canon_v);

  std::vector<const TaintItem *> candidate_items;
  while (!worklist.empty()) {
    AbstractValue cur = worklist.front();
    worklist.pop();
    if (astate.getTaintDomain().has(cur)) {
      for (const auto &item : astate.getTaintDomain().get(cur)) {
        candidate_items.push_back(&item);
      }
    }
    auto edge_it = astate.getPostHeap().getEdges().find(cur);
    if (edge_it != astate.getPostHeap().getEdges().end()) {
      for (const auto &ekv : edge_it->second) {
        AbstractValue nxt = astate.getCanonical(ekv.second.addr);
        if (visited.insert(nxt).second) {
          worklist.push(nxt);
        }
      }
    }
  }

  if (candidate_items.empty()) {
    return false;
  }

  const TaintConfig &config = getConfig();
  std::vector<SinkPolicy> policies;
  auto matched_sinks = config.matchSinksByName(sink_name);
  std::set<TaintKind> seen_sink_kinds;
  for (const ProcedureUnit *unit : matched_sinks) {
    for (const TaintKind &sk : unit->kinds) {
      if (seen_sink_kinds.insert(sk).second) {
        const auto &pvec = config.getSinkPolicies(sk);
        policies.insert(policies.end(), pvec.begin(), pvec.end());
      }
    }
  }
  if (policies.empty() && !seen_sink_kinds.empty()) {
    const auto &fallback = config.getSinkPolicies(TaintKind::UserInput());
    policies.insert(policies.end(), fallback.begin(), fallback.end());
  }

  TaintKind sink_kind =
      seen_sink_kinds.empty() ? TaintKind::Unknown() : *seen_sink_kinds.begin();
  (void)sink_kind;

  std::vector<const TaintItem *> unsanitized_taints;
  for (const TaintItem *item_ptr : candidate_items) {
    const TaintItem &item = *item_ptr;
    if (policies.empty()) {
      if (!itemSanitizedGeneric(item)) {
        unsanitized_taints.push_back(&item);
      }
      continue;
    }

    bool matched_policy = false;
    for (const auto &policy : policies) {
      if (itemSanitizedForPolicy(item, policy)) {
        continue;
      }
      for (const auto &item_kind : item.kinds) {
        for (const auto &source_kind : policy.source_kinds) {
          if (item_kind == source_kind || item_kind == TaintKind::Unknown()) {
            matched_policy = true;
            break;
          }
        }
        if (matched_policy)
          break;
      }
      if (matched_policy)
        break;
    }

    if (matched_policy) {
      unsanitized_taints.push_back(&item);
    }
  }

  if (unsanitized_taints.empty()) {
    return false;
  }

  const TaintItem *critical_item = nullptr;
  TaintKind critical_kind = TaintKind::Unknown();

  for (const auto *item : unsanitized_taints) {
    TaintKind primary = item->getPrimaryKind();
    if (!critical_item) {
      critical_item = item;
      critical_kind = primary;
    } else if (primary == TaintKind::Sensitive()) {
      critical_item = item;
      critical_kind = primary;
    } else if (critical_kind != TaintKind::Sensitive() &&
               primary == TaintKind::Network()) {
      critical_item = item;
      critical_kind = primary;
    } else if (critical_kind == TaintKind::Unknown() &&
               primary != TaintKind::Unknown()) {
      critical_item = item;
      critical_kind = primary;
    }
  }

  if (!critical_item) {
    return false;
  }

  const TaintItem &critical = *critical_item;

  BugReportMgr &mgr = BugReportMgr::get_instance();
  int typeId = mgr.register_bug_type(IssueType::TaintError,
                                     BugDescription::BI_HIGH,
                                     BugDescription::BC_SECURITY, "CWE-20");

  BugReport *report = new BugReport(typeId);

  if (critical.source_instruction) {
    std::string src_msg = "Taint source: ";
    TaintKind primary = critical.getPrimaryKind();
    if (primary == TaintKind::Network()) {
      src_msg += "Network input";
    } else if (primary == TaintKind::UserInput()) {
      src_msg += "User input";
    } else if (primary == TaintKind::FileSystem()) {
      src_msg += "File system";
    } else if (primary == TaintKind::Environment()) {
      src_msg += "Environment variable";
    } else if (primary == TaintKind::Sensitive()) {
      src_msg += "Sensitive data";
    } else {
      src_msg += "Unknown source";
    }

    report->append_step(
        const_cast<llvm::Instruction *>(critical.source_instruction), src_msg,
        0, {}, "source");
  }

  const auto &events = critical.history.getEvents();
  unsigned step_num = 1;
  for (const auto &event : events) {
    if (event.location == nullptr ||
        event.location == critical.source_instruction) {
      continue;
    }

    std::string trace_msg = "Taint propagated here";
    if (event.kind == ValueHistory::EventKind::Store) {
      trace_msg += " (via store)";
    } else if (event.kind == ValueHistory::EventKind::Load) {
      trace_msg += " (via load)";
    } else if (event.kind == ValueHistory::EventKind::FunctionCall) {
      trace_msg += " (via function call)";
    }
    report->append_step(const_cast<llvm::Instruction *>(event.location),
                        trace_msg, step_num++, {}, "trace");
  }

  std::string sink_msg = "Tainted data flows into sink '";
  sink_msg += sink_name;
  sink_msg += "'";
  if (sink_name == "system" || sink_name == "exec" || sink_name == "popen") {
    sink_msg += " (command injection risk)";
  } else if (sink_name == "printf" || sink_name == "sprintf") {
    sink_msg += " (format string vulnerability)";
  } else if (sink_name == "strcpy" || sink_name == "strcat") {
    sink_msg += " (buffer overflow risk)";
  }

  report->append_step(const_cast<llvm::Instruction *>(sink_loc), sink_msg, 0,
                      {}, "sink");

  int confidence = 80;
  if (critical_kind == TaintKind::Sensitive() ||
      critical_kind == TaintKind::Network()) {
    confidence = 95;
  } else if (critical_kind == TaintKind::UserInput()) {
    confidence = 90;
  }
  if (sink_name == "system" || sink_name == "exec") {
    confidence = std::min(95, confidence + 5);
  }

  report->set_conf_score(confidence);
  mgr.insert_report(typeId, report, true);
  return true;
}

void TaintOperations::propagate(AbductiveDomain &astate, AbstractValue src,
                                AbstractValue dest,
                                const llvm::Instruction *loc) {
  AbstractValue c_src = astate.getCanonical(src);
  AbstractValue c_dest = astate.getCanonical(dest);
  if (!astate.getTaintDomain().has(c_src)) {
    return;
  }

  // Copy first in case c_src == c_dest.
  std::vector<TaintItem> copied(astate.getTaintDomain().get(c_src).begin(),
                                astate.getTaintDomain().get(c_src).end());
  for (auto &item : copied) {
    item.history.addEvent(ValueHistory::EventKind::Store, loc,
                          loc ? loc->getFunction() : nullptr);
    astate.getTaintDomain().add(c_dest, std::move(item));
  }
  astate.getPostAttrs().add(c_dest, Attribute::Tainted);
}

void TaintOperations::sanitize(AbductiveDomain &astate, AbstractValue v,
                               TaintKind sanitizer_kind,
                               const llvm::Instruction *sanitizer_loc) {
  AbstractValue canon = astate.getCanonical(v);
  if (!astate.getTaintDomain().has(canon)) {
    return;
  }

  unsigned sanitizer_timestamp = getNextTimestamp();
  // Safe copy-modify-reinsert (avoids UB const_cast on std::set keys).
  std::vector<TaintItem> updated;
  const auto &existing = astate.getTaintDomain().get(canon);
  updated.reserve(existing.size());
  for (TaintItem item : existing) {
    item.addSanitizer(sanitizer_kind, sanitizer_loc, sanitizer_timestamp);
    updated.push_back(std::move(item));
  }
  astate.getTaintDomain().remove(canon);
  for (auto &item : updated) {
    astate.getTaintDomain().add(canon, std::move(item));
  }
}

bool TaintOperations::isSanitized(const AbductiveDomain &astate,
                                  AbstractValue v) {
  AbstractValue canon = astate.getCanonical(v);
  if (!astate.getTaintDomain().has(canon)) {
    return false;
  }

  const auto &taints = astate.getTaintDomain().get(canon);
  for (const auto &item : taints) {
    if (!itemSanitizedGeneric(item)) {
      return false;
    }
  }
  return true;
}

void TaintOperations::propagateThroughLoad(AbductiveDomain &astate,
                                           AbstractValue src_addr,
                                           AbstractValue dest_val,
                                           const llvm::Instruction *loc) {
  propagate(astate, src_addr, dest_val, loc);
}

void TaintOperations::propagateThroughStore(AbductiveDomain &astate,
                                            AbstractValue src_val,
                                            AbstractValue dest_addr,
                                            const llvm::Instruction *loc) {
  propagate(astate, src_val, dest_addr, loc);
}

void TaintOperations::propagateThroughCall(
    AbductiveDomain &astate, const llvm::CallInst *call,
    const std::vector<AbstractValue> &args, AbstractValue ret_val) {
  const TaintConfig &config = getConfig();
  const llvm::Function *callee = call->getCalledFunction();

  if (!callee) {
    for (AbstractValue arg : args) {
      if (astate.getTaintDomain().has(astate.getCanonical(arg))) {
        propagate(astate, arg, ret_val, call);
      }
    }
    return;
  }

  std::vector<TaintKind> source_kinds = config.isSource(callee);
  if (!source_kinds.empty()) {
    taint(astate, ret_val, source_kinds, call);
    return;
  }

  std::vector<TaintKind> sink_kinds;
  if (config.isSink(callee, sink_kinds)) {
    for (size_t i = 0; i < args.size() && i < call->arg_size(); ++i) {
      AbstractValue arg_av = astate.getCanonical(args[i]);
      if (!checkSink(astate, arg_av, callee->getName().str(), call) &&
          astate.isInPre(arg_av)) {
        Trace t;
        t.addEvent(call, "Taint sink: " + callee->getName().str());
        astate.abduceMustNotBeTainted(
            arg_av, MustNotBeTaintedSink(callee->getName().str(), sink_kinds,
                                         call, std::move(t),
                                         astate.nextTimestamp()));
      }
    }
  }

  std::vector<TaintKind> sanitizer_kinds = config.isSanitizer(callee);
  if (!sanitizer_kinds.empty() && !args.empty()) {
    for (const TaintKind &kind : sanitizer_kinds) {
      sanitize(astate, args[0], kind, call);
      astate.addTaintSanitized(
          args[0], SanitizerRecord{kind, call, astate.nextTimestamp()});
    }
    return;
  }

  if (config.isPropagator(callee)) {
    std::vector<AbstractValue> pre_sources;
    for (AbstractValue arg : args) {
      AbstractValue c = astate.getCanonical(arg);
      if (astate.getTaintDomain().has(c)) {
        propagate(astate, c, ret_val, call);
      } else if (astate.isInPre(c)) {
        pre_sources.push_back(c);
      }
    }
    if (!pre_sources.empty()) {
      astate.addPropagateTaintFrom(
          ret_val, PropagateTaintFromInfo{std::move(pre_sources),
                                          callee->getName().str()});
    }
    return;
  }

  for (AbstractValue arg : args) {
    if (astate.getTaintDomain().has(astate.getCanonical(arg))) {
      propagate(astate, arg, ret_val, call);
    }
  }
}

} // namespace pulse
