#pragma once
#include "Checker/Framework/BugReport.h"
#include "Utils/LLVM/StringUtils.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Support/raw_ostream.h>
class DebugInfoAnalysis;

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

using namespace llvm;

class DiagnosticEvent;
class DiagnosticLocation;

#define DECORATOR_EMPH_STR_START "@B@"
#define DECORATOR_EMPH_STR_END "@/B@"
#define DECORATOR_ITALIC_STR_START "@I@"
#define DECORATOR_ITALIC_STR_END "@/I@"
#define DECORATOR_COLOR_STR_RED_START "@FC_RED@"
#define DECORATOR_COLOR_STR_BLUE_START "@FC_BLUE@"
#define DECORATOR_COLOR_STR_YELLOW_START "@FC_YELLOW@"
#define DECORATOR_COLOR_STR_GREEN_START "@FC_GREEN@"
#define DECORATOR_COLOR_STR_END "@/FC@"

namespace ReportDecorator {

// Event ID Type, currently, we use string as
typedef std::string event_ID_t;

// Special Event ID:

// Unit event, representing a string requiring no translation
const event_ID_t __UNIT_EVENT_ID = "";
// List event, representing a list of events merged in one event, where the
// events appears one by one
const event_ID_t __LIST_EVENT_ID = "DECORATOR_EVENT_LIST";

// Default action merging events
// By default,
//     (1) Only events in the same line can be merged
//     (2) The debug line is set as the line for the last event
bool mergeActionDefault(std::vector<DiagnosticEvent *> &merge_events,
                        DiagnosticLocation &dbg_loc_for_merged_event);

// Markdown styles in decorator events
// Construct a markdown style emphasis of given string in decorator events
inline std::string decorator_emph_str(const std::string &str) {
  return std::string(DECORATOR_EMPH_STR_START) + str + DECORATOR_EMPH_STR_END;
}

// A markdown style italic in decorator events
inline std::string decorator_italic_str(const std::string &str) {
  return std::string(DECORATOR_ITALIC_STR_START) + str +
         DECORATOR_ITALIC_STR_END;
}

// A markdown style colored font in decorator events
// Currently we support red/green/yellow/blue
inline std::string decorator_color_red(const std::string &str) {
  return std::string(DECORATOR_COLOR_STR_RED_START) + str +
         DECORATOR_COLOR_STR_END;
}
inline std::string decorator_color_green(const std::string &str) {
  return std::string(DECORATOR_COLOR_STR_GREEN_START) + str +
         DECORATOR_COLOR_STR_END;
}
inline std::string decorator_color_yellow(const std::string &str) {
  return std::string(DECORATOR_COLOR_STR_YELLOW_START) + str +
         DECORATOR_COLOR_STR_END;
}
inline std::string decorator_color_blue(const std::string &str) {
  return std::string(DECORATOR_COLOR_STR_BLUE_START) + str +
         DECORATOR_COLOR_STR_END;
}
} // namespace ReportDecorator

} // namespace reporting
} // namespace lotus

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

using namespace llvm;

class DiagnosticEvent;

// Object describing a debug location for a debug event
class DiagnosticLocation : public BugDiagStep {
public:
  DiagnosticLocation();
  DiagnosticLocation(Value *IR_source, StringRef dbg_file_name, int dbg_line);
  DiagnosticLocation(Value *IR_source, DebugInfoAnalysis &DIA);
  DiagnosticLocation(const DiagnosticLocation &other);
  DiagnosticLocation(const DiagnosticEvent &other);

  virtual ~DiagnosticLocation();

  // Get/Set field values
  int getDbgLine() const;
  StringRef getDbgFileName() const;
  Value *getIRSource() const;
  uint64_t getBinaryAddress() const;

  void setIRSource(Value *IR_source);
  void setDbgFileName(StringRef dbg_file_name);
  void setDbgLine(int dbg_line);

  // reset using default dbg location(null IR source, empty file name line 0)
  void reset();
  // reset fields using the args
  void reset(Value *IR_source, StringRef dbg_file_name, int dbg_line);

  // reset the dbg_file_name and dbg_line according to the debug location for
  // IR_source
  void reset(Value *IR_source, DebugInfoAnalysis &DIA);

  // reset by copying another DiagnosticLocation
  void reset(const DiagnosticLocation &other);
};

