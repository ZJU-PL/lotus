#pragma once

#include "Checker/TPA/Annotation/TaintDescriptor.h"
#include "Checker/TPA/Lattice/TaintLattice.h"

#include <cassert>

namespace annotation {

class SourceTaintEntry {
private:
  TPosition pos;
  TClass tClass;
  tpa::taint::TaintLattice val;

public:
  SourceTaintEntry(TPosition p, TClass c, tpa::taint::TaintLattice l)
      : pos(p), tClass(c), val(l) {}

  TPosition getTaintPosition() const { return pos; }
  TClass getTaintClass() const { return tClass; }
  tpa::taint::TaintLattice getTaintValue() const { return val; }
};

class PipeTaintEntry {
private:
  TPosition dstPos, srcPos;
  TClass dstClass, srcClass;

public:
  PipeTaintEntry(TPosition dp, TClass dc, TPosition sp, TClass sc)
      : dstPos(dp), srcPos(sp), dstClass(dc), srcClass(sc) {}

  TPosition getDstPosition() const { return dstPos; }
  TClass getDstClass() const { return dstClass; }
  TPosition getSrcPosition() const { return srcPos; }
  TClass getSrcClass() const { return srcClass; }
};

class SinkTaintEntry {
private:
  TPosition argPos;
  TClass argClass;

public:
  SinkTaintEntry(TPosition p, TClass c) : argPos(p), argClass(c) {
    assert(!argPos.isReturnPosition());
  }

  TPosition getArgPosition() const { return argPos; }
  TClass getTaintClass() const { return argClass; }
};

class TaintEntry {
private:
  TEnd type;

  union {
    SourceTaintEntry source;
    PipeTaintEntry pipe;
    SinkTaintEntry sink;
  };

  TaintEntry(TPosition p, TClass c, tpa::taint::TaintLattice l)
      : type(TEnd::Source) {
    new (&source) SourceTaintEntry(p, c, l);
  }
  TaintEntry(TPosition dp, TClass dc, TPosition sp, TClass sc)
      : type(TEnd::Pipe) {
    new (&pipe) PipeTaintEntry(dp, dc, sp, sc);
  }
  TaintEntry(TPosition p, TClass c) : type(TEnd::Sink) {
    new (&sink) SinkTaintEntry(p, c);
  }

public:
  TaintEntry(const TaintEntry &rhs) : type(rhs.type) {
    switch (type) {
    case TEnd::Source:
      new (&source) SourceTaintEntry(rhs.source);
      break;
    case TEnd::Pipe:
      new (&pipe) PipeTaintEntry(rhs.pipe);
      break;
    case TEnd::Sink:
      new (&sink) SinkTaintEntry(rhs.sink);
      break;
    }
  }

  static TaintEntry getSourceEntry(TPosition p, TClass c,
                                   tpa::taint::TaintLattice l) {
    return TaintEntry(p, c, l);
  }
  static TaintEntry getPipeEntry(TPosition dp, TClass dc, TPosition sp,
                                 TClass sc) {
    return TaintEntry(dp, dc, sp, sc);
  }
  static TaintEntry getSinkEntry(TPosition p, TClass c) {
    return TaintEntry(p, c);
  }

  ~TaintEntry() {
    switch (type) {
    case TEnd::Source:
      source.~SourceTaintEntry();
      break;
    case TEnd::Pipe:
      pipe.~PipeTaintEntry();
      break;
    case TEnd::Sink:
      sink.~SinkTaintEntry();
      break;
    }
  }

  TEnd getEntryEnd() const { return type; }

  const SourceTaintEntry &getAsSourceEntry() const {
    assert(type == TEnd::Source);
    return source;
  }
  const PipeTaintEntry &getAsPipeEntry() const {
    assert(type == TEnd::Pipe);
    return pipe;
  }
  const SinkTaintEntry &getAsSinkEntry() const {
    assert(type == TEnd::Sink);
    return sink;
  }
};

} // namespace annotation
