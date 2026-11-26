#pragma once

#include "IR/PDG/Models/LibraryModels.h"

namespace pdg {

/// Exact standard basic_string API models. The qualified declaring class,
/// character specialization and LLVM ABI argument roles are checked; unrelated
/// application classes with methods named assign/append/data remain unknown.
/// These models also apply to defined standard-library header methods.
class CppLibraryModels {
public:
  /// A pointer to an exact standard string record, including supported inline
  /// ABI namespaces. Containing records and other STL templates do not qualify.
  static bool isStringObject(const llvm::Value &value);
  static llvm::Optional<CallTaintModel> taint(const llvm::CallBase &call);
  /// Stable basic_string object references returned by mutating APIs alias
  /// their receiver. Character pointers from data/c_str/indexing are excluded:
  /// backing-store invalidation needs a separate storage-epoch analysis.
  static llvm::Optional<unsigned> returnAlias(const llvm::CallBase &call);
};

} // namespace pdg
