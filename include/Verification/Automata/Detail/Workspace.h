#pragma once

#include "Verification/Automata/NestedWordAutomaton.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory_resource>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace lotus::verification::automata::detail {

// A fixed-capacity arena. Storage is acquired during preparation; containers
// using this resource never fall back to the process heap. The limit is shared
// by all arena-backed structures in one workspace.
class Workspace final : public std::pmr::memory_resource {
  std::byte *storage_;
  std::size_t capacity_, used_ = 0;

  void *do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (bytes == 0)
      bytes = 1;
    if (bytes > capacity_ - used_)
      throw ResourceLimit();
    void *position = storage_ + used_;
    auto available = capacity_ - used_;
    if (!std::align(alignment, bytes, position, available))
      throw ResourceLimit();
    used_ = capacity_ - available + bytes;
    return position;
  }
  void do_deallocate(void *, std::size_t, std::size_t) override {}
  bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

public:
  explicit Workspace(std::size_t bytes) : storage_(nullptr), capacity_(bytes) {
    if (!bytes)
      return;
#if defined(__unix__) || defined(__APPLE__)
    // Reserve address space once and commit pages as used. Large configured
    // limits do not force every small query to touch the entire budget.
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
      throw ResourceLimit();
    storage_ = static_cast<std::byte *>(p);
#else
    storage_ = new std::byte[bytes];
#endif
  }
  ~Workspace() override {
    if (!storage_)
      return;
#if defined(__unix__) || defined(__APPLE__)
    munmap(storage_, capacity_);
#else
    delete[] storage_;
#endif
  }
  Workspace(const Workspace &) = delete;
  Workspace &operator=(const Workspace &) = delete;
  std::pmr::memory_resource *resource() { return this; }
  std::size_t bytesUsed() const { return used_; }
};

class HotLoopScope {
  const OperationContext &context_;
  const char *phase_;

public:
  HotLoopScope(const OperationContext &context, const char *phase)
      : context_(context), phase_(phase) {
    if (context_.hotLoopObserver)
      context_.hotLoopObserver(phase_, true, context_.observerData);
    try {
      context_.check();
    } catch (...) {
      if (context_.hotLoopObserver)
        context_.hotLoopObserver(phase_, false, context_.observerData);
      throw;
    }
  }
  ~HotLoopScope() {
    if (context_.hotLoopObserver)
      context_.hotLoopObserver(phase_, false, context_.observerData);
  }
  HotLoopScope(const HotLoopScope &) = delete;
  HotLoopScope &operator=(const HotLoopScope &) = delete;
};

template <typename Iterator, typename Compare = std::less<>>
void checkedSort(Iterator first, Iterator last, const OperationContext &context,
                 Compare compare = {}) {
  context.check();
  if (!context.cancel &&
      context.deadline == std::chrono::steady_clock::time_point::max()) {
    std::sort(first, last, compare);
  } else {
    std::sort(first, last, [&](const auto &a, const auto &b) {
      context.check();
      return compare(a, b);
    });
  }
}
template <typename Iterator>
Iterator checkedUnique(Iterator first, Iterator last,
                       const OperationContext &context) {
  context.check();
  if (!context.cancel &&
      context.deadline == std::chrono::steady_clock::time_point::max())
    return std::unique(first, last);
  return std::unique(first, last, [&](const auto &a, const auto &b) {
    context.check();
    return a == b;
  });
}
} // namespace lotus::verification::automata::detail
