#include "Checker/Framework/CheckerDiagnostic.h"

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Checker/Framework/ReportOptions.h"

#include <cctype>

namespace lotus::checker {

BugReport *CheckerDiagnostic::toBugReport(int bug_type_id) const {
  auto *report = new BugReport(bug_type_id);
  if (!trace.empty()) {
    for (const auto &step : trace) {
      report->append_step(const_cast<llvm::Value *>(step.value), step.message,
                          step.trace_level);
    }
  } else {
    report->append_step(const_cast<llvm::Value *>(primary_value), message);
  }

  report->set_conf_score(confidence);
  if (!suggestion.empty()) {
    report->set_suggestion(suggestion);
  }

  report->add_metadata("checker_id", checker_id);
  report->add_metadata("severity", toString(severity));
  for (const auto &entry : metadata) {
    report->add_metadata(entry.first, entry.second);
  }
  return report;
}

} // namespace lotus::checker

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

using namespace std;

DiagnosticTransformations::DiagnosticTransformations() {}

DiagnosticTransformations::~DiagnosticTransformations() {}

DiagnosticTransformations::ReportEventMergingRule::ReportEventMergingRule(
    ReportDecorator::event_ID_t result_event_ID, std::vector<int> &arg_mapping,
    bool (*merge_setting)(std::vector<DiagnosticEvent *> &,
                          DiagnosticLocation &))
    : result_event_ID(result_event_ID), arg_mapping(arg_mapping),
      merge_action(merge_setting) {}

bool DiagnosticTransformations::ReportEventMergingRule::applyRule(
    std::vector<DiagnosticEvent *> &to_merge, DiagnosticEvent *&result,
    DiagnosticBuilder *parent_decorator) {

  DiagnosticLocation loc;
  if (merge_action(to_merge, loc) && arg_mapping.size() % 2 == 0) {
    DiagnosticText event_desc(result_event_ID);
    for (size_t i = 0; i < arg_mapping.size(); i += 2) {
      int source_event_idx = arg_mapping[i];
      int arg_idx = arg_mapping[i + 1];
      if (source_event_idx < 0 ||
          static_cast<size_t>(source_event_idx) >= to_merge.size()) {
        return false;
      }

      DiagnosticEvent *choosed_source_event = to_merge[source_event_idx];
      int choosed_source_event_arg_size = choosed_source_event->getNumArgs();
      if (choosed_source_event_arg_size >= arg_idx || arg_idx < 0) {
        return false;
      }

      string arg = choosed_source_event->getArg(arg_idx);
      event_desc.pushArg(arg);
    }
    result = parent_decorator->new_decorator_event(event_desc, loc);
    return true;
  }

  return false;
}

DiagnosticTransformations::ReportEventSplittingRule::ReportEventSplittingRule(
    bool (*split_setting)(DiagnosticEvent *, std::vector<DiagnosticEvent *> &,
                          DiagnosticBuilder *))
    : split_action(split_setting) {}

bool DiagnosticTransformations::ReportEventSplittingRule::applyRule(
    DiagnosticEvent *to_split, std::vector<DiagnosticEvent *> &result,
    DiagnosticBuilder *parent_decorator) {
  return split_action(to_split, result, parent_decorator);
}

void DiagnosticTransformations::pushMergingRule(
    ReportDecorator::event_ID_t e1_ID, ReportDecorator::event_ID_t e2_ID,
    ReportDecorator::event_ID_t result_event_ID, std::vector<int> &arg_mapping,
    bool (*merge_action)(std::vector<DiagnosticEvent *> &,
                         DiagnosticLocation &)) {

  ReportEventMergingRule *rule =
      new ReportEventMergingRule(result_event_ID, arg_mapping, merge_action);
  auto &e1_result = report_step_merging_rules[e1_ID];

  if (e1_result.count(e2_ID)) {
    delete e1_result[e2_ID];
  }

  e1_result[e2_ID] = rule;
}

void DiagnosticTransformations::pushSplittingRule(
    ReportDecorator::event_ID_t event_ID,
    bool (*split_action)(DiagnosticEvent *, std::vector<DiagnosticEvent *> &,
                         DiagnosticBuilder *)) {

  ReportEventSplittingRule *rule = new ReportEventSplittingRule(split_action);
  if (report_step_spliting_rules.count(event_ID)) {
    delete report_step_spliting_rules[event_ID];
  }

  report_step_spliting_rules[event_ID] = rule;
}

void DiagnosticTransformations::deleteMergingRule(
    ReportDecorator::event_ID_t e1_ID, ReportDecorator::event_ID_t e2_ID) {
  auto &e1_result = report_step_merging_rules[e1_ID];

  if (e1_result.count(e2_ID)) {
    delete e1_result[e2_ID];
    e1_result.erase(e2_ID);
  }
}

void DiagnosticTransformations::deleteSplittingRule(
    ReportDecorator::event_ID_t event_ID) {
  if (report_step_spliting_rules.count(event_ID)) {
    delete report_step_spliting_rules[event_ID];
    report_step_spliting_rules.erase(event_ID);
  }
}

