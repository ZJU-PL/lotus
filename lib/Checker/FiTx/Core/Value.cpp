#include "llvm/IR/Value.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include "Checker/FiTx/Core/AnalysisHelper.h"
#include "Checker/FiTx/Core/Casting.h"
#include "Checker/FiTx/Core/Instructions.h"
#include "Checker/FiTx/Core/SFG/Converter.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Core/Value.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fitx {
Value::Value(llvm::Value *value, std::vector<Fields> fields,
             long array_element_num)
    : value_(value), value_type_(value ? value->getValueID() : 0),
      array_element_num_(array_element_num),
      is_global_var_(value ? llvm::isa<llvm::GlobalValue>(value) : false),
      is_return_value_(false), fields_(std::move(fields)) {}

Value::Value(std::shared_ptr<fitx::Value> value, std::vector<Fields> fields,
             long array_element_num)
    : value_(value->value_), value_type_(value->getValueID()),
      array_element_num_(array_element_num),
      is_global_var_(value->isGlobalVar()),
      is_return_value_(value->is_return_value_), fields_(std::move(fields)) {}

Value::Value(const Value &value)
    : value_(value.value_), value_type_(value.getValueID()),
      array_element_num_(value.array_element_num_),
      is_global_var_(value.is_global_var_),
      is_return_value_(value.is_return_value_), fields_(value.fields_) {}

Value::Value(std::shared_ptr<Value> value) {
  value_ = value->value_;
  array_element_num_ = value->array_element_num_;
  fields_ = value->fields_;
  is_global_var_ = value->isGlobalVar();
  is_return_value_ = value->is_return_value_;
  value_type_ = value->getValueID();
}

std::shared_ptr<Value> Value::CreateFromDefinition(llvm::Value *value) {
  return fitx::Converter::GetInstance().Convert(value);
}

std::shared_ptr<Value> Value::CreateAppend(std::shared_ptr<Value> src,
                                           std::shared_ptr<Value> target) {
  if (!src)
    return target;
  if (!target)
    return src;

  const auto &src_fields = src->GetFields();
  const auto &tgt_fields = target->GetFields();
  auto src_end = src_fields.end();
  auto target_front = tgt_fields.begin();

  if (!src_fields.empty() && !tgt_fields.empty()) {
    auto source_back = src_fields.end() - 1;
    llvm::Type *type = nullptr;
    if (source_back->FieldType() == target_front->ElementType() &&
        !target_front->FieldType()) {
      target_front++;
    }
    while (target_front < tgt_fields.end() &&
           source_back->ElementType() == target_front->ElementType() &&
           source_back->FieldType() != target_front->type) {
      if (type && type->isPointerTy() &&
          type->getPointerElementType() == source_back->type &&
          type->getPointerElementType() == target_front->type) {
        target_front++;
      }
      type = source_back->type;
      if (source_back == src_fields.begin()) {
        src_end = src_fields.begin();
        break;
      }
      --source_back;
      src_end = source_back + 1;
    }
  }

  auto new_fields = std::vector<Fields>(src_fields.begin(), src_end);
  new_fields.insert(new_fields.end(), target_front, tgt_fields.end());

  long array_element_num =
      std::max(target->ArrayElementNum(), src->ArrayElementNum());

  if (auto managed = Converter::GetInstance().getManagedValue(
          src->rawLLVMValue(), array_element_num, new_fields)) {
    return managed;
  }

  if (auto call_inst = shared_dyn_cast<fitx::CallInst>(src))
    return fitx::CallInst::Create(call_inst, new_fields, array_element_num);

  if (auto inst = shared_dyn_cast<fitx::Instruction>(src))
    return fitx::Instruction::Create(inst, new_fields, array_element_num);

  std::shared_ptr<fitx::Value> value;
  switch (src->getValueID()) {
  case llvm::Value::ConstantIntVal:
    value = std::make_shared<fitx::ConstValue>(
        fitx::shared_dyn_cast<fitx::ConstValue>(src));
    break;
  case llvm::Value::ConstantPointerNullVal:
    value = std::make_shared<fitx::NullValue>(
        fitx::shared_dyn_cast<fitx::NullValue>(src));
    break;
  case llvm::Value::ArgumentVal:
    value = std::make_shared<fitx::Argument>(
        fitx::shared_dyn_cast<fitx::Argument>(src), new_fields,
        array_element_num);
    break;
  default:
    value = std::make_shared<fitx::Value>(src, new_fields, array_element_num);
  }

  Converter::GetInstance().manageValue(src->rawLLVMValue(), value);
  return value;
}

bool Value::operator<(const Value &V) const {
  if (value_ != V.value_)
    return value_ < V.value_;

  if (array_element_num_ != V.array_element_num_)
    return array_element_num_ < V.array_element_num_;

  if (fields_.size() != V.fields_.size())
    return fields_.size() < V.fields_.size();

  if (value_type_ != V.value_type_)
    return value_type_ < V.value_type_;

  for (size_t i = 0; i < fields_.size(); i++) {
    if (fields_[i].type != V.fields_[i].type)
      return fields_[i].type < V.fields_[i].type;

    if (fields_[i].field != V.fields_[i].field)
      return fields_[i].field < V.fields_[i].field;
  }

  return false;
}

