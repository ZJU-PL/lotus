/** @file CheckerDiagnostic.h @brief Diagnostic types and utilities for checker
 * bug reports. */
#pragma once

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/CheckerTypes.h"
#include "Checker/Framework/DiagnosticEvent.h"

#include <list>
#include <map>
#include <string>
#include <vector>

#include <llvm/IR/Value.h>

namespace lotus::checker {

struct CheckerTraceStep {
  const llvm::Value *value = nullptr;
  std::string message;
  int trace_level = 0;
};

struct CheckerDiagnostic {
  std::string checker_id;
  std::string bug_type;
  Severity severity = Severity::Medium;
  const llvm::Value *primary_value = nullptr;
  std::string message;
  std::string suggestion;
  int confidence = 80;
  std::map<std::string, std::string> metadata;
  std::vector<CheckerTraceStep> trace;

  BugReport *toBugReport(int bug_type_id) const;
};

} // namespace lotus::checker

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

class DiagnosticEvent;
class DiagnosticBuilder;

class DiagnosticTransformations {
public:
  // Merging Rule:
  // Merging result = result_event_ID + args
  // Args is generated from arg_mapping. details see
  // DiagnosticTransformations::pushMergingRule merge_action is the
  // callback function telling how to merge the events
  //     the return of merge_action means if we merge the events
  //     DiagnosticLocation& (2nd arg) shall be set as the dbg line for
  //     the merged event
  class ReportEventMergingRule {
  private:
    ReportDecorator::event_ID_t result_event_ID;
    std::vector<int> arg_mapping;
    bool (*merge_action)(std::vector<DiagnosticEvent *> &,
                         DiagnosticLocation &);

  public:
    ReportEventMergingRule(
        ReportDecorator::event_ID_t result_event_ID,
        std::vector<int> &arg_mapping,
        bool (*merge_setting)(std::vector<DiagnosticEvent *> &,
                              DiagnosticLocation &) =
            ReportDecorator::mergeActionDefault);

    bool applyRule(std::vector<DiagnosticEvent *> &to_merge,
                   DiagnosticEvent *&result,
                   DiagnosticBuilder *parent_decorator);
  };

  // Splitting Rule:
  class ReportEventSplittingRule {
  private:
    // Split the first arg in to a kvec of Events and stored in the second arg
    // parent_decorator is the parent decorator report when the transformation
    // is made, used for creating new events
    bool (*split_action)(DiagnosticEvent *, std::vector<DiagnosticEvent *> &,
                         DiagnosticBuilder *);

  public:
    ReportEventSplittingRule(bool (*split_setting)(
        DiagnosticEvent *, std::vector<DiagnosticEvent *> &,
        DiagnosticBuilder *));

    // using "split_action" to split event "to_split", store the result to the
    // kvec "result". parent_decorator is the parent decorator report when the
    // transformation is made, used for creating new events
    bool applyRule(DiagnosticEvent *to_split,
                   std::vector<DiagnosticEvent *> &result,
                   DiagnosticBuilder *parent_decorator);
  };

private:
  // Merging rules
  // Given the first event_ID_t followed by the second event_ID_t, we apply the
  // ReportStepMergingRule
  std::unordered_map<
      ReportDecorator::event_ID_t,
      std::unordered_map<ReportDecorator::event_ID_t, ReportEventMergingRule *>>
      report_step_merging_rules;

  // Merging rules
  // Given the event_ID_t, we apply the ReportStepSplittingRule
  std::unordered_map<ReportDecorator::event_ID_t, ReportEventSplittingRule *>
      report_step_spliting_rules;

public:
  DiagnosticTransformations();
  virtual ~DiagnosticTransformations();

  // Add merging rule : e1_ID + e2_ID => result_event_ID (with arg_mapping using
  // function merge_action)
  //
  // Specially for arg_mapping, we accept the following format
  // The n'th arg in args for the resultant event is the (arg_mapping[2n+1])'th
  // argument for the (arg_mapping[2n])'th event input, E.g. 1,0,0,3,0,2 means
  // that the there are 3 args in the merged event,
  //     the first arg is the result is the arg of index 0 of the input event of
  //     index 1 the second arg is the result is the arg of index 3 of the input
  //     event of index 0 the third arg is the result is the arg of index 2 of
  //     the input event of index 0
  void pushMergingRule(ReportDecorator::event_ID_t e1_ID,
                       ReportDecorator::event_ID_t e2_ID,
                       ReportDecorator::event_ID_t result_event_ID,
                       std::vector<int> &arg_mapping,
                       bool (*merge_action)(std::vector<DiagnosticEvent *> &,
                                            DiagnosticLocation &) =
                           ReportDecorator::mergeActionDefault);

  // Add splitting rule, each event of event_ID shall be split using the
  // function split_action
  void pushSplittingRule(ReportDecorator::event_ID_t event_ID,
                         bool (*split_action)(DiagnosticEvent *,
                                              std::vector<DiagnosticEvent *> &,
                                              DiagnosticBuilder *));

