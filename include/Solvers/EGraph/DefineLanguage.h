#pragma once

#include "Solvers/EGraph/Language.h"

#include <array>
#include <charconv>
#include <locale>
#include <system_error>
#include <variant>

namespace lotus::egraph {

struct DynamicLangDiscriminant {
  Symbol op;
  size_t arity = 0;

  friend bool operator==(DynamicLangDiscriminant lhs,
                         DynamicLangDiscriminant rhs) {
    return lhs.op == rhs.op && lhs.arity == rhs.arity;
  }

  friend bool operator!=(DynamicLangDiscriminant lhs,
                         DynamicLangDiscriminant rhs) {
    return !(lhs == rhs);
  }
};

class DynamicLang {
public:
  using Discriminant = DynamicLangDiscriminant;

  DynamicLang() = default;
  DynamicLang(Symbol op, std::vector<Id> children)
      : op_(op), children_(children.begin(), children.end()) {}
  DynamicLang(Symbol op, std::initializer_list<Id> children)
      : op_(op), children_(children.begin(), children.end()) {}

  static DynamicLang leaf(Symbol op) { return DynamicLang(op, {}); }

  const Symbol &op() const { return op_; }
  const llvm::SmallVector<Id, 2> &children() const { return children_; }
  llvm::SmallVector<Id, 2> &childrenMut() { return children_; }

  Discriminant discriminant() const {
    return Discriminant{op_, children_.size()};
  }

  bool matches(const DynamicLang &other) const {
    return op_ == other.op_ && children_.size() == other.children_.size();
  }

  template <typename F> DynamicLang mapChildren(F &&fn) const {
    auto copy = *this;
    for (Id &id : copy.children_) {
      id = fn(id);
    }
    return copy;
  }

  friend bool operator==(const DynamicLang &lhs, const DynamicLang &rhs) {
    return lhs.op_ == rhs.op_ && lhs.children_ == rhs.children_;
  }

  friend bool operator!=(const DynamicLang &lhs, const DynamicLang &rhs) {
    return !(lhs == rhs);
  }

  friend bool operator<(const DynamicLang &lhs, const DynamicLang &rhs) {
    return std::tie(lhs.op_, lhs.children_) < std::tie(rhs.op_, rhs.children_);
  }

private:
  Symbol op_;
  llvm::SmallVector<Id, 2> children_;
};

template <> struct LanguageOps<DynamicLang> {
  static std::optional<DynamicLang> fromOp(std::string_view op,
                                           const std::vector<Id> &children) {
    return DynamicLang(Symbol(op), children);
  }

  static std::string display(const DynamicLang &node) {
    return std::string(node.op().view());
  }
};

namespace detail {

template <typename T, typename = void>
struct IsEqualityComparable : std::false_type {};

template <typename T>
struct IsEqualityComparable<
    T, std::void_t<decltype(std::declval<const T &>() ==
                            std::declval<const T &>())>>
    : std::is_convertible<decltype(std::declval<const T &>() ==
                                   std::declval<const T &>()),
                          bool> {};

template <typename T, typename = void>
struct IsLessThanComparable : std::false_type {};

template <typename T>
struct IsLessThanComparable<
    T, std::void_t<decltype(std::declval<const T &>() <
                            std::declval<const T &>())>>
    : std::is_convertible<decltype(std::declval<const T &>() <
                                   std::declval<const T &>()),
                          bool> {};

template <typename T, typename = void> struct IsHashable : std::false_type {};

template <typename T>
struct IsHashable<
    T, std::void_t<decltype(std::hash<T>{}(std::declval<const T &>()))>>
    : std::true_type {};

template <typename T>
struct IsTypedLanguagePayload
    : std::bool_constant<std::is_copy_constructible_v<T> &&
                         IsEqualityComparable<T>::value &&
                         IsLessThanComparable<T>::value && IsHashable<T>::value &&
                         !std::is_floating_point_v<T>> {};

} // namespace detail

// Customize this trait for payload types that do not support stream parsing.
template <typename T, typename = void> struct TypedValueCodec {
  static std::optional<T> parse(std::string_view text) {
    static_assert(
        std::is_default_constructible_v<T>,
        "Stream-based TypedValueCodec payloads must be default constructible; "
        "specialize TypedValueCodec<T> for other payload types");
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    T value;
    if (!(input >> value)) {
      return std::nullopt;
    }
    input >> std::ws;
    if (!input.eof()) {
      return std::nullopt;
    }
    return value;
  }

  static std::string display(const T &value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << value;
    return output.str();
  }
};

template <typename T>
struct TypedValueCodec<
    T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
  static std::optional<T> parse(std::string_view text) {
    if (!text.empty() && text.front() == '+') {
      text.remove_prefix(1);
    }
    if constexpr (std::is_signed_v<T>) {
      int64_t value = 0;
      auto result =
          std::from_chars(text.data(), text.data() + text.size(), value, 10);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
          value < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
          value > static_cast<int64_t>(std::numeric_limits<T>::max())) {
        return std::nullopt;
      }
      return static_cast<T>(value);
    } else {
      uint64_t value = 0;
      auto result =
          std::from_chars(text.data(), text.data() + text.size(), value, 10);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
          value > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
        return std::nullopt;
      }
      return static_cast<T>(value);
    }
  }

  static std::string display(T value) {
    std::array<char, std::numeric_limits<uint64_t>::digits10 + 3> buffer{};
    if constexpr (std::is_signed_v<T>) {
      auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                  static_cast<int64_t>(value), 10);
      if (result.ec != std::errc{}) {
        throw std::runtime_error("Failed to format integral typed payload");
      }
      return std::string(buffer.data(), result.ptr);
    } else {
      auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                  static_cast<uint64_t>(value), 10);
      if (result.ec != std::errc{}) {
        throw std::runtime_error("Failed to format integral typed payload");
      }
      return std::string(buffer.data(), result.ptr);
    }
  }
};

