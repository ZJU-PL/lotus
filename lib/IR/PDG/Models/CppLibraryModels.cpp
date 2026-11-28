#include "IR/PDG/Models/CppLibraryModels.h"

#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DerivedTypes.h"

#include "IR/PDG/Models/ValueFacts.h"

#include <cstdlib>
#include <set>
#include <unordered_map>

using namespace llvm;
namespace pdg {
namespace {

struct Name {
  std::string method;
  std::string context;
  bool constructor = false;
  bool destructor = false;
  Optional<unsigned> template_character_bits;
};

Optional<Name> name(const Function &function) {
  StringRef raw = function.getName();
  if (!raw.startswith("_Z") && !raw.startswith("___Z"))
    return None;
  std::string encoded = raw.str();
  static std::unordered_map<std::string, Optional<Name>> cache;
  auto cached = cache.find(encoded);
  if (cached != cache.end())
    return cached->second;
  if (cache.size() >= 8192)
    cache.clear();
  auto store = [&](Optional<Name> val) -> Optional<Name> {
    cache.emplace(encoded, val);
    return val;
  };
  ItaniumPartialDemangler demangler;
  if (demangler.partialDemangle(encoded.c_str()) || !demangler.isFunction())
    return store(None);
  char *base = demangler.getFunctionBaseName(nullptr, nullptr);
  char *context = demangler.getFunctionDeclContextName(nullptr, nullptr);
  if (!base || !context) {
    std::free(base);
    std::free(context);
    return store(None);
  }
  Name result{base, context, demangler.isCtorOrDtor() && base[0] != '~',
              demangler.isCtorOrDtor() && base[0] == '~'};
  char *full = demangler.getFunctionName(nullptr, nullptr);
  std::string complete = full ? full : "";
  std::free(full);
  std::free(base);
  std::free(context);
  // A member template has its template arguments on the base name. Removing
  // a balanced specialization suffix does not change the declaring class.
  auto angle = result.method.find('<');
  if (angle != std::string::npos)
    result.method.resize(angle);
  // getFunctionBaseName omits template arguments. The complete function name
  // retains operator+'s CharT specialization, including wide characters.
  size_t plus = complete.find("operator+");
  if (plus != std::string::npos) {
    size_t template_start = complete.find('<', plus);
    if (template_start != std::string::npos) {
      StringRef argument = StringRef(complete)
                               .drop_front(template_start + 1)
                               .split(',')
                               .first.trim();
      if (argument == "char" || argument == "signed char" ||
          argument == "unsigned char" || argument == "char8_t")
        result.template_character_bits = 8;
      else if (argument == "char16_t")
        result.template_character_bits = 16;
      else if (argument == "char32_t")
        result.template_character_bits = 32;
      else if (argument == "wchar_t")
        result.template_character_bits = 0;
    }
  }
  // libc++ gives many defined header methods ABI tags such as
  // c_str[abi:ne180100]; the tags do not change the API's parameter roles.
  for (size_t tag = result.method.find("[abi:"); tag != std::string::npos;
       tag = result.method.find("[abi:")) {
    size_t end = result.method.find(']', tag);
    if (end == std::string::npos)
      return store(None);
    result.method.erase(tag, end - tag + 1);
  }
  return store(result);
}

bool namespacePrefix(StringRef &context) {
  if (!context.consume_front("std::"))
    return context.consume_front("bsl::");
  // Accept documented inline ABI namespaces, not arbitrary nested classes.
  for (StringRef abi : {"__1::", "__2::", "__cxx11::", "__ndk1::", "__8::"})
    if (context.consume_front(abi))
      break;
  return true;
}

bool freeNamespace(StringRef context) {
  return context == "std" || context == "bsl" || context == "std::__1" ||
         context == "std::__2" || context == "std::__cxx11" ||
         context == "std::__ndk1" || context == "std::__8";
}

Optional<unsigned> characterWidth(StringRef context) {
  if (!namespacePrefix(context))
    return None;
  // Itanium's legacy Ss substitution is printed as std::string for some
  // member contexts, while its constructors print the complete specialization.
  if (context == "string" || context == "u8string")
    return 8;
  if (context == "wstring")
    return 0;
  if (context == "u16string")
    return 16;
  if (context == "u32string")
    return 32;
  if (!context.consume_front("basic_string<"))
    return None;
  // The declaring context must end at this class specialization. For example,
  // basic_string<...>::helper::append is a different function.
  unsigned level = 1;
  size_t close = 0;
  for (; close < context.size() && level; ++close) {
    if (context[close] == '<')
      ++level;
    else if (context[close] == '>')
      --level;
  }
  if (level || close != context.size())
    return None;
  StringRef type = context.split(',').first.trim();
  if (type.endswith(">"))
    type = type.drop_back().trim();
  if (type == "char" || type == "signed char" || type == "unsigned char" ||
      type == "char8_t")
    return 8;
  if (type == "char16_t")
    return 16;
  if (type == "char32_t")
    return 32;
  // wchar_t ABI width is checked against actual character pointer parameters;
  // zero denotes either the usual i16 or i32 representation.
  if (type == "wchar_t")
    return 0;
  return None;
}

bool stringPointer(Type *type) {
  if (!type->isPointerTy())
    return false;
  const auto *record = dyn_cast<StructType>(type->getPointerElementType());
  if (!record || !record->hasName())
    return false;
  StringRef context = record->getName();
  context.consume_front("class.");
  context.consume_front("struct.");
  StringRef original = context;
  if (!namespacePrefix(context))
    return false;
  if (context == "string" || context == "wstring" || context == "u8string" ||
      context == "u16string" || context == "u32string")
    return true;
  // LLVM identified structs omit template arguments and may gain numeric
  // suffixes for other specializations of the same C++ template.
  if (context.consume_front("basic_string")) {
    if (context.empty())
      return true;
    if (context.consume_front("."))
      return !context.empty() &&
             context.find_first_not_of("0123456789") == StringRef::npos;
    return characterWidth(original) != None;
  }
  return false;
}

bool character(Type *type, unsigned bits) {
  return type->isIntegerTy() &&
         (bits ? type->getIntegerBitWidth() == bits
               : type->isIntegerTy(16) || type->isIntegerTy(32));
}
bool stringInput(Type *type, unsigned bits) {
  return stringPointer(type) ||
         (type->isPointerTy() &&
          character(type->getPointerElementType(), bits));
}
TaintEndpoint scalar(int argument) {
  return {argument, TaintChannel::Value, false};
}

Optional<unsigned> returnedObject(const CallBase &call) {
  const Function *target = ValueFacts::callee(call);
  for (unsigned i = 0; i < call.arg_size(); ++i)
    if (call.paramHasAttr(i, Attribute::StructRet) ||
        (target && target->hasParamAttribute(i, Attribute::StructRet)))
      if (stringPointer(call.getArgOperand(i)->getType()))
        return i;
  return None;
}
bool zero(const CallBase &call, unsigned argument) {
  if (argument >= call.arg_size())
    return false;
  auto value = ValueFacts::integer(*call.getArgOperand(argument));
  return value && value->isZero();
}

struct Builder {
  const CallBase &call;
  unsigned bits;
  CallTaintModel model;
  Builder(const CallBase &call, unsigned bits) : call(call), bits(bits) {
    model.known = true;
  }
  TaintEndpoint contents(int argument) const {
    Type *type =
        argument < 0
            ? call.getType()
            : call.getArgOperand(static_cast<unsigned>(argument))->getType();
    return {argument,
            stringPointer(type) ? TaintChannel::ObjectContent
                                : TaintChannel::Memory,
            false};
  }
  void flow(TaintEndpoint from, TaintEndpoint to, bool concatenates = false,
            bool numeric = false) {
    model.transfers.push_back({from, to, concatenates, numeric, false});
  }
  void aliasReturn(unsigned owner) {
    if (call.getType()->isPointerTy())
      flow(contents(owner), contents(-1));
  }
  std::vector<TaintEndpoint> inputs(unsigned first) {
    std::vector<TaintEndpoint> result;
    for (unsigned i = first; i < call.arg_size(); ++i) {
      Type *type = call.getArgOperand(i)->getType();
      if (stringInput(type, bits))
        result.push_back(contents(i));
      else if (character(type, bits))
        result.push_back(scalar(i));
    }
    return result;
  }
  bool emptyInput(unsigned first) {
    // A zero count in a counted constructor/assign/append makes the supplied
    // characters irrelevant. Allocator arguments and positions are distinct.
    if (first >= call.arg_size())
      return false;
    for (unsigned i = first; i < call.arg_size(); ++i) {
      Type *type = call.getArgOperand(i)->getType();
      if (character(type, bits) && i > first &&
          call.getArgOperand(i - 1)->getType()->isIntegerTy() &&
          !character(call.getArgOperand(i - 1)->getType(), bits))
        return zero(call, i - 1); // fill character preceded by count
      if (stringInput(type, bits) && i + 1 < call.arg_size() &&
          call.getArgOperand(i + 1)->getType()->isIntegerTy()) {
        if (stringPointer(type)) {
          if (i + 2 < call.arg_size() &&
              call.getArgOperand(i + 2)->getType()->isIntegerTy())
            return zero(call, i + 2); // substring position, count
        } else {
          return zero(call, i + 1); // character pointer, count
        }
      }
    }
    return false;
  }
};

} // namespace

bool CppLibraryModels::isStringObject(const Value &value) {
  return stringPointer(value.getType());
}

Optional<unsigned> CppLibraryModels::returnAlias(const CallBase &call) {
  const Function *target = ValueFacts::callee(call);
  if (!target || call.arg_empty() || !stringPointer(call.getType()) ||
      !stringPointer(call.getArgOperand(0)->getType()))
    return None;
  auto qualified = name(*target);
  if (!qualified || !characterWidth(qualified->context))
    return None;
  if (qualified->method == "assign" || qualified->method == "append" ||
      qualified->method == "operator=" || qualified->method == "operator+=")
    return 0;
  return None;
}

Optional<CallTaintModel> CppLibraryModels::taint(const CallBase &call) {
  const Function *target = ValueFacts::callee(call);
  if (!target)
    return None;
  auto qualified = name(*target);
  if (!qualified)
    return None;
  const std::string &method = qualified->method;
  auto sret = returnedObject(call);

  if (freeNamespace(qualified->context) &&
      (method == "to_string" || method == "to_wstring")) {
    if (!sret || call.arg_size() != 2)
      return None;
    unsigned input = *sret == 0 ? 1 : 0;
    Type *type = call.getArgOperand(input)->getType();
    if (!type->isIntegerTy() && !type->isFloatingPointTy())
      return None;
    Builder builder(call, method == "to_string" ? 8 : 0);
    builder.model.overwrites.push_back(builder.contents(*sret));
    // Preserve numeric provenance for derived sizes while keeping it outside
    // text-content taint. Ordinary content transfers reject numeric facts.
    builder.flow(scalar(input), builder.contents(*sret), false, true);
    return builder.model;
  }

  if (freeNamespace(qualified->context) && method == "operator+") {
    // Real basic_string returns use sret. Mock empty C++ classes may erase the
    // result completely (a void call with no output argument); no destination
    // is guessed from nearby instructions or source locations.
    if (!sret || call.arg_size() != 3)
      return None;
    unsigned lhs = *sret == 0 ? 1 : 0;
    unsigned rhs = *sret == 2 ? 1 : 2;
    if (lhs == *sret || rhs == *sret || lhs == rhs ||
        (!stringPointer(call.getArgOperand(lhs)->getType()) &&
         !stringPointer(call.getArgOperand(rhs)->getType())))
      return None;
    if (!qualified->template_character_bits)
      return None;
    unsigned bits = *qualified->template_character_bits;
    Builder builder(call, bits);
    builder.model.overwrites.push_back(builder.contents(*sret));
    for (unsigned i : {lhs, rhs}) {
      Type *type = call.getArgOperand(i)->getType();
      if (stringInput(type, bits))
        builder.flow(builder.contents(i), builder.contents(*sret), i == rhs);
      else if (character(type, bits))
        builder.flow(scalar(i), builder.contents(*sret), i == rhs);
      else
        return None;
    }
    return builder.model;
  }

  auto bits = characterWidth(qualified->context);
  if (!bits)
    return None;
  unsigned owner = sret && *sret == 0 ? 1 : 0;
  if (owner >= call.arg_size() ||
      !stringPointer(call.getArgOperand(owner)->getType()))
    return None;
  unsigned first = owner + 1;
  Builder builder(call, *bits);

  if (qualified->constructor || method == "assign" || method == "operator=") {
    builder.model.overwrites.push_back(builder.contents(owner));
    if (!builder.emptyInput(first))
      for (TaintEndpoint source : builder.inputs(first)) {
        builder.flow(source, builder.contents(owner));
        if (call.getType()->isPointerTy())
          builder.flow(source, builder.contents(-1));
      }
    return builder.model;
  }
  if (qualified->destructor || method == "clear" ||
      (method == "resize" && zero(call, first))) {
    builder.model.overwrites.push_back(builder.contents(owner));
    return builder.model;
  }
  if (method == "append" || method == "operator+=" || method == "push_back" ||
      method == "insert" || method == "replace" || method == "resize") {
    if (!builder.emptyInput(first))
      for (TaintEndpoint source : builder.inputs(first)) {
        builder.flow(source, builder.contents(owner), true);
        if (call.getType()->isPointerTy())
          builder.flow(source, builder.contents(-1), true);
      }
    builder.aliasReturn(owner);
    return builder.model;
  }
  if (method == "c_str" || method == "data" || method == "front" ||
      method == "back" || method == "at" || method == "operator[]") {
    if (!call.getType()->isPointerTy() ||
        !character(call.getType()->getPointerElementType(), *bits))
      return None;
    builder.flow(builder.contents(owner), builder.contents(-1));
    return builder.model;
  }
  if (method == "size" || method == "length" || method == "capacity") {
    if (!call.getType()->isIntegerTy() || first != call.arg_size())
      return None;
    builder.flow(builder.contents(owner), scalar(-1), false, true);
    return builder.model;
  }
  if (method == "substr") {
    if (!sret)
      return None;
    builder.model.overwrites.push_back(builder.contents(*sret));
    if (!zero(call, first + 1))
      builder.flow(builder.contents(owner), builder.contents(*sret));
    return builder.model;
  }
  if (method == "copy") {
    if (first >= call.arg_size() ||
        !stringInput(call.getArgOperand(first)->getType(), *bits))
      return None;
    if (!zero(call, first + 1))
      builder.flow(builder.contents(owner), builder.contents(first));
    return builder.model;
  }
  if (method == "swap") {
    if (first >= call.arg_size() ||
        !stringPointer(call.getArgOperand(first)->getType()))
      return None;
    // Transfers are evaluated on pre-call facts, so each destination receives
    // the opposite object's previous contents before strong overwrites apply.
    builder.model.overwrites = {builder.contents(owner),
                                builder.contents(first)};
    builder.flow(builder.contents(owner), builder.contents(first));
    builder.flow(builder.contents(first), builder.contents(owner));
    return builder.model;
  }
  if (method == "erase") {
    auto count = first + 1 < call.arg_size()
                     ? ValueFacts::integer(*call.getArgOperand(first + 1))
                     : None;
    bool whole = first == call.arg_size() ||
                 (zero(call, first) && count && count->isAllOnes());
    if (whole)
      builder.model.overwrites.push_back(builder.contents(owner));
    else
      builder.aliasReturn(owner);
    return builder.model;
  }
  if (method == "reserve" || method == "shrink_to_fit" || method == "empty" ||
      method == "max_size" || method == "get_allocator" ||
      method == "compare" || method == "find" || method == "rfind" ||
      method == "find_first_of" || method == "find_last_of" ||
      method == "find_first_not_of" || method == "find_last_not_of")
    return builder.model;
  return None;
}
} // namespace pdg