  // Delete merging rule that we do not merge events with ID e1_ID and e2_ID
  void deleteMergingRule(ReportDecorator::event_ID_t e1_ID,
                         ReportDecorator::event_ID_t e2_ID);
  // Delete splitting rule that event of e1_ID shall not be split
  void deleteSplittingRule(ReportDecorator::event_ID_t e1_ID);

  // Get the merging rule merging e1_ID and e2_ID. If the rule does not exist,
  // we return null
  ReportEventMergingRule *getMergingRule(ReportDecorator::event_ID_t &e1_ID,
                                         ReportDecorator::event_ID_t &e2_ID);
  // Get the splitting rule splitting e_ID. If the rule does not exist, we
  // return null
  ReportEventSplittingRule *getSplittingRule(ReportDecorator::event_ID_t &e_ID);
};

} // namespace reporting
} // namespace lotus

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

using namespace llvm;

class DiagnosticEvent;
class DiagnosticBuilder;
class DiagnosticTransformations;

// Basic operations on report decorator, such as insert/delete event, event look
// up, revision, etc. Note : All methods in this class are general operations on
// events proceeded in the decorator,
//        not suggested for overriding any of the methods in this class
//
// A basic clean decorator (that does not apply default processing of bug
// reports) can directly use or extend this class Other classes such as
// DefaultReportDecorator/LLVMValueReportDecorator
//            implement some default procedure for general report events, which
//            can be extended/overrided on demand.
class DiagnosticBuilder {
public:
  typedef std::list<DiagnosticEvent *>::iterator iterator;
  static DebugInfoAnalysis *DIA;

private:
  DiagnosticTransformations *transformation_rules;

  std::list<DiagnosticEvent *> events;

  std::vector<DiagnosticEvent *> decorator_events_cache;

protected:
private:
  // Try to merge events *iter and its next event, and update the corresponding
  // structure if merging succeeds return the iterator pointing to the resultant
  // merged event if merging succeeds, otherwise, return the iterator passed to
  // the function
  iterator tryMerge(iterator iter);

  // Try to split event *iter, and update the corresponding structure if
  // splitting succeeds return the iterator pointing to the NEXT event of the
  // last resultant split event if splitting succeeds, otherwise, return the
  // iterator passed to the function
  iterator trySplit(iterator iter);

  // Apply Merging and Splitting rules
  // Merging rules first, the splitting if conflict exists
  void finalize();

  // Remove the current last event if it is the same as the second last event
  void clearLastEventRedundency();

public:
  DiagnosticBuilder(DiagnosticTransformations *transformation_rules = nullptr);
  virtual ~DiagnosticBuilder();

  // Make bug report from the report decorator, invoked by checker writer or bug
  // report manager
  void makeReport(BugReport *);

  friend class DiagnosticTransformations;

protected:
  // ===================================Methods for General
  // Users============================================

  // Append some description info in the current last event, such as the further
  // description on some key values These two functions do not add new events,
  // but just append the information before/after the existing last event Note :
  // These two functions shall do nothing if the decorator is empty, say, no
  // events exist
  void appendLastStepDescription(const DiagnosticText &desc);
  void addBeforeLastStepDescription(const DiagnosticText &desc);

  // Collecting info:

  // get the current first/last event
  DiagnosticEvent *front() const;
  DiagnosticEvent *back() const;
  // get the current number of events
  int size() const;
  // return true if there are currently no events in the report decorator
  bool empty() const;

  // pushBackEvent : general users can use
  // pushDecoratorEvent/pushDecoratorEventwithNoArg macro in
  // DiagnosticEventRegistry.h
  //             to push back registered events

  // createEvent : general users can use ReportDecorator::getXXXEvent function
  // to create a corresponding event with ID XXX
  //            the getXXXEvent function is defined when XXX event is registered
  //            by the macro RegisterDecoratorEvent Details can be seen in
  //            DiagnosticEventRegistry.h

  // ===================================Methods for Advanced
  // Users============================================

  // Create new decorator event
  // Note : the created event is bounded to the parent report decorator, and can
  // be manually pushed into the decorator when required
  //        without any further action
  DiagnosticEvent *new_decorator_event(const DiagnosticText &event_desc,
                                       const DiagnosticLocation &dbg_loc);

  // operations on the events within the report decorator, semantic similar to
  // std::list
  iterator begin();
  iterator end();
  iterator pushFront(const DiagnosticText &event_desc,
                     const DiagnosticLocation &dbg_loc);
  iterator pushFront(DiagnosticEvent *);
  iterator pushEnd(const DiagnosticText &event_desc,
                   const DiagnosticLocation &dbg_loc);
  iterator pushEnd(DiagnosticEvent *);
  iterator insertBefore(iterator, const DiagnosticText &event_desc,
                        const DiagnosticLocation &dbg_loc);
  iterator insertBefore(iterator, DiagnosticEvent *);
  iterator insertAfter(iterator, const DiagnosticText &event_desc,
                       const DiagnosticLocation &dbg_loc);
  iterator insertAfter(iterator, DiagnosticEvent *);
  iterator erase(iterator);
  void remove(DiagnosticEvent *);
  void removeFront();
  void removeEnd();
};

} // namespace reporting
} // namespace lotus
