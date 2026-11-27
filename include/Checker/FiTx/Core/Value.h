/// \file Value.h
/// \brief FiTx value representation with field-sensitive tracking.
#pragma once
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Value.h"

#include <map>
#include <queue>
#include <set>
#include <vector>

namespace fitx {

class Instruction;
class Value {
public:
  constexpr static int kNonFieldVariable = -1;
  constexpr static int kNonArrayElement = -2;
  constexpr static int kArbitaryArrayElement = -1;

  /// Static field offset: struct field or constant array index (paper §3).
  struct Fields {
    Fields(llvm::Type *type, long field = kNonFieldVariable)
        : type(type), field(field) {}
    llvm::Type *type;
    long field;
    bool operator==(const Fields &value) const {
      return type == value.type && field == value.field;
    }

    bool operator!=(const Fields &value) const {
      if (type == value.type)
        return field != value.field;
      return type != value.type;
    }
    llvm::Type *ElementType() const {
      if (!type)
        return nullptr;
      return type->isPointerTy() ? type->getPointerElementType() : type;
    }

    llvm::Type *FieldType() const {
      llvm::Type *element = ElementType();
      if (field < 0 || !element || !element->isStructTy() ||
          static_cast<unsigned>(field) >= element->getStructNumElements())
        return nullptr;
      return element->getStructElementType(static_cast<unsigned>(field));
    }
  };
  using FieldList = std::vector<Fields>;

  // factory
  static std::shared_ptr<Value> CreateFromDefinition(llvm::Value *value);
  static std::shared_ptr<Value> CreateAppend(std::shared_ptr<Value> src,
                                             std::shared_ptr<Value> target);
  static std::shared_ptr<Value>
  CreateManagedValue(std::shared_ptr<fitx::Instruction> value);

  Value(llvm::Value *value, std::vector<Fields> fields, long array_element_num);
  Value(std::shared_ptr<fitx::Value> value, std::vector<Fields> fields,
        long array_element_num);
  Value(const Value &value);
  Value &operator=(const Value &value) = default;

  Value(unsigned value_type = 0)
      : value_(nullptr), value_type_(value_type),
        array_element_num_(kNonArrayElement), is_global_var_(false),
        is_return_value_(false), fields_(std::vector<Fields>()) {}

  Value(llvm::Value *value)
      : value_(value), value_type_(value ? value->getValueID() : 0),
        array_element_num_(kNonArrayElement),
        is_global_var_(value ? llvm::isa<llvm::GlobalValue>(value) : false),
        is_return_value_(false), fields_(std::vector<Fields>()) {}

  Value(std::shared_ptr<Value> value);

  bool operator<(const Value &V) const;
  bool operator==(const Value &V) const;
  bool operator==(const llvm::Value *V) const;
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                                       const fitx::Value &value);

  llvm::Value &getLLVMValue_() const;
  llvm::Value *rawLLVMValue() const { return value_; }
  llvm::Type &getLLVMType_() const;

  bool isArgument();
  bool isGlobalVar();
  bool isReturnValue();
  void setReturnValue(bool is_return_value);

  long ArrayElementNum();
  bool isArbitaryArrayElement() {
    return array_element_num_ == kArbitaryArrayElement;
  }

  long Field() const;
  const std::vector<Fields> &GetFields() const;

  unsigned getValueID() const { return value_type_; }

  bool isRoot() const { return fields_.empty(); }

  void addUser(std::shared_ptr<fitx::Value> user) { users_.push_back(user); }

  std::vector<std::weak_ptr<fitx::Value>> Users() { return users_; }

  void setManagedId(size_t id) { managed_id_ = id; }
  size_t ManagedId() { return managed_id_; }

private:
  size_t managed_id_ = 0;

  llvm::Value *value_;

  unsigned value_type_;

  long array_element_num_;
  bool is_global_var_;
  bool is_return_value_;
  std::vector<Fields> fields_;
  std::vector<std::weak_ptr<fitx::Value>> users_;
};

