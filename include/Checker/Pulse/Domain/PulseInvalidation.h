/** @file PulseInvalidation.h @brief Pointer invalidation tracking for Pulse analysis. */
#pragma once

#include <string>

namespace llvm {
class Instruction;
} // namespace llvm

namespace pulse {

/**
 * Invalidation kind: how memory was invalidated (Infer-aligned).
 * Used for richer diagnostics and decompiler-style traces.
 */
enum class InvalidationKind {
  CFree,
  CppDelete,
  CppDeleteArray,
  GoneOutOfScope,
  Realloc,
  FClose,
  LockReleased,
  StdVectorPushBack,
  StdVectorClear,
  Other
};

/** Human-readable description for reporting. */
inline const char *invalidationKindString(InvalidationKind k) {
  switch (k) {
  case InvalidationKind::CFree:
    return "freed (free)";
  case InvalidationKind::CppDelete:
    return "freed (delete)";
  case InvalidationKind::CppDeleteArray:
    return "freed (delete[])";
  case InvalidationKind::GoneOutOfScope:
    return "gone out of scope";
  case InvalidationKind::Realloc:
    return "invalidated (realloc)";
  case InvalidationKind::FClose:
    return "closed (fclose)";
  case InvalidationKind::LockReleased:
    return "released (unlock)";
  case InvalidationKind::StdVectorPushBack:
    return "invalidated (std::vector::push_back)";
  case InvalidationKind::StdVectorClear:
    return "invalidated (std::vector::clear)";
  case InvalidationKind::Other:
  default:
    return "invalidated";
  }
}

} // namespace pulse