DiagnosticTransformations::ReportEventMergingRule *
DiagnosticTransformations::getMergingRule(ReportDecorator::event_ID_t &e1_ID,
                                          ReportDecorator::event_ID_t &e2_ID) {
  if (report_step_merging_rules.count(e1_ID)) {
    auto &cur_merging_rules = report_step_merging_rules[e1_ID];
    if (cur_merging_rules.count(e2_ID)) {
      return cur_merging_rules[e2_ID];
    }
  }

  return nullptr;
}

DiagnosticTransformations::ReportEventSplittingRule *
DiagnosticTransformations::getSplittingRule(ReportDecorator::event_ID_t &e_ID) {
  if (report_step_spliting_rules.count(e_ID)) {
    return report_step_spliting_rules[e_ID];
  }

  return nullptr;
}
} // namespace reporting
} // namespace lotus

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

using namespace std;

DebugInfoAnalysis *DiagnosticBuilder::DIA = nullptr;

DiagnosticBuilder::DiagnosticBuilder(
    DiagnosticTransformations *transformation_rules)
    : transformation_rules(transformation_rules) {
  // Don't need to pass away the singleton passes
}

DiagnosticBuilder::~DiagnosticBuilder() {
  for (DiagnosticEvent *e : decorator_events_cache) {
    delete e;
  }
}

DiagnosticBuilder::iterator DiagnosticBuilder::tryMerge(iterator iter) {
  if (!transformation_rules) {
    // no rule assigned
    return iter;
  }

  iterator next = iter;
  next++;
  if (next == end())
    return iter;

  DiagnosticEvent *e_cur = *iter;
  DiagnosticEvent *e_next = *next;

  ReportDecorator::event_ID_t ID_cur = e_cur->getEventID();
  ReportDecorator::event_ID_t ID_next = e_next->getEventID();

  auto *merging_rule = transformation_rules->getMergingRule(ID_cur, ID_next);
  if (merging_rule) {
    // Merging rule exists

    std::vector<DiagnosticEvent *> to_merge;
    to_merge.reserve(2);
    to_merge.push_back(e_cur);
    to_merge.push_back(e_next);
    DiagnosticEvent *merge_result = nullptr;
    bool merged = merging_rule->applyRule(to_merge, merge_result, this);
    if (merged) {
      next = erase(iter);
      next = insertAfter(next, merge_result);
      next--;

      // next->next is the inserted merging result event
      // the return iter of erase(next) is next->next, which is the iterator
      // pointing to the merging result event
      return erase(next);
    }
  }
  return iter;
}

DiagnosticBuilder::iterator DiagnosticBuilder::trySplit(iterator iter) {
  if (!transformation_rules) {
    // no rule assigned
    return iter;
  }

  DiagnosticEvent *e = *iter;
  ReportDecorator::event_ID_t e_ID = e->getEventID();

  auto *splitting_rule = transformation_rules->getSplittingRule(e_ID);
  if (splitting_rule) {
    // Splitting rule exists
    std::vector<DiagnosticEvent *> splitting_result;
    bool split = splitting_rule->applyRule(e, splitting_result, this);
    if (split) {
      int result_count = splitting_result.size();
      for (int i = 0; i < result_count; i++) {
        iter = insertBefore(iter, splitting_result[i]);
        iter++;
      }

      // the return iter of erase(iter) is iter->next,
      // which is the iterator pointing to the NEXT event of the last resultant
      // split event
      return erase(iter);
    }
  }

  return iter;
}

void DiagnosticBuilder::finalize() {
  // Merging first
  for (iterator iter = begin(); iter != end();) {
    DiagnosticEvent *before_merge = *iter;
    iter = tryMerge(iter);
    DiagnosticEvent *after_merge = *iter;

    if (before_merge == after_merge) {
      // merge failed, we can move on
      iter++;
      continue;
    }

    // merge succeeds, we first try merge the previous nodes
    // and then we try again merging the merged cur-step to the following step
    while (before_merge != after_merge && iter != begin()) {
      iter--;
      before_merge = *iter;
      iter = tryMerge(iter);
      after_merge = *iter;
    }
  }

  for (iterator iter = begin(); iter != end(); iter++) {
    iter = trySplit(iter);
  }
}

void DiagnosticBuilder::makeReport(BugReport *report) {
  finalize();

  if (report) {
    for (DiagnosticEvent *e : *this) {
      std::string tip;
      if (report_options::TipFormat == "html") {
        tip = e->makeTip(DiagnosticText::FK_HTML);
      } else if (report_options::TipFormat == "plain-text") {
        tip = e->makeTip(DiagnosticText::FK_PlainTextInternational);
      } else if (report_options::TipFormat == "pp-formatted-string") {
        tip = e->makeTip(DiagnosticText::FK_DecoratorInternational);
      } else if (report_options::TipFormat == "pp-event") {
        // TODO: pp-event is not supported yet
        tip = e->makeTip(DiagnosticText::FK_HTML2);
      } else {
        tip = e->makeTip(DiagnosticText::FK_HTML2);
      }

      if (e->isTipStartingWithLowerCase() && !tip.empty() &&
          std::islower(static_cast<unsigned char>(tip[0])))
        tip[0] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(tip[0])));
      report->append_step(e->getIRSource(), tip);
      auto *step = report->get_steps().back();
      step->src_file = e->getDbgFileName().str();
      step->src_line = e->getDbgLine();
      step->binary_addr = e->getBinaryAddress();
    }
  }
}

