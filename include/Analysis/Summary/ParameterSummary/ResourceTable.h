/** @file ResourceTable.h @brief Resource table for tracking parameter-related resources. */
#pragma once

#include "llvm/ADT/StringRef.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus::analysis::parametersummary {

enum class ResourceRole {
  Allocator,
  Deallocator,
  Reallocator,
  Acquire,
  Release,
  Lock,
  Unlock,
  NullSource,
  Dereference,
};

class ResourceTable {
public:
  ResourceTable();

  static ResourceTable empty();
  static ResourceTable fromModuleSpecs(llvm::Module &module);

  void add(llvm::StringRef name, ResourceRole role);
  bool hasRole(llvm::StringRef name, ResourceRole role) const;
  std::vector<std::string> functionNames() const;

private:
  void populateBuiltins();
  void populateFromModuleSpecs(llvm::Module &module);
  void populateCStdlib();
  void populateCppOperators();
  void populatePosixIo();
  void populatePosixThreads();
  void populateMemoryMapping();
  void populateCommonWrappers();
  void populateDereference();

  std::map<std::string, std::set<ResourceRole>> entries_;
};

} // namespace lotus::analysis::parametersummary
