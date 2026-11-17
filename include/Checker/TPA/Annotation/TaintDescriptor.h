#pragma once

#include "Annotation/ArgPosition.h"

namespace annotation {

// TClass specify whether the taintness is on the value, on the memory location
// pointed to by the value, or on all memory locations that can be obtained from
// dereferencing pointers that are results of doing pointer arithmetics on the
// value
enum class TClass : uint8_t { ValueOnly, DirectMemory, ReachableMemory };

// TEnd specify whether the given record is a taint source, a taint sink, or a
// taint pipe (transfer taint from one end to another)
enum class TEnd : uint8_t { Source, Sink, Pipe };

// TPosition specify where the position of the taintness is
using TPosition = APosition;

} // namespace annotation