template <> struct TypedValueCodec<bool> {
  static std::optional<bool> parse(std::string_view text) {
    if (text == "true") {
      return true;
    }
    if (text == "false") {
      return false;
    }
    return std::nullopt;
  }

  static std::string display(bool value) { return value ? "true" : "false"; }
};

template <> struct TypedValueCodec<Symbol> {
  static std::optional<Symbol> parse(std::string_view text) {
    return Symbol(text);
  }

  static std::string display(Symbol value) { return value.str(); }
};

template <> struct TypedValueCodec<std::string> {
  static std::optional<std::string> parse(std::string_view text) {
    return std::string(text);
  }

  static std::string display(const std::string &value) { return value; }
};

} // namespace lotus::egraph

// VARIANTS is an X-macro with entries of the form:
//   X(CONSTANT, Name, "literal", _)
//   X(FIXED, Name, "operator", arity)
//   X(VARIADIC, Name, "operator", _)
//   X(DATA, Name, _, PayloadType)
//   X(DATA_FIXED, Name, _, PayloadTypeAndArity)
//   X(DATA_VARIADIC, Name, _, PayloadType)
// DATA_FIXED uses LOTUS_EGRAPH_TYPED_DATA(PayloadType, arity) as its last
// argument. Payload types containing commas should be introduced through a
// type alias before they are passed to the macro.

#define LOTUS_EGRAPH_TYPED_DATA(TYPE, ARITY) (TYPE, ARITY)
#define LOTUS_EGRAPH_TYPED_DETAIL_UNPAREN(...) __VA_ARGS__
#define LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(MACRO, NAME, DATA)                \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA_EXPAND(                                 \
      MACRO, NAME, LOTUS_EGRAPH_TYPED_DETAIL_UNPAREN DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA_EXPAND(...)                       \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA_I(__VA_ARGS__)
#define LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA_I(MACRO, NAME, TYPE, ARITY)       \
  MACRO(NAME, TYPE, ARITY)

#define LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(PREFIX, KIND, NAME, OP, ARG)        \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH_I(PREFIX, KIND, NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH_I(PREFIX, KIND, NAME, OP, ARG)      \
  PREFIX##_##KIND(NAME, OP, ARG)