bool Value::operator==(const Value &V) const {
  if (value_ != V.value_ || fields_.size() != V.fields_.size() ||
      array_element_num_ != V.array_element_num_ ||
      value_type_ != V.value_type_)
    return false;

  for (size_t i = 0; i < fields_.size(); i++) {
    if (fields_[i].type != V.fields_[i].type ||
        fields_[i].field != V.fields_[i].field)
      return false;
  }
  return true;
}

bool Value::operator==(const llvm::Value *V) const { return value_ == V; }

llvm::Value &Value::getLLVMValue_() const { return *value_; }

llvm::Type &Value::getLLVMType_() const {
  const Fields &field = fields_.back();
  if (field.field >= 0 && field.type && field.type->isStructTy() &&
      static_cast<unsigned>(field.field) < field.type->getStructNumElements())
    return *field.type->getStructElementType(
        static_cast<unsigned>(field.field));
  return *field.type;
}

long Value::Field() const {
  return fields_.empty() ? kNonFieldVariable : fields_.back().field;
}

const std::vector<Value::Fields> &Value::GetFields() const { return fields_; };

bool Value::isArgument() { return value_ && llvm::isa<llvm::Argument>(value_); }

bool Value::isGlobalVar() { return is_global_var_; }

bool Value::isReturnValue() { return is_return_value_; }
void Value::setReturnValue(bool is_return_value) {
  is_return_value_ = is_return_value;
}

long Value::ArrayElementNum() { return array_element_num_; }

ConstValue::ConstValue(llvm::ConstantInt *value)
    : Value(value),
      const_value_(
          value && value->getBitWidth() <= 64
              ? value->getSExtValue()
              : (value ? static_cast<int64_t>(value->getValue().getLimitedValue())
                       : 0)) {}

ConstValue::ConstValue(int64_t const_value)
    : Value(llvm::Value::ConstantIntVal), const_value_(const_value) {}

ConstValue::ConstValue(std::shared_ptr<ConstValue> value)
    : Value(value), const_value_(value->getConstValue()) {}

NullValue::NullValue(llvm::ConstantPointerNull *value) : Value(value) {}
NullValue::NullValue(std::shared_ptr<fitx::NullValue> value) : Value(value) {}

Argument::Argument(llvm::Argument *argument, std::vector<Value::Fields> fields,
                   long array_element_num)
    : Value(argument, std::move(fields), array_element_num),
      arg_num_(argument->getArgNo()) {}

Argument::Argument(std::shared_ptr<Argument> argument,
                   std::vector<Value::Fields> fields, long array_element_num)
    : Value(argument->rawLLVMValue(), std::move(fields), array_element_num),
      arg_num_(argument->ArgNum()) {}

/* Value Collection Class*/
ValueCollection ValueCollection::createFromIntersection(
    const std::vector<ValueCollection> &collections) {
  if (collections.empty())
    return ValueCollection();

  ValueCollection result = *collections.begin();
  for (auto collection = collections.begin() + 1;
       collection != collections.end(); collection++) {
    ValueCollection tmp;
    for (const auto &v : result.Values()) {
      if (collection->values_.find(v) != collection->values_.end())
        tmp.add(v);
    }
    result = std::move(tmp);
  }
  return result;
}

ValueCollection ValueCollection::createFromUnion(
    const std::vector<ValueCollection> &collections) {
  if (collections.empty())
    return ValueCollection();

  ValueCollection result = *collections.begin();
  for (auto collection = collections.begin() + 1;
       collection != collections.end(); collection++) {
    result.add(*collection);
  }
  return result;
}

bool ValueCollection::add(std::shared_ptr<Value> value) {
  if (!value)
    return false;
  if (!values_.insert(value).second)
    return false;
  by_llvm_value_[value->rawLLVMValue()].push_back(value);
  return true;
}

void ValueCollection::add(const ValueCollection &value_collection) {
  for (const auto &v : value_collection.Values()) {
    add(v);
  }
}

void ValueCollection::remove(std::shared_ptr<Value> value) {
  if (!value || !values_.erase(value))
    return;
  auto it = by_llvm_value_.find(value->rawLLVMValue());
  if (it != by_llvm_value_.end()) {
    auto &vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), value), vec.end());
    if (vec.empty())
      by_llvm_value_.erase(it);
  }
}

void ValueCollection::clear() {
  values_.clear();
  by_llvm_value_.clear();
}

bool ValueCollection::exists(fitx::Value value) {
  auto it = by_llvm_value_.find(value.rawLLVMValue());
  if (it == by_llvm_value_.end())
    return false;
  for (const auto &compared_value : it->second) {
    if (*compared_value == value)
      return true;
  }
  return false;
}

