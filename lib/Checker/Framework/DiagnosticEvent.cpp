#include "Checker/Framework/DiagnosticEvent.h"

#include "Analysis/DebugInfo/DebugInfoAnalysis.h"
#include "Utils/LLVM/StringUtils.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Instructions.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

bool ReportDecorator::mergeActionDefault(
    std::vector<DiagnosticEvent *> &merge_events,
    DiagnosticLocation &dbg_loc_for_merged_event) {
  if (merge_events.size() <= 1)
    return false;

  int dbg_line = merge_events[0]->getDbgLine();
  StringRef dbg_file = merge_events[0]->getDbgFileName();
  int event_count = merge_events.size();
  Value *IR_source_last = nullptr;
  for (int i = 0; i < event_count; i++) {
    DiagnosticEvent *e = merge_events[i];
    if (dbg_line != e->getDbgLine() || dbg_file != e->getDbgFileName()) {
      return false;
    }

    IR_source_last = e->getIRSource();
  }

  dbg_loc_for_merged_event.reset(IR_source_last, dbg_file, dbg_line);
  return true;
}
} // namespace reporting
} // namespace lotus

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

using namespace std;

DiagnosticLocation::DiagnosticLocation() { reset(); }
DiagnosticLocation::DiagnosticLocation(Value *value, StringRef file, int line) {
  reset(value, file, line);
}
DiagnosticLocation::DiagnosticLocation(Value *value, DebugInfoAnalysis &debug) {
  reset(value, debug);
}
DiagnosticLocation::DiagnosticLocation(const DiagnosticLocation &other) {
  reset(other);
}
DiagnosticLocation::DiagnosticLocation(const DiagnosticEvent &other) {
  reset(other.getIRSource(), other.getDbgFileName(), other.getDbgLine());
  binary_addr = other.getBinaryAddress();
}
DiagnosticLocation::~DiagnosticLocation() = default;
int DiagnosticLocation::getDbgLine() const { return src_line; }
StringRef DiagnosticLocation::getDbgFileName() const { return src_file; }
Value *DiagnosticLocation::getIRSource() const { return inst; }
uint64_t DiagnosticLocation::getBinaryAddress() const { return binary_addr; }
void DiagnosticLocation::setIRSource(Value *value) { inst = value; }
void DiagnosticLocation::setDbgFileName(StringRef file) {
  src_file = file.str();
}
void DiagnosticLocation::setDbgLine(int line) { src_line = line; }
void DiagnosticLocation::reset() {
  static_cast<BugDiagStep &>(*this) = BugDiagStep{};
  src_line = -1;
}
void DiagnosticLocation::reset(Value *value, StringRef file, int line) {
  inst = value;
  src_file = file.str();
  src_line = line;
}
void DiagnosticLocation::reset(Value *value, DebugInfoAnalysis &debug) {
  reset();
  inst = value;
  if (value) {
    populateDebugInfo(debug);
    src_file = debug.getSourceFile(value);
    src_line = debug.getSourceLine(value);
  }
}
void DiagnosticLocation::reset(const DiagnosticLocation &other) {
  static_cast<BugDiagStep &>(*this) = static_cast<const BugDiagStep &>(other);
}

//=============================DecorationEventDescription
// Impl===========================================

DiagnosticText::DiagnosticText(const ReportDecorator::event_ID_t &event_ID)
    : event_ID(event_ID), data("") {}

DiagnosticText::DiagnosticText(const ReportDecorator::event_ID_t &event_ID,
                               const std::string &data)
    : event_ID(event_ID), data(data) {}

DiagnosticText::DiagnosticText(const DiagnosticText &other)
    : event_ID(other.event_ID), data(other.data) {

  // Deep copy
  // since each report is not large, this is acceptable
  for (int i = 0; i < other.getNumArgs(); i++) {
    event_args.push_back(other.event_args[i]
                             ? new DiagnosticText(*other.event_args[i])
                             : nullptr);
  }
}

DiagnosticText::DiagnosticText(DiagnosticText &&other)
    : event_ID(std::move(other.event_ID)), data(std::move(other.data)),
      event_args(std::move(other.event_args)) {}

