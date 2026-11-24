#pragma once

#include <string>

namespace pdg {

enum class ResourceKind {
  Unknown,
  Heap,
  File,
  FileDescriptor,
  Directory,
  Lock
};

std::string resourceKindName(ResourceKind kind);

} // namespace pdg