bool ValueCollection::exists(std::shared_ptr<Value> value) {
  return values_.find(value) != values_.end();
}

// Same base LLVM value and compatible field path (same or more specific);
// used to apply store/branch transitions to all SSA/field variants (paper §3).
std::set<std::shared_ptr<Value>>
ValueCollection::getRelatedValues(std::shared_ptr<Value> value) const {
  std::set<std::shared_ptr<Value>> related_values;
  llvm::SmallVector<std::shared_ptr<Value>, 8> buf;
  collectRelatedValues(value, buf);
  related_values.insert(buf.begin(), buf.end());
  return related_values;
}

void ValueCollection::collectRelatedValues(
    const std::shared_ptr<Value> &value,
    llvm::SmallVectorImpl<std::shared_ptr<Value>> &out) const {
  if (!value)
    return;
  auto it = by_llvm_value_.find(value->rawLLVMValue());
  if (it == by_llvm_value_.end())
    return;

  const auto &val_fields = value->GetFields();
  const size_t val_field_size = val_fields.size();
  for (const auto &new_value : it->second) {
    const auto &new_fields = new_value->GetFields();
    if (new_fields.size() >= val_field_size &&
        (val_fields.empty() ||
         val_fields.back().field == Value::kNonFieldVariable ||
         val_fields.back().field == new_fields[val_field_size - 1].field)) {
      out.push_back(new_value);
    }
  }
}

std::set<std::shared_ptr<Value>>
ValueCollection::getParentValues(std::shared_ptr<Value> value) const {
  std::set<std::shared_ptr<Value>> related_values;
  llvm::SmallVector<std::shared_ptr<Value>, 8> buf;
  collectParentValues(value, buf);
  related_values.insert(buf.begin(), buf.end());
  return related_values;
}

void ValueCollection::collectParentValues(
    const std::shared_ptr<Value> &value,
    llvm::SmallVectorImpl<std::shared_ptr<Value>> &out) const {
  if (!value)
    return;
  auto it = by_llvm_value_.find(value->rawLLVMValue());
  if (it == by_llvm_value_.end())
    return;

  const size_t val_field_size = value->GetFields().size();
  for (const auto &new_value : it->second) {
    if (new_value->GetFields().size() <= val_field_size) {
      out.push_back(new_value);
    }
  }
}

AliasValues::AliasValues() : alias_size_(0) {}

// Record may-alias: both directions so getAliasInfo works for either value.
void AliasValues::addAlias(std::shared_ptr<fitx::Value> src,
                           std::shared_ptr<fitx::Value> target) {
  if (alias_info_[src].add(target)) {
    updated_values_.push_back(src);
    alias_size_++;
  }

  if (alias_info_[target].add(src)) {
    updated_values_.push_back(target);
    alias_size_++;
  }
}

void AliasValues::addAlias(AliasValues &collection) {
  if (alias_size_ >= collection.Size())
    return;

  const std::vector<std::shared_ptr<fitx::Value>> &updated =
      collection.UpdatedValues();

  if (!updated.empty()) {
    for (auto value : updated) {
      if (const auto *info = collection.getAliasInfo(value)) {
        alias_info_[value].add(*info);
        alias_size_ += alias_info_[value].size();
      }
    }
    return;
  }

  for (auto alias_info : collection.AliasInfo()) {
    alias_info_[alias_info.first].add(alias_info.second);
    alias_size_ += alias_info_[alias_info.first].size();
  }
}

const ValueCollection *
AliasValues::getAliasInfo(std::shared_ptr<fitx::Value> value) {
  auto it = alias_info_.find(value);
  if (it != alias_info_.end())
    return &it->second;
  return nullptr;
}

ManagedValues &ManagedValues::GetInstance() {
  static ManagedValues *managed_values = new ManagedValues();
  return *managed_values;
}

ManagedValues::ManagedValues() { managed_values_.reserve(kReserveSize); }

std::shared_ptr<fitx::Value> ManagedValues::getValueFromID(size_t id) {
  if (id >= Size())
    return nullptr;
  return managed_values_[id];
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                              const fitx::Value &value) {
  ostream << "[fitx::Value] ";
  ostream << "ValueType: " << value.value_type_ << " ";
  ostream << "Array Element: " << value.array_element_num_ << " ";
  ostream << "(";
  for (const auto &field : value.fields_) {
    ostream << "{Type: ";
    if (!field.type) {
      ostream << "<null>";
    } else if (auto *struct_ty = llvm::dyn_cast<llvm::StructType>(field.type)) {
      if (!struct_ty->isLiteral())
        ostream << struct_ty->getName();
      else
        ostream << *struct_ty;
    } else {
      ostream << *field.type;
    }
    ostream << " Field: " << field.field << "}, ";
  }
  ostream << ")";

  return ostream;
}

}; // namespace fitx