DiagnosticText::~DiagnosticText() {
  for (DiagnosticText *event_desc : event_args) {
    delete event_desc;
  }
}

ReportDecorator::event_ID_t DiagnosticText::getEventID() const {
  return event_ID;
}

int DiagnosticText::getNumArgs() const { return event_args.size(); }

string DiagnosticText::getArg(int i, FormatKind fk) const {
  DiagnosticText *arg_i = getArgEvent(i);
  return arg_i == nullptr ? "" : arg_i->makeTip(fk);
}

DiagnosticText *DiagnosticText::getArgEvent(int i) const {
  assert(i >= 0 && i < getNumArgs() &&
         "incorrect arg index queried within a report decorator event");
  return event_args[i];
}

void DiagnosticText::pushArg(const std::string &data) {
  event_args.push_back(new DiagnosticTextUnit(data));
}

void DiagnosticText::pushArg(const DiagnosticText &arg_event) {
  event_args.push_back(new DiagnosticText(arg_event));
}

static string &replace_all_distinct(string &str, const string &old_value,
                                    const string &new_value) {
  for (string::size_type pos(0); pos != string::npos;
       pos += new_value.length()) {
    if ((pos = str.find(old_value, pos)) != string::npos)
      str.replace(pos, old_value.length(), new_value);
    else
      break;
  }
  return str;
}

string DiagnosticText::makeTipByPattern(const std::string &pattern,
                                        FormatKind fk) const {
  if (!data.empty()) {
    return data;
  }

  if (event_ID == ReportDecorator::__LIST_EVENT_ID) {
    string ret;
    for (int i = 0; i < getNumArgs(); i++) {
      ret += getArg(i, fk);
    }
    return ret;

  } else {
    string ret = pattern;
    for (int i = 0; i < getNumArgs(); i++) {
      string formal_arg = format_str("%c%c%d%c", '$', '(', i + 1, ')');
      replace_all_distinct(ret, formal_arg, getArg(i, fk));
    }
    return ret;
  }
}

string DiagnosticText::makeDecoratorLocalTip() const {
  return makeTipByPattern(DiagnosticEventRegistry::getPattern(event_ID),
                          FormatKind::FK_DecoratorLocal);
}
string DiagnosticText::makeDecoratorInternationalTip() const {
  return makeTipByPattern(DiagnosticEventRegistry::getTipPatternForEvent(event_ID),
                          FormatKind::FK_DecoratorInternational);
}

static void decorator_string_to_HTML(string &to_change) {
  replace_all_distinct(to_change, "&", "&amp;");
  replace_all_distinct(to_change, "\"", "&quot;");
  replace_all_distinct(to_change, "<", "&lt;");
  replace_all_distinct(to_change, ">", "&gt;");

  replace_all_distinct(to_change, DECORATOR_EMPH_STR_START, "<b>");
  replace_all_distinct(to_change, DECORATOR_EMPH_STR_END, "</b>");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_START, "<i>");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_END, "</i>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_RED_START,
                       "<font color=red>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_BLUE_START,
                       "<font color=blue>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_YELLOW_START,
                       "<font color=yellow>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_GREEN_START,
                       "<font color=green>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_END, "</font>");
}

static void decorator_string_to_HTML2(string &to_change) {
  replace_all_distinct(to_change, "&", "&amp;");
  replace_all_distinct(to_change, "\"", "&quot;");
  replace_all_distinct(to_change, "<", "&lt;");
  replace_all_distinct(to_change, ">", "&gt;");

  replace_all_distinct(to_change, DECORATOR_EMPH_STR_START, "<b>");
  replace_all_distinct(to_change, DECORATOR_EMPH_STR_END, "</b>");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_START, "<i>");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_END, "</i>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_RED_START,
                       "<font color=black>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_BLUE_START,
                       "<font color=black>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_YELLOW_START,
                       "<font color=black>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_GREEN_START,
                       "<font color=black>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_END, "</font>");
}