DiagnosticEvent *
DiagnosticBuilder::new_decorator_event(const DiagnosticText &event_desc,
                                       const DiagnosticLocation &dbg_loc) {
  DiagnosticEvent *new_event = new DiagnosticEvent(event_desc, dbg_loc);
  decorator_events_cache.push_back(new_event);
  return new_event;
}

int DiagnosticBuilder::size() const { return events.size(); }

bool DiagnosticBuilder::empty() const { return events.empty(); }

DiagnosticEvent *DiagnosticBuilder::front() const {
  return empty() ? nullptr : events.front();
}
DiagnosticEvent *DiagnosticBuilder::back() const {
  return empty() ? nullptr : events.back();
}

DiagnosticBuilder::iterator DiagnosticBuilder::begin() {
  return events.begin();
}

DiagnosticBuilder::iterator DiagnosticBuilder::end() { return events.end(); }

DiagnosticBuilder::iterator
DiagnosticBuilder::pushFront(const DiagnosticText &event_desc,
                             const DiagnosticLocation &dbg_loc) {
  DiagnosticEvent *e = new_decorator_event(event_desc, dbg_loc);
  return pushFront(e);
}

DiagnosticBuilder::iterator DiagnosticBuilder::pushFront(DiagnosticEvent *e) {
  events.push_front(e);
  return begin();
}

DiagnosticBuilder::iterator
DiagnosticBuilder::pushEnd(const DiagnosticText &event_desc,
                           const DiagnosticLocation &dbg_loc) {
  DiagnosticEvent *e = new_decorator_event(event_desc, dbg_loc);
  return pushEnd(e);
}

// Test if e1 and e2 are same events
static bool is_same_event(DiagnosticEvent *e1, DiagnosticEvent *e2) {
  if ((e1 == nullptr) || (e2 == nullptr)) {
    return false;
  }

  if (e1->getDbgLine() != e2->getDbgLine()) {
    return false;
  }

  if (e1->getIRSource() != e2->getIRSource()) {
    return false;
  }

  if (e1->getDbgFileName() != e2->getDbgFileName()) {
    return false;
  }

  if (e1->makeTip() != e2->makeTip()) {
    return false;
  }

  return true;
}

DiagnosticBuilder::iterator DiagnosticBuilder::pushEnd(DiagnosticEvent *e) {
  // We apply duplication test
  if (!is_same_event(e, back()))
    events.push_back(e);

  iterator ret = end();
  ret--;
  return ret;
}

DiagnosticBuilder::iterator
DiagnosticBuilder::insertBefore(iterator iter, const DiagnosticText &event_desc,
                                const DiagnosticLocation &dbg_loc) {
  DiagnosticEvent *e = new_decorator_event(event_desc, dbg_loc);
  return insertBefore(iter, e);
}

DiagnosticBuilder::iterator
DiagnosticBuilder::insertBefore(iterator iter, DiagnosticEvent *e) {
  return events.insert(iter, e);
}

DiagnosticBuilder::iterator
DiagnosticBuilder::insertAfter(iterator iter, const DiagnosticText &event_desc,
                               const DiagnosticLocation &dbg_loc) {
  DiagnosticEvent *e = new_decorator_event(event_desc, dbg_loc);
  return insertAfter(iter, e);
}

DiagnosticBuilder::iterator DiagnosticBuilder::insertAfter(iterator iter,
                                                           DiagnosticEvent *e) {
  iterator next = iter;
  next++;
  if (next == end()) {
    return pushEnd(e);
  } else {
    return insertBefore(next, e);
  }
}

DiagnosticBuilder::iterator DiagnosticBuilder::erase(iterator iter) {
  return events.erase(iter);
}

void DiagnosticBuilder::remove(DiagnosticEvent *e) { events.remove(e); }

void DiagnosticBuilder::removeFront() { events.pop_front(); }

void DiagnosticBuilder::removeEnd() { events.pop_back(); }

void DiagnosticBuilder::clearLastEventRedundency() {
  if (size() >= 2) {
    iterator iter_last = end();
    iter_last--;

    iterator iter_second_last = iter_last;
    iter_second_last--;

    DiagnosticEvent *last = *iter_last;
    DiagnosticEvent *second_last = *iter_second_last;

    if (is_same_event(last, second_last)) {
      removeEnd();
    }
  }
}

void DiagnosticBuilder::appendLastStepDescription(const DiagnosticText &desc) {
  if (!empty()) {
    back()->appendAfter(desc);
    clearLastEventRedundency();
  }
}

void DiagnosticBuilder::addBeforeLastStepDescription(
    const DiagnosticText &desc) {
  if (!empty()) {
    back()->appendBefore(desc);
    clearLastEventRedundency();
  }
}
} // namespace reporting
} // namespace lotus
