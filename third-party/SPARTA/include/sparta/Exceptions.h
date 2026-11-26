/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <any>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace sparta {

namespace exception_impl {

template <typename Info>
inline constexpr unsigned char error_info_key = 0;

template <typename T, typename = void>
struct is_ostreamable : std::false_type {};

template <typename T>
struct is_ostreamable<T, std::void_t<decltype(std::declval<std::ostream &>()
                                              << std::declval<T>())>>
    : std::true_type {};

template <typename T> std::string stringify(const T &value) {
  if constexpr (is_ostreamable<T>::value) {
    std::ostringstream output;
    output << value;
    return output.str();
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<std::underlying_type_t<T>>(value));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

} // namespace exception_impl

// Typed attributes are independent of the exception's inheritance hierarchy,
// so the throw macro can attach source context to ordinary std::exceptions too.
class exception_metadata {
public:
  const char* diagnostic() const noexcept { return m_message.c_str(); }

  template <typename Info>
  void set_error_info(typename Info::value_type value) const {
    m_info.insert_or_assign(
        &exception_impl::error_info_key<Info>,
        std::any(std::in_place_type<typename Info::value_type>, std::move(value)));
  }

  template <typename Info>
  const typename Info::value_type* find_error_info() const noexcept {
    const auto found = m_info.find(&exception_impl::error_info_key<Info>);
    return found == m_info.end()
               ? nullptr
               : std::any_cast<typename Info::value_type>(&found->second);
  }

  void append_error(std::string_view name, std::string value) const {
    m_message += m_has_error ? ", " : ": ";
    m_message += name;
    m_message += "='";
    m_message += std::move(value);
    m_message += '\'';
    m_has_error = true;
  }

private:
  mutable std::string m_message{"abstract interpretation error"};
  mutable bool m_has_error{false};
  mutable std::unordered_map<const void*, std::any> m_info;
};

/* The base class of all abstract-interpretation exceptions. */
class abstract_interpretation_exception : public virtual std::exception,
                                          public virtual exception_metadata {
 public:
  const char* what() const noexcept override { return diagnostic(); }
};

/* This exception flags an inconsistent internal state. */
class internal_error : public virtual abstract_interpretation_exception {};

/* This exception flags an argument holding an invalid value. */
class invalid_argument : public virtual abstract_interpretation_exception {};

/* This exception flags use of an operation outside its domain. */
class undefined_operation : public virtual abstract_interpretation_exception {};

struct tag_error_msg {};
struct tag_argument_name {};
struct tag_operation_name {};
struct tag_expected_kind {};
struct tag_actual_kind {};
struct tag_exception_at {};
struct tag_throw_function {};
struct tag_throw_file {};
struct tag_throw_line {};

template <typename Tag> constexpr std::string_view error_info_name() {
  return "error_info";
}

template <> constexpr std::string_view error_info_name<tag_error_msg>() {
  return "error_msg";
}

template <> constexpr std::string_view error_info_name<tag_argument_name>() {
  return "argument_name";
}

template <> constexpr std::string_view error_info_name<tag_operation_name>() {
  return "operation_name";
}

template <> constexpr std::string_view error_info_name<tag_expected_kind>() {
  return "expected_kind";
}

template <> constexpr std::string_view error_info_name<tag_actual_kind>() {
  return "actual_kind";
}

template <> constexpr std::string_view error_info_name<tag_exception_at>() {
  return "exception_at";
}

template <> constexpr std::string_view error_info_name<tag_throw_function>() { return "throw_function"; }
template <> constexpr std::string_view error_info_name<tag_throw_file>() { return "throw_file"; }
template <> constexpr std::string_view error_info_name<tag_throw_line>() { return "throw_line"; }

/* A name-value pair attached lazily to a SPARTA exception. */
template <typename Tag, typename Value> class error_info {
public:
  using value_type = Value;
  explicit error_info(Value value) : m_value(std::move(value)) {}

  void append_to(const exception_metadata &exception) const {
    exception.template set_error_info<error_info>(m_value);
    exception.append_error(error_info_name<Tag>(),
                           exception_impl::stringify(m_value));
  }

private:
  Value m_value;
};

// Retain typed metadata and retrieval, rather than reducing error_info to a
// diagnostic string. This is the Boost-free counterpart of get_error_info.
template <typename Info, typename Exception>
const typename Info::value_type* get_error_info(const Exception& exception) noexcept {
  if constexpr (std::is_base_of_v<exception_metadata, Exception>) {
    return exception.template find_error_info<Info>();
  } else if constexpr (std::is_polymorphic_v<Exception>) {
#if defined(__cpp_rtti) || defined(__GXX_RTTI) || defined(_CPPRTTI)
    const auto* error = dynamic_cast<const exception_metadata*>(&exception);
    return error ? error->template find_error_info<Info>() : nullptr;
#else
    return nullptr;
#endif
  } else {
    return nullptr;
  }
}

using error_msg = error_info<tag_error_msg, std::string>;
using argument_name = error_info<tag_argument_name, std::string>;
using operation_name = error_info<tag_operation_name, std::string>;
using throw_function = error_info<tag_throw_function, std::string>;
using throw_file = error_info<tag_throw_file, std::string>;
using throw_line = error_info<tag_throw_line, int>;

template <
    typename Exception, typename Tag, typename Value,
    std::enable_if_t<std::is_base_of_v<exception_metadata,
                                       std::remove_reference_t<Exception>>,
                     int> = 0>
Exception &&operator<<(Exception &&exception,
                       const error_info<Tag, Value> &info) {
  info.append_to(exception);
  return std::forward<Exception>(exception);
}

} // namespace sparta

namespace sparta::exception_impl {

#if defined(__GNUC__) || defined(__clang__)
#define SPARTA_CURRENT_FUNCTION __PRETTY_FUNCTION__
#elif defined(_MSC_VER)
#define SPARTA_CURRENT_FUNCTION __FUNCSIG__
#else
#define SPARTA_CURRENT_FUNCTION __func__
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SPARTA_COLD_FUNCTION __attribute__((cold))
#define SPARTA_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define SPARTA_COLD_FUNCTION
#define SPARTA_NOINLINE __declspec(noinline)
#else
#define SPARTA_COLD_FUNCTION
#define SPARTA_NOINLINE
#endif

template <typename Error>
class with_context : public Error, public exception_metadata {
 public:
  explicit with_context(const Error& error) : Error(error) {}
  explicit with_context(Error&& error) : Error(std::move(error)) {}
};

inline void add_location(const exception_metadata& error, const char* function,
                         const char* file, int line) {
  error << throw_function(function) << throw_file(file) << throw_line(line);
}

template <typename Exception>
[[noreturn]] SPARTA_COLD_FUNCTION void
throw_exception(Exception &&exception, const char *function, const char *file,
                int line) {
  using Error = std::decay_t<Exception>;
  if constexpr (std::is_base_of_v<exception_metadata, Error>) {
    auto copy = std::forward<Exception>(exception);
    add_location(copy, function, file, line);
    throw copy;
  } else if constexpr (std::is_base_of_v<std::exception, Error> && !std::is_final_v<Error>) {
    with_context<Error> copy(std::forward<Exception>(exception));
    add_location(copy, function, file, line);
    throw copy;
  } else {
    throw std::forward<Exception>(exception);
  }
}

} // namespace sparta::exception_impl

#if defined(__GNUC__) || defined(__clang__)
#define SPARTA_UNLIKELY(C) (__builtin_expect(!!(C), 0))
#else
#define SPARTA_UNLIKELY(C) (C)
#endif

// As upstream, keep exception construction in a deferred, cold call instead
// of inflating the hot analysis routine with string/attribute construction.
#define SPARTA_THROW_EXCEPTION(E) \
  SPARTA_THROW_EXCEPTION_IMPL(E, __FILE__, __LINE__)
#define SPARTA_THROW_EXCEPTION_IMPL(E, File, Line) \
  SPARTA_THROW_EXCEPTION_IMPL2(E, File, Line)
#define SPARTA_THROW_EXCEPTION_IMPL2(E, File, Line) \
  do { \
    constexpr const char* kCurrentFunction = SPARTA_CURRENT_FUNCTION; \
    auto deferred = [&]() { \
      ::sparta::exception_impl::throw_exception((E), kCurrentFunction, File, Line); \
    }; \
    struct L##Line { \
      decltype(deferred) impl; \
      [[noreturn]] SPARTA_NOINLINE SPARTA_COLD_FUNCTION void throw_exception() { \
        impl(); \
        std::terminate(); \
      } \
    }; \
    L##Line{std::move(deferred)}.throw_exception(); \
  } while (0)

/* An assert-like macro that throws an exception. */
#define SPARTA_RUNTIME_CHECK(C, E)                                             \
  do {                                                                         \
    if (SPARTA_UNLIKELY(!(C))) {                                               \
      SPARTA_THROW_EXCEPTION((E));                                             \
    }                                                                          \
  } while (0)

/* Indicates that the given variable is unused. */
#define SPARTA_UNUSED_VARIABLE(v) static_cast<void>(v)
