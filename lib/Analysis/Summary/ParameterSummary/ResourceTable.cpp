#include "Analysis/Summary/ParameterSummary/ResourceTable.h"

#include "Alias/Infrastructure/Spec/AliasSpecManager.h"

namespace lotus::analysis::parametersummary {

using lotus::alias::FunctionCategory;

ResourceTable::ResourceTable() { populateBuiltins(); }

ResourceTable ResourceTable::empty() {
  ResourceTable table;
  table.entries_.clear();
  return table;
}

ResourceTable ResourceTable::fromModuleSpecs(llvm::Module &module) {
  ResourceTable table = ResourceTable::empty();
  table.populateFromModuleSpecs(module);
  table.populateBuiltins();
  return table;
}

void ResourceTable::add(llvm::StringRef name, ResourceRole role) {
  entries_[name.str()].insert(role);
}

bool ResourceTable::hasRole(llvm::StringRef name, ResourceRole role) const {
  auto it = entries_.find(name.str());
  return it != entries_.end() && it->second.count(role) != 0;
}

std::vector<std::string> ResourceTable::functionNames() const {
  std::vector<std::string> names;
  names.reserve(entries_.size());
  for (const auto &entry : entries_)
    names.push_back(entry.first);
  return names;
}

void ResourceTable::populateFromModuleSpecs(llvm::Module &module) {
  lotus::alias::AliasSpecManager specs;
  specs.initialize(module);

  for (llvm::Function &function : module) {
    if (function.isIntrinsic())
      continue;

    llvm::StringRef name = function.getName();
    switch (specs.getCategory(&function)) {
    case FunctionCategory::Allocator:
      add(name, ResourceRole::Allocator);
      break;
    case FunctionCategory::Deallocator:
      add(name, ResourceRole::Deallocator);
      break;
    case FunctionCategory::Reallocator:
      add(name, ResourceRole::Reallocator);
      break;
    default:
      break;
    }

    for (const auto &return_alias : specs.getReturnAliasInfo(&function)) {
      if (return_alias.isNull) {
        add(name, ResourceRole::NullSource);
        break;
      }
    }

    auto mod_ref = specs.getModRefInfo(&function);
    if (!mod_ref.referencedArgs.empty())
      add(name, ResourceRole::Dereference);
  }
}

void ResourceTable::populateBuiltins() {
  populateCStdlib();
  populateCppOperators();
  populatePosixIo();
  populatePosixThreads();
  populateMemoryMapping();
  populateCommonWrappers();
  populateDereference();
}

void ResourceTable::populateCStdlib() {
  for (llvm::StringRef name :
       {"malloc", "calloc", "strdup", "strndup", "aligned_alloc"}) {
    add(name, ResourceRole::Allocator);
    add(name, ResourceRole::NullSource);
  }

  add("realloc", ResourceRole::Reallocator);
  add("realloc", ResourceRole::NullSource);
  add("free", ResourceRole::Deallocator);
}

void ResourceTable::populateCppOperators() {
  for (llvm::StringRef name :
       {"_Znwm", "_Znam", "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t"}) {
    add(name, ResourceRole::Allocator);
  }
  add("_ZnwmRKSt9nothrow_t", ResourceRole::NullSource);
  add("_ZnamRKSt9nothrow_t", ResourceRole::NullSource);

  for (llvm::StringRef name :
       {"_ZdlPv", "_ZdaPv", "_ZdlPvm", "_ZdaPvm"}) {
    add(name, ResourceRole::Deallocator);
  }
}

void ResourceTable::populatePosixIo() {
  for (llvm::StringRef name : {"fopen", "fdopen", "freopen", "open", "socket"}) {
    add(name, ResourceRole::Acquire);
  }
  add("fclose", ResourceRole::Release);
  add("close", ResourceRole::Release);
}

void ResourceTable::populatePosixThreads() {
  for (llvm::StringRef name : {"pthread_mutex_lock", "pthread_rwlock_rdlock",
                               "pthread_rwlock_wrlock"}) {
    add(name, ResourceRole::Lock);
  }
  add("pthread_mutex_unlock", ResourceRole::Unlock);
  add("pthread_rwlock_unlock", ResourceRole::Unlock);
}

void ResourceTable::populateMemoryMapping() {
  add("mmap", ResourceRole::Allocator);
  add("mmap", ResourceRole::NullSource);
  add("munmap", ResourceRole::Deallocator);
}

void ResourceTable::populateCommonWrappers() {
  for (llvm::StringRef name : {"g_malloc", "g_malloc0", "g_try_malloc",
                               "g_try_malloc0", "xmalloc", "xcalloc",
                               "xstrdup"}) {
    add(name, ResourceRole::Allocator);
  }
  add("g_free", ResourceRole::Deallocator);
  add("xrealloc", ResourceRole::Reallocator);
}

void ResourceTable::populateDereference() {
  for (llvm::StringRef name :
       {"memcpy", "memmove", "memset", "strlen", "printf", "fprintf",
        "strcpy", "strncpy"}) {
    add(name, ResourceRole::Dereference);
  }
}

} // namespace lotus::analysis::parametersummary