// Object describing an event description, i.e. a debug tip
// Special kinds of DiagnosticText:
//      DiagnosticTextList:
//          A list of sub-descriptions, sub-descriptions appears one by one
//          according to the event_args sequence
//      DiagnosticTextUnit:
//          String description requiring no translation in different languages,
//          such as variable names
class DiagnosticText {
public:
  enum FormatKind {
    FK_PlainTextLocal, // Raw string in English as the pattern in the code
    FK_PlainTextInternational, // Raw string using the language package
    FK_HTML,                   // HTML format using the language package
    FK_HTML2, // HTML format (black & white) using the language package
    FK_SimpleFormatted, // Simple formatted string using the language package
    FK_DecoratorLocal,  // Raw decorator string in English as the pattern in the
                        // code, using default decorator format specification
    FK_DecoratorInternational, // Raw decorator string using the language
                               // package, using default decorator format
                               // specification
  };

private:
  // Pattern ID: Each pattern ID represents a step type, arguments are marked as
  // $$
  ReportDecorator::event_ID_t event_ID;

  // Used as the string description for "unit" description
  // unit description means a description that requires no translation given any
  // language, such as variable names
  std::string data;

  // Pattern arguments
  std::vector<DiagnosticText *> event_args;

private:
  std::string makeTipByPattern(const std::string &pattern, FormatKind fk) const;
  std::string makeDecoratorLocalTip() const;
  std::string makeDecoratorInternationalTip() const;

protected:
  DiagnosticText(const ReportDecorator::event_ID_t &event_ID,
                 const std::string &data);

public:
  DiagnosticText(const ReportDecorator::event_ID_t &);
  DiagnosticText(const DiagnosticText &other);
  DiagnosticText(DiagnosticText &&other);
  virtual ~DiagnosticText();

  // Get field values
  ReportDecorator::event_ID_t getEventID() const;
  int getNumArgs() const;
  std::string getArg(int i, FormatKind fk = FK_PlainTextLocal) const;
  DiagnosticText *getArgEvent(int i) const;

  // Push argument for the event description
  // An argument for a description can be an event description or a string
  // Event description argument means the arg that should be translated for
  // different languages, String argument shall never be translated, such as
  // variable names
  void pushArg(const std::string &arg);
  void pushArg(const DiagnosticText &arg_event);

  // Make string tip for the event description
  virtual std::string makeTip(FormatKind fk = FK_PlainTextLocal) const;

  friend class DiagnosticEvent;
};

class DiagnosticTextList : public DiagnosticText {
public:
  DiagnosticTextList();
};

class DiagnosticTextUnit : public DiagnosticText {
public:
  DiagnosticTextUnit(const std::string &);
};

// Object describing a whole decorator event, usually representing a debug step
// A decorator event includes a DiagnosticText (tip) and a
// DiagnosticLocation (debug location)
class DiagnosticEvent {
private:
  DiagnosticText *event_desc;
  DiagnosticLocation dbg_loc;

public:
  DiagnosticEvent(const DiagnosticText &event_desc,
                  const DiagnosticLocation &dbg_loc);
  virtual ~DiagnosticEvent();

  // Append DiagnosticText to_append before/after the event_desc
  // field of the decorator event
  void appendBefore(const DiagnosticText &to_append);
  void appendAfter(const DiagnosticText &to_append);

  // Get field values (for event_desc/dbg_loc)
  ReportDecorator::event_ID_t getEventID() const;
  int getNumArgs() const;
  std::string getArg(int i) const;
  DiagnosticText *getArgEvent(int i) const;
  int getDbgLine() const;
  StringRef getDbgFileName() const;
  Value *getIRSource() const;
  uint64_t getBinaryAddress() const;

  // Make string tip for the event decorator (from event_desc)
  std::string makeTip(
      DiagnosticText::FormatKind fk = DiagnosticText::FK_PlainTextLocal) const;

  // Return true if the
  bool isTipStartingWithLowerCase() const;
};

} // namespace reporting
} // namespace lotus

// Suppress the warnings due to non-standard gcc features
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wvariadic-macros"
#endif

namespace lotus {
namespace reporting {
using namespace llvm;
using namespace std;

// Modelling a kind of decorator events
class DiagnosticEventTemplate {
private:
  // Number of event args
  int num_args;
  // Pattern of the event, using $(ARG_IDX) to represent the args
  // E.g. "$(1) is stored to $(2)" means "<The first arg> is stored to <The
  // second arg>" Note : ARG_IDX starts from 1
  const std::string pattern;
  // Description of the event
  const std::string description;
  // The translation to the targeting language
  std::string translation;

  void setTranslation(const std::string &);

public:
  DiagnosticEventTemplate(int num_args, const std::string &pattern,
                          const std::string &description,
                          const std::string &translation = "");
  virtual ~DiagnosticEventTemplate();

  int getNumArgs() const;
  const std::string &getPattern() const;
  const std::string &getDescription() const;
  const std::string &getTranslation() const;

  void dump(raw_ostream &) const;