static void decorator_string_to_raw_string(string &to_change) {
  replace_all_distinct(to_change, DECORATOR_EMPH_STR_START, "");
  replace_all_distinct(to_change, DECORATOR_EMPH_STR_END, "");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_START, "");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_END, "");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_RED_START, "");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_BLUE_START, "");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_YELLOW_START, "");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_GREEN_START, "");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_END, "");
}

static void decorator_string_to_simple_formatted(string &to_change) {
  replace_all_distinct(to_change, DECORATOR_EMPH_STR_START, "<b>");
  replace_all_distinct(to_change, DECORATOR_EMPH_STR_END, "</b>");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_START, "<i>");
  replace_all_distinct(to_change, DECORATOR_ITALIC_STR_END, "</i>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_RED_START,
                       "<font color=red>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_BLUE_START,
                       "<font color=blue>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_YELLOW_START,
                       "<font color=yellow>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_GREEN_START,
                       "<font color=green>");
  replace_all_distinct(to_change, DECORATOR_COLOR_STR_END, "</font>");
}

string DiagnosticText::makeTip(FormatKind fk) const {
  switch (fk) {
  case FK_PlainTextLocal: {
    string ret = makeDecoratorLocalTip();
    decorator_string_to_raw_string(ret);
    return ret;
  }
  case FK_PlainTextInternational: {
    string ret = makeDecoratorInternationalTip();
    decorator_string_to_raw_string(ret);
    return ret;
  }
  case FK_HTML: {
    string ret = makeDecoratorInternationalTip();
    decorator_string_to_HTML(ret);
    return ret;
  }
  case FK_HTML2: {
    string ret = makeDecoratorInternationalTip();
    decorator_string_to_HTML2(ret);
    return ret;
  }
  case FK_SimpleFormatted: {
    string ret = makeDecoratorInternationalTip();
    decorator_string_to_simple_formatted(ret);
    return ret;
  }
  case FK_DecoratorLocal: {
    string ret = makeDecoratorLocalTip();
    return ret;
  }
  case FK_DecoratorInternational: {
    string ret = makeDecoratorInternationalTip();
    return ret;
  }
  }

  return "";
}

DiagnosticTextList::DiagnosticTextList()
    : DiagnosticText(ReportDecorator::__LIST_EVENT_ID) {}

DiagnosticTextUnit::DiagnosticTextUnit(const string &data)
    : DiagnosticText(ReportDecorator::__UNIT_EVENT_ID, data) {}

//======================================DiagnosticEvent
// Impl==============================================

DiagnosticEvent::DiagnosticEvent(const DiagnosticText &event_desc,
                                 const DiagnosticLocation &dbg_loc)
    : event_desc(new DiagnosticText(event_desc)), dbg_loc(dbg_loc) {}

DiagnosticEvent::~DiagnosticEvent() { delete event_desc; }

ReportDecorator::event_ID_t DiagnosticEvent::getEventID() const {
  return event_desc->getEventID();
}

int DiagnosticEvent::getNumArgs() const { return event_desc->getNumArgs(); }

string DiagnosticEvent::getArg(int i) const { return event_desc->getArg(i); }

DiagnosticText *DiagnosticEvent::getArgEvent(int i) const {
  return event_desc->getArgEvent(i);
}

int DiagnosticEvent::getDbgLine() const { return dbg_loc.getDbgLine(); }

StringRef DiagnosticEvent::getDbgFileName() const {
  return dbg_loc.getDbgFileName();
}

Value *DiagnosticEvent::getIRSource() const { return dbg_loc.getIRSource(); }

uint64_t DiagnosticEvent::getBinaryAddress() const {
  return dbg_loc.getBinaryAddress();
}

string DiagnosticEvent::makeTip(DiagnosticText::FormatKind fk) const {
  return event_desc->makeTip(fk);
}

bool DiagnosticEvent::isTipStartingWithLowerCase() const {
  string tip = makeTip(DiagnosticText::FormatKind::FK_DecoratorInternational);
  if (!tip.empty() && tip[0] >= 'a' && tip[0] <= 'z') {
    return true;
  }

  return false;
}

