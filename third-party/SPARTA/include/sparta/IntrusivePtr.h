/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the MIT license in the SPARTA LICENSE file.
 */
#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace sparta {

// Ownership follows the pointee's ADL add_ref/release hooks. In particular,
// Patricia trees keep their atomic reference count and leaf tag in one word,
// while S-expression components retain their original non-atomic counter.
template <typename T> class intrusive_ptr {
public:
  using element_type = T;

  constexpr intrusive_ptr() noexcept = default;
  constexpr intrusive_ptr(std::nullptr_t) noexcept {}

  explicit intrusive_ptr(T *pointer, bool add_ref = true) : m_pointer(pointer) {
    if (m_pointer && add_ref) {
      intrusive_ptr_add_ref(m_pointer);
    }
  }

  intrusive_ptr(const intrusive_ptr &other) : intrusive_ptr(other.get()) {}
  template <typename U,
            std::enable_if_t<std::is_convertible_v<U *, T *>, int> = 0>
  intrusive_ptr(const intrusive_ptr<U> &other) : intrusive_ptr(other.get()) {}

  intrusive_ptr(intrusive_ptr &&other) noexcept : m_pointer(other.detach()) {}
  template <typename U,
            std::enable_if_t<std::is_convertible_v<U *, T *>, int> = 0>
  intrusive_ptr(intrusive_ptr<U> &&other) noexcept
      : m_pointer(other.detach()) {}

  ~intrusive_ptr() {
    if (m_pointer) {
      intrusive_ptr_release(m_pointer);
    }
  }

  intrusive_ptr &operator=(intrusive_ptr other) noexcept {
    swap(other);
    return *this;
  }

  T *get() const noexcept { return m_pointer; }
  T &operator*() const noexcept { return *m_pointer; }
  T *operator->() const noexcept { return m_pointer; }
  explicit operator bool() const noexcept { return m_pointer != nullptr; }

  T *detach() noexcept { return std::exchange(m_pointer, nullptr); }
  void reset(T *pointer = nullptr, bool add_ref = true) {
    intrusive_ptr(pointer, add_ref).swap(*this);
  }
  void swap(intrusive_ptr &other) noexcept {
    std::swap(m_pointer, other.m_pointer);
  }

private:
  T *m_pointer{nullptr};
};

template <typename T, typename U>
bool operator==(const intrusive_ptr<T> &left,
                const intrusive_ptr<U> &right) noexcept {
  return left.get() == right.get();
}
template <typename T, typename U>
bool operator!=(const intrusive_ptr<T> &left,
                const intrusive_ptr<U> &right) noexcept {
  return !(left == right);
}
template <typename T>
bool operator==(const intrusive_ptr<T> &pointer, std::nullptr_t) noexcept {
  return pointer.get() == nullptr;
}
template <typename T>
bool operator==(std::nullptr_t, const intrusive_ptr<T> &pointer) noexcept {
  return pointer == nullptr;
}
template <typename T>
bool operator!=(const intrusive_ptr<T> &pointer, std::nullptr_t) noexcept {
  return !(pointer == nullptr);
}
template <typename T>
bool operator!=(std::nullptr_t, const intrusive_ptr<T> &pointer) noexcept {
  return !(pointer == nullptr);
}
template <typename T> T *get_pointer(const intrusive_ptr<T> &pointer) noexcept {
  return pointer.get();
}

template <typename T, typename U>
intrusive_ptr<T> static_pointer_cast(const intrusive_ptr<U> &pointer) {
  return intrusive_ptr<T>(static_cast<T *>(pointer.get()));
}
template <typename T, typename U>
intrusive_ptr<T> static_pointer_cast(intrusive_ptr<U> &&pointer) noexcept {
  return intrusive_ptr<T>(static_cast<T *>(pointer.detach()), false);
}
template <typename T>
void swap(intrusive_ptr<T> &left, intrusive_ptr<T> &right) noexcept {
  left.swap(right);
}

} // namespace sparta