  friend class DiagnosticEventRegistry;
};

// Decorator event registrator
// Using the constructor of the class to register events.
// Note: Objects of this class can only be global
// We strongly recommend users to use the Macros below to register events
//
// Note : Decorator event ID shall be globally unique, because this is used to
// help select event description/translation
class DiagnosticEventRegistry {
  class EventSet {
    std::unordered_map<ReportDecorator::event_ID_t, DiagnosticEventTemplate *>
        data;

  public:
    virtual ~EventSet();

    friend class DiagnosticEventRegistry;
  };

private:
  static EventSet events;

private:
  static void loadBinary(const char *buf_data);

public:
  const static int ARG_NUM_ERROR = -1;

  DiagnosticEventRegistry(const ReportDecorator::event_ID_t &, int num_args = 0,
                          const std::string &pattern = "",
                          const std::string &description = "",
                          bool finished = false);
  virtual ~DiagnosticEventRegistry();

  // Collect the registered events, usually used in the beginning of main
  // function Note : should ensure this function is called before the using of
  // the events
  static void finishRegistration();

  static int getNumArgs(const ReportDecorator::event_ID_t &event_id);
  static const std::string
  getPattern(const ReportDecorator::event_ID_t &event_id);
  static const std::string
  getDescription(const ReportDecorator::event_ID_t &event_id);
  static bool isEventExist(const ReportDecorator::event_ID_t &event_id);
  static const std::string
  getTranslation(const ReportDecorator::event_ID_t &event_id);
  static const std::string
  getTipPatternForEvent(const ReportDecorator::event_ID_t &event_id);

  // dump/load registered events, readable json file is used for translating
  // debug tips
  static void dump(raw_ostream &);
  static void dumpJson(const std::string &file_name);
  static void loadJson(const std::string &file_name);
};

// Macros for Registering Events
// Internal Macros, used for building event registration macros
#define INTERNAL_REGISTER_DECORATOR_EVENT_ID_STRING(event_id)                  \
  static const std::string event_id = #event_id

#define INTERNAL_REGISTER_DECORATOR_EVENT_ID(event_id, ArgNumber, pattern,     \
                                             description)                      \
  static DiagnosticEventRegistry __decorator_event_##event_id(                 \
      #event_id, ArgNumber, pattern, description)

#define INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(event_id)  \
  get##event_id##Event

#define DECORATOR_EVENT_DESCRIPTION_CLASS_NAME DiagnosticText

// External Macros (All Macros below), used for registering events
//
// User Manual:
//
// Use
// RegisterDecoratorEventWithXXXArgument/RegisterDecoratorEventWithXXXArguments
// Macro in global space
//       to register events with the corresponding number of args
// For each registration, we have
//       (1) A string variable event_id representing the eventID
//       (2) A creation method for the event :
//                       DiagnosticText get##event_id##Event(args...)
// All resources are in the ReportDecorator name space
// Users can type "ReportDecorator::" to list the visible registered resources
// (Especially, use RegisterVulnerabilityDescriptionEvent macro to register bug
// type description)
//
// Use pushDecoratorEvent/pushReportDecoratorEventWithNoArg in decorator
// implementation functions
//       to push end events with corresponding type
//
#define REPORT_DECORATOR_NAMESPACE ReportDecorator

#define RegisterDecoratorEventWithNoArgument(event_id, pattern, description)   \
  namespace REPORT_DECORATOR_NAMESPACE {                                       \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID_STRING(event_id);                       \
  static DECORATOR_EVENT_DESCRIPTION_CLASS_NAME                                \
  INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(event_id)() {    \
    DECORATOR_EVENT_DESCRIPTION_CLASS_NAME ret(event_id);                      \
    return ret;                                                                \
  }                                                                            \
  }                                                                            \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID(event_id, 0, pattern, description)

#define RegisterDecoratorEventWithOneArgument(event_id, pattern, description)  \
  namespace REPORT_DECORATOR_NAMESPACE {                                       \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID_STRING(event_id);                       \
  template <class T1>                                                          \
  static DECORATOR_EVENT_DESCRIPTION_CLASS_NAME                                \
  INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(event_id)(       \
      const T1 &arg1) {                                                        \
    DECORATOR_EVENT_DESCRIPTION_CLASS_NAME ret(event_id);                      \
    ret.pushArg(arg1);                                                         \
    return ret;                                                                \
  }                                                                            \
  }                                                                            \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID(event_id, 1, pattern, description)

#define RegisterDecoratorEventWithTwoArguments(event_id, pattern, description) \
  namespace REPORT_DECORATOR_NAMESPACE {                                       \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID_STRING(event_id);                       \
  template <class T1, class T2>                                                \
  static DECORATOR_EVENT_DESCRIPTION_CLASS_NAME                                \
  INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(event_id)(       \
      const T1 &arg1, const T2 &arg2) {                                        \
    DECORATOR_EVENT_DESCRIPTION_CLASS_NAME ret(event_id);                      \
    ret.pushArg(arg1);                                                         \
    ret.pushArg(arg2);                                                         \
    return ret;                                                                \
  }                                                                            \
  }                                                                            \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID(event_id, 2, pattern, description)

#define RegisterDecoratorEventWithThreeArguments(event_id, pattern,            \
                                                 description)                  \
  namespace REPORT_DECORATOR_NAMESPACE {                                       \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID_STRING(event_id);                       \
  template <class T1, class T2, class T3>                                      \
  static DECORATOR_EVENT_DESCRIPTION_CLASS_NAME                                \
  INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(event_id)(       \
      const T1 &arg1, const T2 &arg2, const T3 &arg3) {                        \
    DECORATOR_EVENT_DESCRIPTION_CLASS_NAME ret(event_id);                      \
    ret.pushArg(arg1);                                                         \
    ret.pushArg(arg2);                                                         \
    ret.pushArg(arg3);                                                         \
    return ret;                                                                \
  }                                                                            \
  }                                                                            \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID(event_id, 3, pattern, description)