void DiagnosticEvent::appendBefore(const DiagnosticText &to_append) {
  // Here event_desc/cur_desc is directly pushed as an arg of the new
  // event_desc,
  //    and shall be deleted in the destructor of DiagnosticText
  DiagnosticText *cur_desc = event_desc;
  event_desc = new DiagnosticTextList();
  event_desc->pushArg(to_append);
  event_desc->event_args.push_back(cur_desc);
}

void DiagnosticEvent::appendAfter(const DiagnosticText &to_append) {
  // Here event_desc/cur_desc is directly pushed as an arg of the new
  // event_desc,
  //    and shall be deleted in the destructor of DiagnosticText
  DiagnosticText *cur_desc = event_desc;
  event_desc = new DiagnosticTextList();
  event_desc->event_args.push_back(cur_desc);
  event_desc->pushArg(to_append);
}
} // namespace reporting
} // namespace lotus
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FormatVariadic.h>

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

DiagnosticEventTemplate::DiagnosticEventTemplate(int num_args,
                                                 const std::string &pattern,
                                                 const std::string &description,
                                                 const std::string &translation)
    : num_args(num_args), pattern(pattern), description(description),
      translation(translation) {}

DiagnosticEventTemplate::~DiagnosticEventTemplate() {}

int DiagnosticEventTemplate::getNumArgs() const { return num_args; }

const std::string &DiagnosticEventTemplate::getPattern() const {
  return pattern;
}

const std::string &DiagnosticEventTemplate::getDescription() const {
  return description;
}

const std::string &DiagnosticEventTemplate::getTranslation() const {
  return translation;
}

void DiagnosticEventTemplate::setTranslation(
    const std::string &new_translation) {
  translation = new_translation;
}

void DiagnosticEventTemplate::dump(raw_ostream &o) const {
  o << "(" << num_args << ") \t (" << getPattern() << ") \t )"
    << getDescription() << ")\n";
}

DiagnosticEventRegistry::EventSet DiagnosticEventRegistry::events;

// We use constructor to collect decorator events : Constructors for global
// variables shall be executed before main function The collected events are
// used to output translation models for support different languages
//
// One issue is that the initialization for global variables has no orders,
//      DiagnosticEventRegistry::events may be initialized after the
//      constructor, and thus, we must NOT directly collect event infos into
//      DiagnosticEventRegistry::events Luckily, static variable in a function
//      shall be initialized in the first calling of the function. Therefore, we
//      first collect the event info into a static field in the constructor, and
//      update to DiagnosticEventRegistry::events when main function is
//      executed (by calling the function
//      DiagnosticEventRegistry::finishRegistration() in the main function)
// We use a finished_mark to ensure all event registrations are successfully
// collected before main function is executed.
DiagnosticEventRegistry::DiagnosticEventRegistry(
    const ReportDecorator::event_ID_t &event_id, int num_args,
    const std::string &pattern, const std::string &description, bool finished) {
  static bool finished_mark = false;
  static std::unordered_map<ReportDecorator::event_ID_t,
                            DiagnosticEventTemplate *>
      _events;

  if (finished_mark && finished)
    return;
  if (finished_mark) {
    outs() << event_id << " registration failed!!!\n";
    assert(
        false &&
        "cannot register event locally, please make the registration global");
  }

  if (finished) {
    for (auto &iter : _events) {
      events.data[iter.first] = iter.second;
    }
    finished_mark = true;
    return;
  }

  if (_events.count(event_id)) {
    errs() << "ERROR: multiple registration of event ID " << event_id
           << "!!!\n";
    llvm_unreachable("Event multiple registered!");
  }

  _events[event_id] =
      new DiagnosticEventTemplate(num_args, pattern, description);
}

DiagnosticEventRegistry::~DiagnosticEventRegistry() {}

void DiagnosticEventRegistry::finishRegistration() {
  DiagnosticEventRegistry finish(ReportDecorator::__UNIT_EVENT_ID,
                                 ARG_NUM_ERROR, "", "", true);
}

bool DiagnosticEventRegistry::isEventExist(
    const ReportDecorator::event_ID_t &event_id) {
  return (events.data.count(event_id) != 0);
}