#define LOTUS_EGRAPH_TYPED_DETAIL_KIND(KIND, NAME, OP, ARG) NAME,

#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE(KIND, NAME, OP, ARG)                \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE, KIND, \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_CONSTANT(NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_FIXED(NAME, OP, ARITY)
#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_VARIADIC(NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_DATA(NAME, OP, TYPE)                \
  static_assert(                                                               \
      ::lotus::egraph::detail::IsTypedLanguagePayload<TYPE>::value,            \
      #NAME                                                                    \
      " payload must be copyable, equality comparable, ordered, hashable, "   \
      "and must not be a raw floating-point type");
#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_DATA_FIXED(NAME, OP, DATA)          \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_DATA_FIXED_I(NAME, TYPE, ARITY)     \
  LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_DATA(NAME, _, TYPE)
#define LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_DATA_VARIADIC(NAME, OP, TYPE)       \
  LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE_DATA(NAME, OP, TYPE)

#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT(KIND, NAME, OP, ARG)                  \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_STRUCT, KIND,   \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_CONSTANT(NAME, OP, ARG)               \
  struct NAME {                                                                \
    llvm::ArrayRef<Id> children() const { return {}; }                         \
    llvm::MutableArrayRef<Id> childrenMut() { return {}; }                     \
  };
#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_FIXED(NAME, OP, ARITY)                \
  struct NAME {                                                                \
    std::array<Id, ARITY> ids{};                                               \
    llvm::ArrayRef<Id> children() const { return ids; }                        \
    llvm::MutableArrayRef<Id> childrenMut() { return ids; }                    \
  };
#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_VARIADIC(NAME, OP, ARG)               \
  struct NAME {                                                                \
    llvm::SmallVector<Id, 2> ids;                                              \
    llvm::ArrayRef<Id> children() const { return ids; }                        \
    llvm::MutableArrayRef<Id> childrenMut() { return ids; }                    \
  };
#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_DATA(NAME, OP, TYPE)                  \
  struct NAME {                                                                \
    TYPE value;                                                                \
    llvm::ArrayRef<Id> children() const { return {}; }                         \
    llvm::MutableArrayRef<Id> childrenMut() { return {}; }                     \
  };
#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_DATA_FIXED(NAME, OP, DATA)            \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_DATA_FIXED_I(NAME, TYPE, ARITY)       \
  struct NAME {                                                                \
    TYPE value;                                                                \
    std::array<Id, ARITY> ids{};                                               \
    llvm::ArrayRef<Id> children() const { return ids; }                        \
    llvm::MutableArrayRef<Id> childrenMut() { return ids; }                    \
  };
#define LOTUS_EGRAPH_TYPED_DETAIL_STRUCT_DATA_VARIADIC(NAME, OP, TYPE)         \
  struct NAME {                                                                \
    TYPE value;                                                                \
    llvm::SmallVector<Id, 2> ids;                                              \
    llvm::ArrayRef<Id> children() const { return ids; }                        \
    llvm::MutableArrayRef<Id> childrenMut() { return ids; }                    \
  };

#define LOTUS_EGRAPH_TYPED_DETAIL_STORAGE(KIND, NAME, OP, ARG) , NAME

#define LOTUS_EGRAPH_TYPED_DETAIL_CHILDREN(KIND, NAME, OP, ARG)                \
  case Kind::NAME:                                                             \
    return get##NAME().children();

#define LOTUS_EGRAPH_TYPED_DETAIL_CHILDREN_MUT(KIND, NAME, OP, ARG)            \
  case Kind::NAME:                                                             \
    return get##NAME().childrenMut();

#define LOTUS_EGRAPH_TYPED_DETAIL_ACCESSORS(NAME)                              \
  bool is##NAME() const { return std::holds_alternative<NAME>(storage_); }     \
  const NAME &get##NAME() const { return std::get<NAME>(storage_); }           \
  NAME &get##NAME() { return std::get<NAME>(storage_); }