class ConstValue : public Value {
public:
  ConstValue(llvm::ConstantInt *value);
  ConstValue(int64_t const_value);
  ConstValue(std::shared_ptr<ConstValue> value);

  int64_t getConstValue() { return const_value_; }

  static bool classof(const fitx::Value *value) {
    return value->getValueID() == llvm::Value::ConstantIntVal;
  }

private:
  std::int64_t const_value_;
};

class NullValue : public Value {
public:
  NullValue(llvm::ConstantPointerNull *value);
  NullValue(std::shared_ptr<NullValue> value);

  static bool classof(const fitx::Value *value) {
    return value->getValueID() == llvm::Value::ConstantPointerNullVal;
  }
};

class Argument : public Value {
public:
  Argument(llvm::Argument *argument, std::vector<Value::Fields> fields,
           long array_element_num);

  Argument(std::shared_ptr<Argument> argument,
           std::vector<Value::Fields> fields, long array_element_num);

  uint64_t ArgNum() const { return arg_num_; }

  static bool classof(const fitx::Value *value) {
    return value->getValueID() == llvm::Value::ArgumentVal;
  }

private:
  uint64_t arg_num_;
};

/// Set of framework values with O(1)-indexed SSA/field "related" lookup.
class ValueCollection {
public:
  ValueCollection() = default;

  static ValueCollection
  createFromIntersection(const std::vector<ValueCollection> &collections);
  static ValueCollection
  createFromUnion(const std::vector<ValueCollection> &collections);

  const std::set<std::shared_ptr<Value>> &Values() const { return values_; };

  bool exists(fitx::Value value);
  bool exists(std::shared_ptr<Value> val);

  bool add(std::shared_ptr<Value> val);

  void add(const ValueCollection &collection);
  void remove(std::shared_ptr<Value> val);
  void clear();

  std::set<std::shared_ptr<Value>>
  getRelatedValues(std::shared_ptr<Value> value) const;

  void collectRelatedValues(
      const std::shared_ptr<Value> &value,
      llvm::SmallVectorImpl<std::shared_ptr<Value>> &out) const;

  std::set<std::shared_ptr<Value>>
  getParentValues(std::shared_ptr<Value> value) const;

  void collectParentValues(
      const std::shared_ptr<Value> &value,
      llvm::SmallVectorImpl<std::shared_ptr<Value>> &out) const;

  size_t size() const { return values_.size(); }

private:
  std::set<std::shared_ptr<Value>> values_;
  llvm::DenseMap<const llvm::Value *, std::vector<std::shared_ptr<Value>>>
      by_llvm_value_;
};

class AliasValues {
public:
  AliasValues();

  void addAlias(std::shared_ptr<fitx::Value> src,
                std::shared_ptr<fitx::Value> target);

  void addAlias(AliasValues &collection);

  const ValueCollection *getAliasInfo(std::shared_ptr<fitx::Value> value);

  const std::vector<std::shared_ptr<fitx::Value>> &UpdatedValues() {
    return updated_values_;
  };

  const std::map<std::shared_ptr<fitx::Value>, ValueCollection> &AliasInfo() {
    return alias_info_;
  };

  size_t Size() { return alias_size_; }

  void clear() {
    alias_info_.clear();
    updated_values_.clear();
    alias_size_ = 0;
  }

private:
  std::map<std::shared_ptr<fitx::Value>, ValueCollection> alias_info_;

  std::vector<std::shared_ptr<fitx::Value>> updated_values_;
  size_t alias_size_;
};

class ManagedValues {
public:
  static ManagedValues &GetInstance();
  constexpr static size_t kReserveSize = 10000;

  size_t Size() { return managed_values_.size(); }

  void addValue(std::shared_ptr<fitx::Value> value) {
    value->setManagedId(Size());
    return managed_values_.push_back(value);
  }

  std::shared_ptr<fitx::Value> getValueFromID(size_t id);
  void clear() { managed_values_.clear(); }

private:
  ManagedValues();
  std::vector<std::shared_ptr<fitx::Value>> managed_values_;
};

}; // namespace fitx
