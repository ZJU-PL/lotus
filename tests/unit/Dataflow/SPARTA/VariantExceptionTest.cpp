/* Copyright (c) Meta Platforms, Inc. and affiliates. MIT license. */
#include <sparta/ConstantAbstractDomain.h>
#include <sparta/DisjointUnionAbstractDomain.h>
#include <gtest/gtest.h>
#include <stdexcept>

namespace {
class ThrowingDomain final : public sparta::AbstractDomain<ThrowingDomain> {
public:
  inline static bool fail_copy = false;
  inline static bool fail_move = false;
  ThrowingDomain() = default;
  explicit ThrowingDomain(int value) : m_value(value) {}
  explicit ThrowingDomain(sparta::AbstractValueKind kind) : m_value(kind) {}
  ThrowingDomain(const ThrowingDomain &other) : m_value(other.m_value) {
    if (fail_copy)
      throw std::runtime_error("copy failed");
  }
  ThrowingDomain(ThrowingDomain &&other) noexcept(false)
      : m_value(other.m_value) {
    if (fail_move)
      throw std::runtime_error("move failed");
  }
  ThrowingDomain &operator=(const ThrowingDomain &other) {
    if (fail_copy)
      throw std::runtime_error("copy assignment failed");
    m_value = other.m_value;
    return *this;
  }
  ThrowingDomain &operator=(ThrowingDomain &&other) noexcept(false) {
    if (fail_move)
      throw std::runtime_error("move assignment failed");
    m_value = other.m_value;
    return *this;
  }
  static ThrowingDomain top() {
    return ThrowingDomain(sparta::AbstractValueKind::Top);
  }
  static ThrowingDomain bottom() {
    return ThrowingDomain(sparta::AbstractValueKind::Bottom);
  }
  bool is_top() const { return m_value.is_top(); }
  bool is_bottom() const { return m_value.is_bottom(); }
  bool leq(const ThrowingDomain &other) const {
    return m_value.leq(other.m_value);
  }
  bool equals(const ThrowingDomain &other) const {
    return m_value.equals(other.m_value);
  }
  void set_to_top() { m_value.set_to_top(); }
  void set_to_bottom() { m_value.set_to_bottom(); }
  void join_with(const ThrowingDomain &other) {
    m_value.join_with(other.m_value);
  }
  void widen_with(const ThrowingDomain &other) {
    m_value.widen_with(other.m_value);
  }
  void meet_with(const ThrowingDomain &other) {
    m_value.meet_with(other.m_value);
  }
  void narrow_with(const ThrowingDomain &other) {
    m_value.narrow_with(other.m_value);
  }
  auto get_constant() const { return m_value.get_constant(); }
  friend std::ostream &operator<<(std::ostream &output,
                                  const ThrowingDomain &domain) {
    return output << domain.m_value;
  }

private:
  sparta::ConstantAbstractDomain<int> m_value;
};
using Sum =
    sparta::DisjointUnionAbstractDomain<sparta::ConstantAbstractDomain<int>,
                                        ThrowingDomain>;

TEST(SpartaVariantExceptionTest, failedCrossAlternativeCopyRetainsValidDomain) {
  Sum destination = Sum::bottom();
  const Sum source{ThrowingDomain(42)};
  ThrowingDomain::fail_copy = true;
  EXPECT_THROW(destination.join_with(source), std::runtime_error);
  ThrowingDomain::fail_copy = false;
  EXPECT_TRUE(destination.is_bottom());
  EXPECT_EQ(source.get<ThrowingDomain>().get_constant().value(), 42);
  destination.join_with(source);
  EXPECT_EQ(destination.get<ThrowingDomain>().get_constant().value(), 42);
  auto copy = destination;
  copy.set_to_top();
  EXPECT_FALSE(destination.is_top());
}

TEST(SpartaVariantExceptionTest, movingThrowingAlternativeUsesCopyFallback) {
  Sum destination = Sum::bottom();
  Sum source{ThrowingDomain(42)};
  ThrowingDomain::fail_move = true;
  EXPECT_NO_THROW(destination = std::move(source));
  ThrowingDomain::fail_move = false;
  EXPECT_EQ(destination.get<ThrowingDomain>().get_constant().value(), 42);
  EXPECT_EQ(source.get<ThrowingDomain>().get_constant().value(), 42);
}

TEST(SpartaVariantExceptionTest,
     failedCopyFallbackDuringMoveRetainsValidDomain) {
  Sum destination = Sum::bottom();
  Sum source{ThrowingDomain(42)};
  ThrowingDomain::fail_copy = true;
  EXPECT_THROW(destination = std::move(source), std::runtime_error);
  ThrowingDomain::fail_copy = false;
  EXPECT_TRUE(destination.is_bottom());
  EXPECT_EQ(source.get<ThrowingDomain>().get_constant().value(), 42);
}
} // namespace