#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY(KIND, NAME, OP, ARG)                 \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_FACTORY, KIND,  \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_CONSTANT(NAME, OP, ARG)              \
  static Self make##NAME() { return Self(NAME{}); }                            \
  LOTUS_EGRAPH_TYPED_DETAIL_ACCESSORS(NAME)
#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_FIXED(NAME, OP, ARITY)               \
  static Self make##NAME(std::array<Id, ARITY> children) {                     \
    return Self(NAME{std::move(children)});                                    \
  }                                                                            \
  static Self make##NAME(std::initializer_list<Id> children) {                 \
    if (children.size() != ARITY) {                                            \
      throw std::invalid_argument("Incorrect child count for " #NAME);         \
    }                                                                          \
    std::array<Id, ARITY> copied{};                                            \
    std::copy(children.begin(), children.end(), copied.begin());               \
    return make##NAME(std::move(copied));                                      \
  }                                                                            \
  LOTUS_EGRAPH_TYPED_DETAIL_ACCESSORS(NAME)
#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_VARIADIC(NAME, OP, ARG)              \
  static Self make##NAME(llvm::ArrayRef<Id> children) {                        \
    NAME value;                                                                \
    value.ids.append(children.begin(), children.end());                        \
    return Self(std::move(value));                                             \
  }                                                                            \
  static Self make##NAME(std::initializer_list<Id> children) {                 \
    return make##NAME(llvm::ArrayRef<Id>(children.begin(), children.size()));  \
  }                                                                            \
  LOTUS_EGRAPH_TYPED_DETAIL_ACCESSORS(NAME)
#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_DATA(NAME, OP, TYPE)                 \
  static Self make##NAME(TYPE value) { return Self(NAME{std::move(value)}); }  \
  LOTUS_EGRAPH_TYPED_DETAIL_ACCESSORS(NAME)
#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_DATA_FIXED(NAME, OP, DATA)           \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_DATA_FIXED_I(NAME, TYPE, ARITY)      \
  static Self make##NAME(TYPE value, std::array<Id, ARITY> children) {         \
    return Self(NAME{std::move(value), std::move(children)});                  \
  }                                                                            \
  static Self make##NAME(TYPE value, std::initializer_list<Id> children) {     \
    if (children.size() != ARITY) {                                            \
      throw std::invalid_argument("Incorrect child count for " #NAME);         \
    }                                                                          \
    std::array<Id, ARITY> copied{};                                            \
    std::copy(children.begin(), children.end(), copied.begin());               \
    return make##NAME(std::move(value), std::move(copied));                    \
  }                                                                            \
  LOTUS_EGRAPH_TYPED_DETAIL_ACCESSORS(NAME)
#define LOTUS_EGRAPH_TYPED_DETAIL_FACTORY_DATA_VARIADIC(NAME, OP, TYPE)        \
  static Self make##NAME(TYPE data, llvm::ArrayRef<Id> children) {             \
    NAME value{std::move(data), {}};                                           \
    value.ids.append(children.begin(), children.end());                        \
    return Self(std::move(value));                                             \
  }                                                                            \
  static Self make##NAME(TYPE data, std::initializer_list<Id> children) {      \
    return make##NAME(std::move(data),                                         \
                      llvm::ArrayRef<Id>(children.begin(), children.size()));  \
  }                                                                            \
  LOTUS_EGRAPH_TYPED_DETAIL_ACCESSORS(NAME)

#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH(KIND, NAME, OP, ARG)                   \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_MATCH, KIND,    \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH_CONSTANT(NAME, OP, ARG)                \
  case Kind::NAME:                                                             \
    return true;
#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH_FIXED(NAME, OP, ARITY)                 \
  case Kind::NAME:                                                             \
    return true;
#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH_VARIADIC(NAME, OP, ARG)                \
  case Kind::NAME:                                                             \
    return get##NAME().ids.size() == other.get##NAME().ids.size();
#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH_DATA(NAME, OP, TYPE)                   \
  case Kind::NAME:                                                             \
    return get##NAME().value == other.get##NAME().value;
#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH_DATA_FIXED(NAME, OP, DATA)             \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_MATCH_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH_DATA_FIXED_I(NAME, TYPE, ARITY)        \
  case Kind::NAME:                                                             \
    return get##NAME().value == other.get##NAME().value;