DiagnosticEventRegistry::EventSet::~EventSet() {
  for (auto iter : data) {
    delete iter.second;
  }
}

int DiagnosticEventRegistry::getNumArgs(
    const ReportDecorator::event_ID_t &event_id) {
  if (isEventExist(event_id)) {
    if (auto *item = events.data[event_id]) {
      return item->getNumArgs();
    }
  }

  return ARG_NUM_ERROR;
}

const std::string DiagnosticEventRegistry::getPattern(
    const ReportDecorator::event_ID_t &event_id) {
  if (isEventExist(event_id)) {
    if (auto *item = events.data[event_id]) {
      return item->getPattern();
    }
  }

  return "";
}

const std::string DiagnosticEventRegistry::getDescription(
    const ReportDecorator::event_ID_t &event_id) {
  if (isEventExist(event_id)) {
    if (auto *item = events.data[event_id]) {
      return item->getDescription();
    }
  }

  return "";
}

const std::string DiagnosticEventRegistry::getTranslation(
    const ReportDecorator::event_ID_t &event_id) {
  if (isEventExist(event_id)) {
    if (auto *item = events.data[event_id]) {
      return item->getTranslation();
    }
  }

  return "";
}

const std::string DiagnosticEventRegistry::getTipPatternForEvent(
    const ReportDecorator::event_ID_t &event_id) {
  const std::string &translation = getTranslation(event_id);
  const std::string &pattern = getPattern(event_id);
  if (pattern.empty() || !translation.empty()) {
    return translation;
  } else {
    return pattern;
  }
}

void DiagnosticEventRegistry::dump(raw_ostream &o) {
  for (auto iter : events.data) {
    auto &event_id = iter.first;
    auto &event_item = iter.second;
    o << event_id << ":\t";
    if (event_item) {
      event_item->dump(o);
    } else {
      o << "Undefined\n";
    }
  }
}

void DiagnosticEventRegistry::dumpJson(const std::string &file_name) {
  llvm::json::Array array;
  for (const auto &entry : events.data) {
    auto *item = entry.second;
    array.push_back(llvm::json::Object{
        {"ID", entry.first},
        {"Description", item ? item->getDescription() : ""},
        {"NumberArgs", item ? item->getNumArgs() : ARG_NUM_ERROR},
        {"Pattern", item ? item->getPattern() : ""},
        {"Translation", !item || (!item->getPattern().empty() &&
                                  item->getTranslation().empty())
                            ? "Translation not available..."
                            : item->getTranslation()}});
  }
  std::error_code error;
  llvm::raw_fd_ostream out(file_name, error, llvm::sys::fs::OF_None);
  if (error) {
    llvm::errs() << "Cannot write event package: " << error.message() << '\n';
    return;
  }
  out << llvm::formatv("{0:2}", llvm::json::Value(llvm::json::Object{
                                    {"Registered Events", std::move(array)}}));
}

void DiagnosticEventRegistry::loadJson(const std::string &file_name) {
  auto buffer = llvm::MemoryBuffer::getFile(file_name);
  if (!buffer) {
    llvm::errs() << "Cannot read event package: " << file_name << '\n';
    return;
  }
  auto parsed = llvm::json::parse((*buffer)->getBuffer());
  if (!parsed) {
    llvm::errs() << llvm::toString(parsed.takeError()) << '\n';
    return;
  }
  auto *object = parsed->getAsObject();
  auto *array = object ? object->getArray("Registered Events") : nullptr;
  if (!array)
    return;
  for (const auto &row : *array) {
    auto *entry = row.getAsObject();
    if (!entry)
      continue;
    auto id = entry->getString("ID");
    auto description = entry->getString("Description");
    auto count = entry->getInteger("NumberArgs");
    auto pattern = entry->getString("Pattern");
    auto translation = entry->getString("Translation");
    if (!id || !description || !count || !pattern || !translation)
      continue;
    auto found = events.data.find(id->str());
    if (found != events.data.end() && found->second &&
        found->second->getPattern() == *pattern &&
        *translation != "Translation not available...")
      found->second->setTranslation(translation->str());
  }
}

} // namespace reporting
} // namespace lotus