#define RegisterDecoratorEventWithFourArguments(event_id, pattern,             \
                                                description)                   \
  namespace REPORT_DECORATOR_NAMESPACE {                                       \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID_STRING(event_id);                       \
  template <class T1, class T2, class T3, class T4>                            \
  static DECORATOR_EVENT_DESCRIPTION_CLASS_NAME                                \
  INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(event_id)(       \
      const T1 &arg1, const T2 &arg2, const T3 &arg3, const T4 &arg4) {        \
    DECORATOR_EVENT_DESCRIPTION_CLASS_NAME ret(event_id);                      \
    ret.pushArg(arg1);                                                         \
    ret.pushArg(arg2);                                                         \
    ret.pushArg(arg3);                                                         \
    ret.pushArg(arg4);                                                         \
    return ret;                                                                \
  }                                                                            \
  }                                                                            \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID(event_id, 4, pattern, description)

#define RegisterDecoratorEventWithFiveArguments(event_id, pattern,             \
                                                description)                   \
  namespace REPORT_DECORATOR_NAMESPACE {                                       \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID_STRING(event_id);                       \
  template <class T1, class T2, class T3, class T4, class T5>                  \
  static DECORATOR_EVENT_DESCRIPTION_CLASS_NAME                                \
  INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(event_id)(       \
      const T1 &arg1, const T2 &arg2, const T3 &arg3, const T4 &arg4,          \
      const T5 &arg5) {                                                        \
    DECORATOR_EVENT_DESCRIPTION_CLASS_NAME ret(event_id);                      \
    ret.pushArg(arg1);                                                         \
    ret.pushArg(arg2);                                                         \
    ret.pushArg(arg3);                                                         \
    ret.pushArg(arg4);                                                         \
    ret.pushArg(arg5);                                                         \
    return ret;                                                                \
  }                                                                            \
  }                                                                            \
  INTERNAL_REGISTER_DECORATOR_EVENT_ID(event_id, 5, pattern, description)

#define RegisterVulnerabilityDescriptionEvent(event_id, vuln_name, CWEID_str)  \
  RegisterDecoratorEventWithNoArgument(                                        \
      event_id,                                                                \
      std::string(" (") +                                                      \
          ::lotus::reporting::ReportDecorator::decorator_color_red(            \
              std::string(vuln_name) +                                         \
              (strlen(CWEID_str) == 0 ? std::string("")                        \
                                      : std::string(", ") + CWEID_str)) +      \
          ")",                                                                 \
      std::string("Description of '") + vuln_name + "' bug")

#define pushDecoratorEvent(dbg_loc, event_id, ...)                             \
  {                                                                            \
    const DECORATOR_EVENT_DESCRIPTION_CLASS_NAME &event_desc =                 \
        REPORT_DECORATOR_NAMESPACE::                                           \
            INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(       \
                event_id)(__VA_ARGS__);                                        \
    pushEnd(event_desc, dbg_loc);                                              \
  }

#define pushReportDecoratorEventWithNoArg(dbg_loc, event_id)                   \
  {                                                                            \
    const DECORATOR_EVENT_DESCRIPTION_CLASS_NAME &event_desc =                 \
        REPORT_DECORATOR_NAMESPACE::                                           \
            INTERNAL_DECORATOR_EVENT_DESCRIPTION_CREATION_FUNCTION_NAME(       \
                event_id)();                                                   \
    pushEnd(event_desc, dbg_loc);                                              \
  }

} // namespace reporting
} // namespace lotus
