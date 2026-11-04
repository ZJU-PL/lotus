#pragma once

#include <algorithm>
#include <cassert>
#include <utility>
#include <vector>

#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace trace {
using llvm::raw_ostream;

enum TraceType {
  // Source-Sink trace such as NPD
  SOURCE_SINK,
  // Source with no sink trace such as ML
  SOURCE_NO_SINK,
  // A simple sequence of trace items indicating a value flow
  SIMPLE_VALUE_FLOW,
  // A simple sequence of trace items not indicating a value flow
  NON_VALUE_FLOW
};

/*
 * Trace is an ordered list of values with checker reporting metadata.
 * Trace is designed as a lightweight data structure that can be frequently
 * created and dropped without much performance penalty. The base class of Trace
 * does not assign any semantics to the trace, use the derived classes for
 * specific purposes.
 *
 * Note: The trace content type T must be a POD type.
 */
template <typename T> class Trace {
private:
  /// Score is designed for reporting system as a comprehensive assessment to
  /// the trace Confidence/Importance is designed for checker writers to provide
  /// internal info to scoring system Confidence is for the feasibility of the
  /// trace, Importance is for the nature of the report

  /// The constructive confidence of the trace' feasibility
  ///      (the probability that the trace is valid according to the report
  ///      construction procedure)
  /// which should be set by users who construct the trace.
  /// The value should be in [0, 1].
  float constructive_confidence = 1.0;

  /// The importance of the bug type that the Trace is reporting
  /// For example exploitable security issue/crash bugs has higher importance
  /// and bad programming practice has lower importance
  /// This shall be specified by checker writers
  /// Value ranges in [0,100], Default is 80
  int bug_type_importance = 80;

  /// The final score of the trace indicating the importance of the trace
  /// which shall be used for bug reporting system
  /// This value is decided during post verification procedure when all traces
  /// are generated checkers do not need to update this part. The value should
  /// be in [0, 100].
  int score = SCORE_UNINIT;

  /// Define the type of the trace. This is used to guess key steps in the trace
  TraceType trace_type = SIMPLE_VALUE_FLOW;

protected:
  std::vector<T> vec_trace;

public:
  inline static constexpr int SCORE_UNINIT = -1;
  inline static constexpr int IDX_INVALID = -1;

  Trace() {}
  Trace(std::vector<T> trace) : vec_trace(trace) {}
  Trace(const Trace<T> &trace) : vec_trace(trace.vec_trace) {
    bug_type_importance = trace.bug_type_importance;
    constructive_confidence = trace.constructive_confidence;
    score = trace.score;
    trace_type = trace.trace_type;
  }
  virtual ~Trace() {};

  Trace &operator=(const Trace &trace) {
    if (this != &trace) {
      vec_trace = trace.vec_trace;
      bug_type_importance = trace.bug_type_importance;
      constructive_confidence = trace.constructive_confidence;
      score = trace.score;
      trace_type = trace.trace_type;
    }
    return *this;
  }

  //-------Interfaces for manipulating a trace-------
  // These functions are kept non-virtual to improve performance

  // clear the trace
  void clear() { vec_trace.clear(); }

  // Reverse the order of the values in the trace
  void reverse() { std::reverse(vec_trace.begin(), vec_trace.end()); }

  const T &head() const {
    assert(get_length() > 0 && "Getting element from an empty trace");
    return at(0);
  }

  const T &tail() const {
    assert(get_length() > 0 && "Getting element from an empty trace");
    return at(get_length() - 1);
  }

  // Append/remove a value from the end of trace
  // This is the main method to modify the trace since trace is seldom modified
  // randomly
  void push(const T &v) { vec_trace.push_back(v); }
  void push(const Trace<T> &other) {
    vec_trace.insert(vec_trace.end(), other.vec_trace.begin(),
                     other.vec_trace.end());
  }
  T pop() {
    assert(!vec_trace.empty());
    T result = vec_trace.back();
    vec_trace.pop_back();
    return result;
  }

  // Also provide interfaces to randomly modify the trace, but these operations
  // take O(n) time Insert a value into trace before/after the position \p x.
  // The range for \p x >= 0
  // If \p x >= current_trace_size, calling these functions is equal to calling
  // push(v)
  void insert_before(int x, T &v) {
    assert(x >= 0 && "Cannot insert value to a negative index");

    push(v);
    int size = get_length();
    for (int i = size - 1; i > x; --i) {
      std::swap(vec_trace[i], vec_trace[i - 1]);
    }
  }
  void insert_after(int x, T &v) { insert_before(x + 1, v); }

  //-------Interfaces for iterating a trace-------
  // We recommend users to iterate the trace after the trace is finished/valid
  // Yet the following interfaces can be applied when the trace is not fully
  // generated, i.e. not valid

  // get the length of the trace
  int get_length() const { return vec_trace.size(); }

  // iterating the trace
  typename std::vector<T>::iterator begin() { return vec_trace.begin(); }
  typename std::vector<T>::iterator end() { return vec_trace.end(); }

  typename std::vector<T>::const_iterator begin() const {
    return vec_trace.begin();
  }
  typename std::vector<T>::const_iterator end() const {
    return vec_trace.end();
  }

  // Obtain the value at position \p x of the trace
  const T &operator[](int x) const { return vec_trace[x]; }
  const T &at(int x) const { return operator[](x); }

  /// See \c Trace::score
  void set_score(int score) {
    if (score != SCORE_UNINIT) {
      if (score > 100)
        score = 100;
      else if (score < 0)
        score = 0;
    }

    this->score = score;
  };

  /// See \c Trace::Score
  int get_score() const { return score; };

  /// See \c Trace::bug_type_importance
  void set_bug_type_importance(int bug_type_importance) {
    if (bug_type_importance > 100)
      bug_type_importance = 100;
    else if (bug_type_importance < 0)
      bug_type_importance = 0;

    this->bug_type_importance = bug_type_importance;
  };

  /// See \c Trace::bug_type_importance
  int get_bug_type_importance() const { return bug_type_importance; };

  /// See \c Trace::constructive_confidence
  void set_constructive_confidence(float constructive_confidence) {
    if (constructive_confidence > 1.0f)
      constructive_confidence = 1.0f;
    else if (constructive_confidence < 0.0f)
      constructive_confidence = 0.0f;

    this->constructive_confidence = constructive_confidence;
  };

  /// See \c Trace::constructive_confidence
  float get_constructive_confidence() const { return constructive_confidence; };

  /// See \c Trace::constructive_confidence
  void set_trace_type(TraceType trace_type) { this->trace_type = trace_type; };

  /// See \c Trace::constructive_confidence
  TraceType get_trace_type() const { return trace_type; };

  // This succinct version can be called in gdb to print the trace
  virtual void print(raw_ostream &O) {
    O << "Trace with " << get_length() << " steps\n";
  }
};

} // namespace trace
} // namespace lotus