#define LOTUS_EGRAPH_TYPED_DETAIL_MATCH_DATA_VARIADIC(NAME, OP, TYPE)          \
  case Kind::NAME:                                                             \
    return get##NAME().value == other.get##NAME().value &&                     \
           get##NAME().ids.size() == other.get##NAME().ids.size();

#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL(KIND, NAME, OP, ARG)                   \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_EQUAL, KIND,    \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_CONSTANT(NAME, OP, ARG)                \
  case Kind::NAME:                                                             \
    return true;
#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_FIXED(NAME, OP, ARITY)                 \
  case Kind::NAME:                                                             \
    return get##NAME().ids == other.get##NAME().ids;
#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_VARIADIC(NAME, OP, ARG)                \
  LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_FIXED(NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_DATA(NAME, OP, TYPE)                   \
  case Kind::NAME:                                                             \
    return get##NAME().value == other.get##NAME().value;
#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_DATA_FIXED(NAME, OP, DATA)             \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_DATA_FIXED_I(NAME, TYPE, ARITY)        \
  case Kind::NAME:                                                             \
    return get##NAME().value == other.get##NAME().value &&                     \
           get##NAME().ids == other.get##NAME().ids;
#define LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_DATA_VARIADIC(NAME, OP, TYPE)          \
  LOTUS_EGRAPH_TYPED_DETAIL_EQUAL_DATA_FIXED_I(NAME, TYPE, 0)

#define LOTUS_EGRAPH_TYPED_DETAIL_LESS(KIND, NAME, OP, ARG)                    \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_LESS, KIND,     \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_LESS_CONSTANT(NAME, OP, ARG)                 \
  case Kind::NAME:                                                             \
    return false;
#define LOTUS_EGRAPH_TYPED_DETAIL_LESS_FIXED(NAME, OP, ARITY)                  \
  case Kind::NAME:                                                             \
    return get##NAME().ids < other.get##NAME().ids;
#define LOTUS_EGRAPH_TYPED_DETAIL_LESS_VARIADIC(NAME, OP, ARG)                 \
  LOTUS_EGRAPH_TYPED_DETAIL_LESS_FIXED(NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_LESS_DATA(NAME, OP, TYPE)                    \
  case Kind::NAME:                                                             \
    return get##NAME().value < other.get##NAME().value;
#define LOTUS_EGRAPH_TYPED_DETAIL_LESS_DATA_FIXED(NAME, OP, DATA)              \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_LESS_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_LESS_DATA_FIXED_I(NAME, TYPE, ARITY)         \
  case Kind::NAME: {                                                           \
    const auto &lhs_value = get##NAME();                                       \
    const auto &rhs_value = other.get##NAME();                                 \
    if (!(lhs_value.value == rhs_value.value)) {                               \
      return lhs_value.value < rhs_value.value;                                \
    }                                                                          \
    return lhs_value.ids < rhs_value.ids;                                      \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_LESS_DATA_VARIADIC(NAME, OP, TYPE)           \
  LOTUS_EGRAPH_TYPED_DETAIL_LESS_DATA_FIXED_I(NAME, TYPE, 0)

#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE(KIND, NAME, OP, ARG)                   \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_PARSE, KIND,    \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_CONSTANT(NAME, OP, ARG)                \
  if (op == OP && children.empty()) {                                          \
    return make##NAME();                                                       \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_FIXED(NAME, OP, ARITY)                 \
  if (op == OP && children.size() == ARITY) {                                  \
    std::array<Id, ARITY> copied{};                                            \
    std::copy(children.begin(), children.end(), copied.begin());               \
    return make##NAME(std::move(copied));                                      \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIADIC(NAME, OP, ARG)                \
  if (op == OP) {                                                              \
    return make##NAME(llvm::ArrayRef<Id>(children));                           \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_DATA(NAME, OP, TYPE)                   \
  if (children.empty()) {                                                      \
    auto parsed = ::lotus::egraph::TypedValueCodec<TYPE>::parse(op);           \
    if (parsed) {                                                              \
      return make##NAME(std::move(*parsed));                                   \
    }                                                                          \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_DATA_FIXED(NAME, OP, DATA)             \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_PARSE_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_DATA_FIXED_I(NAME, TYPE, ARITY)        \
  if (children.size() == ARITY) {                                              \
    auto parsed = ::lotus::egraph::TypedValueCodec<TYPE>::parse(op);           \
    if (parsed) {                                                              \
      std::array<Id, ARITY> copied{};                                          \
      std::copy(children.begin(), children.end(), copied.begin());             \
      return make##NAME(std::move(*parsed), std::move(copied));                \
    }                                                                          \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_DATA_VARIADIC(NAME, OP, TYPE)          \
  if (auto parsed = ::lotus::egraph::TypedValueCodec<TYPE>::parse(op)) {       \
    return make##NAME(std::move(*parsed), llvm::ArrayRef<Id>(children));       \
  }

#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT(KIND, NAME, OP, ARG)           \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(                                           \
      LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT, KIND, NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_CONSTANT(NAME, OP, ARG)        \
  if (variant == #NAME) {                                                      \
    return op == OP && children.empty() ? std::optional<Self>(make##NAME())    \
                                         : std::nullopt;                       \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_FIXED(NAME, OP, ARITY)         \
  if (variant == #NAME) {                                                      \
    if (op != OP || children.size() != ARITY) {                                \
      return std::nullopt;                                                     \
    }                                                                          \
    std::array<Id, ARITY> copied{};                                            \
    std::copy(children.begin(), children.end(), copied.begin());               \
    return make##NAME(std::move(copied));                                      \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_VARIADIC(NAME, OP, ARG)        \
  if (variant == #NAME) {                                                      \
    return op == OP ? std::optional<Self>(                                     \
                          make##NAME(llvm::ArrayRef<Id>(children)))             \
                    : std::nullopt;                                            \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_DATA(NAME, OP, TYPE)           \
  if (variant == #NAME) {                                                      \
    if (!children.empty()) {                                                   \
      return std::nullopt;                                                     \
    }                                                                          \
    auto parsed = ::lotus::egraph::TypedValueCodec<TYPE>::parse(op);           \
    return parsed ? std::optional<Self>(make##NAME(std::move(*parsed)))        \
                  : std::nullopt;                                              \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_DATA_FIXED(NAME, OP, DATA)     \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_DATA_FIXED_I(NAME, TYPE,       \
                                                             ARITY)            \
  if (variant == #NAME) {                                                      \
    if (children.size() != ARITY) {                                            \
      return std::nullopt;                                                     \
    }                                                                          \
    auto parsed = ::lotus::egraph::TypedValueCodec<TYPE>::parse(op);           \
    if (!parsed) {                                                             \
      return std::nullopt;                                                     \
    }                                                                          \
    std::array<Id, ARITY> copied{};                                            \
    std::copy(children.begin(), children.end(), copied.begin());               \
    return make##NAME(std::move(*parsed), std::move(copied));                  \
  }
#define LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT_DATA_VARIADIC(NAME, OP, TYPE) \
  if (variant == #NAME) {                                                      \
    auto parsed = ::lotus::egraph::TypedValueCodec<TYPE>::parse(op);           \
    return parsed ? std::optional<Self>(make##NAME(                            \
                        std::move(*parsed), llvm::ArrayRef<Id>(children)))      \
                  : std::nullopt;                                              \
  }

#define LOTUS_EGRAPH_TYPED_DETAIL_VARIANT_NAME(KIND, NAME, OP, ARG)            \
  case Kind::NAME:                                                             \
    return #NAME;

#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY(KIND, NAME, OP, ARG)                 \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY, KIND,  \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_CONSTANT(NAME, OP, ARG)              \
  case Kind::NAME:                                                             \
    return OP;
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_FIXED(NAME, OP, ARITY)               \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_CONSTANT(NAME, OP, ARITY)
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_VARIADIC(NAME, OP, ARG)              \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_CONSTANT(NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_DATA(NAME, OP, TYPE)                 \
  case Kind::NAME:                                                             \
    return ::lotus::egraph::TypedValueCodec<TYPE>::display(get##NAME().value);
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_DATA_FIXED(NAME, OP, DATA)           \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_DATA_FIXED_I(NAME, TYPE, ARITY)      \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_DATA(NAME, _, TYPE)
#define LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_DATA_VARIADIC(NAME, OP, TYPE)        \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY_DATA(NAME, OP, TYPE)

#define LOTUS_EGRAPH_TYPED_DETAIL_HASH(KIND, NAME, OP, ARG)                    \
  LOTUS_EGRAPH_TYPED_DETAIL_DISPATCH(LOTUS_EGRAPH_TYPED_DETAIL_HASH, KIND,     \
                                     NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_HASH_CONSTANT(NAME, OP, ARG)                 \
  case Kind::NAME:                                                             \
    break;
#define LOTUS_EGRAPH_TYPED_DETAIL_HASH_FIXED(NAME, OP, ARITY)                  \
  case Kind::NAME:                                                             \
    for (Id child : get##NAME().ids) {                                         \
      ::lotus::egraph::hashCombine(seed, child);                               \
    }                                                                          \
    break;
#define LOTUS_EGRAPH_TYPED_DETAIL_HASH_VARIADIC(NAME, OP, ARG)                 \
  LOTUS_EGRAPH_TYPED_DETAIL_HASH_FIXED(NAME, OP, ARG)
#define LOTUS_EGRAPH_TYPED_DETAIL_HASH_DATA(NAME, OP, TYPE)                    \
  case Kind::NAME:                                                             \
    ::lotus::egraph::hashCombine(seed, get##NAME().value);                     \
    break;
#define LOTUS_EGRAPH_TYPED_DETAIL_HASH_DATA_FIXED(NAME, OP, DATA)              \
  LOTUS_EGRAPH_TYPED_DETAIL_APPLY_DATA(                                        \
      LOTUS_EGRAPH_TYPED_DETAIL_HASH_DATA_FIXED_I, NAME, DATA)
#define LOTUS_EGRAPH_TYPED_DETAIL_HASH_DATA_FIXED_I(NAME, TYPE, ARITY)         \
  case Kind::NAME:                                                             \
    ::lotus::egraph::hashCombine(seed, get##NAME().value);                     \
    for (Id child : get##NAME().ids) {                                         \
      ::lotus::egraph::hashCombine(seed, child);                               \
    }                                                                          \
    break;
#define LOTUS_EGRAPH_TYPED_DETAIL_HASH_DATA_VARIADIC(NAME, OP, TYPE)           \
  LOTUS_EGRAPH_TYPED_DETAIL_HASH_DATA_FIXED_I(NAME, TYPE, 0)

#define LOTUS_EGRAPH_TYPED_DETAIL_DEFINE_CLASS(NAME, VARIANTS)                 \
  class NAME {                                                                 \
    struct StorageSentinel {};                                                 \
                                                                               \
  public:                                                                      \
    using Self = NAME;                                                         \
    using Id = ::lotus::egraph::Id;                                            \
    enum class Kind : uint32_t {                                               \
      VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_KIND)                                 \
    };                                                                         \
    using Discriminant = Kind;                                                 \
    VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_STRUCT)                                 \
    VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_VALIDATE)                               \
    NAME() = delete;                                                           \
    VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_FACTORY)                                \
    Kind kind() const {                                                        \
      const size_t index = storage_.index();                                   \
      if (index == 0 || index == std::variant_npos) {                           \
        throw std::logic_error("Typed language has no active variant");        \
      }                                                                        \
      return static_cast<Kind>(index - 1);                                     \
    }                                                                          \
    Discriminant discriminant() const { return kind(); }                       \
    llvm::ArrayRef<Id> children() const {                                      \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_CHILDREN)                           \
      }                                                                        \
      throw std::logic_error("Unknown typed language variant");                \
    }                                                                          \
    llvm::MutableArrayRef<Id> childrenMut() {                                  \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_CHILDREN_MUT)                       \
      }                                                                        \
      throw std::logic_error("Unknown typed language variant");                \
    }                                                                          \
    template <typename F> Self mapChildren(F &&fn) const {                     \
      Self copy = *this;                                                       \
      for (Id &child : copy.childrenMut()) {                                   \
        child = fn(child);                                                     \
      }                                                                        \
      return copy;                                                             \
    }                                                                          \
    bool matches(const Self &other) const {                                    \
      if (kind() != other.kind()) {                                            \
        return false;                                                          \
      }                                                                        \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_MATCH)                              \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
    bool operator==(const Self &other) const {                                 \
      if (kind() != other.kind()) {                                            \
        return false;                                                          \
      }                                                                        \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_EQUAL)                              \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
    bool operator!=(const Self &other) const { return !(*this == other); }     \
    bool operator<(const Self &other) const {                                  \
      if (kind() != other.kind()) {                                            \
        return static_cast<uint32_t>(kind()) <                                 \
               static_cast<uint32_t>(other.kind());                            \
      }                                                                        \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_LESS)                               \
      }                                                                        \
      return false;                                                            \
    }                                                                          \
    static std::optional<Self> fromOp(std::string_view op,                     \
                                      const std::vector<Id> &children) {       \
      VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_PARSE)                                \
      return std::nullopt;                                                     \
    }                                                                          \
    static std::optional<Self>                                                 \
    fromSerializedVariant(std::string_view variant, std::string_view op,       \
                          const std::vector<Id> &children) {                    \
      VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_PARSE_VARIANT)                        \
      return std::nullopt;                                                     \
    }                                                                          \
    std::string_view variantName() const {                                     \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_VARIANT_NAME)                       \
      }                                                                        \
      throw std::logic_error("Unknown typed language variant");                \
    }                                                                          \
    std::string display() const {                                              \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_DISPLAY)                            \
      }                                                                        \
      throw std::logic_error("Unknown typed language variant");                \
    }                                                                          \
    size_t hash() const {                                                      \
      size_t seed = std::hash<uint32_t>{}(static_cast<uint32_t>(kind()));      \
      switch (kind()) {                                                        \
        VARIANTS(LOTUS_EGRAPH_TYPED_DETAIL_HASH)                               \
      }                                                                        \
      return seed;                                                             \
    }                                                                          \
                                                                               \
  private:                                                                     \
    using Storage = std::variant<StorageSentinel VARIANTS(                     \
        LOTUS_EGRAPH_TYPED_DETAIL_STORAGE)>;                                   \
    template <typename Variant, typename = std::enable_if_t<!std::is_same_v<   \
                                    std::decay_t<Variant>, Self>>>             \
    explicit NAME(Variant &&variant)                                           \
        : storage_(std::in_place_type<std::decay_t<Variant>>,                  \
                   std::forward<Variant>(variant)) {}                          \
    Storage storage_;                                                          \
  };

#define LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE(NAME, VARIANTS)                     \
  LOTUS_EGRAPH_TYPED_DETAIL_DEFINE_CLASS(NAME, VARIANTS)

#define LOTUS_EGRAPH_DEFINE_TYPED_LANGUAGE_IN(NAMESPACE, NAME, VARIANTS)       \
  namespace NAMESPACE {                                                        \
  LOTUS_EGRAPH_TYPED_DETAIL_DEFINE_CLASS(NAME, VARIANTS)                       \
  }

template <> struct std::hash<lotus::egraph::DynamicLangDiscriminant> {
  size_t operator()(
      const lotus::egraph::DynamicLangDiscriminant &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op);
    lotus::egraph::hashCombine(seed, value.arity);
    return seed;
  }
};

template <> struct std::hash<lotus::egraph::DynamicLang> {
  size_t operator()(const lotus::egraph::DynamicLang &value) const noexcept {
    size_t seed = std::hash<lotus::egraph::Symbol>{}(value.op());
    for (lotus::egraph::Id child : value.children()) {
      lotus::egraph::hashCombine(seed, child);
    }
    return seed;
  }
};
