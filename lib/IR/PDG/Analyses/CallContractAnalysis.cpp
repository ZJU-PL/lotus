#include "IR/PDG/Analyses/CallContractAnalysis.h"

#include "llvm/ADT/Triple.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/InstIterator.h"

#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"

#include <algorithm>
#include <map>
#include <set>

using namespace llvm;

namespace pdg {
namespace {

struct FormatAbi {
  unsigned pointer_bits;
  unsigned int_bits;
  unsigned long_bits;
  unsigned wide_bits;
  bool microsoft;
  bool known;
  bool long_double_is_double;

  explicit FormatAbi(const Module &module)
      : pointer_bits(module.getDataLayout().getPointerSizeInBits()),
        int_bits(32), long_bits(0), wide_bits(0), microsoft(false),
        known(false), long_double_is_double(false) {
    Triple triple(module.getTargetTriple());
    // LLVM integers erase C typedefs, but the target ABI retains their widths.
    // Without a target, only directives whose size is independent of it are
    // checked; a host ABI is never substituted for the analyzed module's ABI.
    known = triple.getArch() != Triple::UnknownArch;
    microsoft = triple.isOSWindows();
    if (triple.getArch() == Triple::avr || triple.getArch() == Triple::msp430)
      int_bits = 16;
    long_double_is_double =
        microsoft || triple.getArch() == Triple::arm ||
        triple.getArch() == Triple::thumb ||
        (triple.isOSDarwin() && (triple.getArch() == Triple::aarch64 ||
                                 triple.getArch() == Triple::aarch64_be));
    if (known) {
      long_bits = microsoft ? 32 : std::max(32u, pointer_bits);
      wide_bits = microsoft ? 16 : 32;
    }
    if (auto size = ValueFacts::macroInteger(module, "__SIZEOF_LONG__"))
      long_bits = *size * 8;
    if (auto size = ValueFacts::macroInteger(module, "__SIZEOF_INT__"))
      int_bits = *size * 8;
    if (auto size = ValueFacts::macroInteger(module, "__SIZEOF_WCHAR_T__"))
      wide_bits = *size * 8;
  }
};

Optional<unsigned> integerBits(StringRef length, const FormatAbi &abi,
                               bool pointer) {
  if (length.empty())
    return abi.int_bits;
  if (length == "h")
    return pointer ? 16 : abi.int_bits;
  if (length == "hh")
    return pointer ? 8 : abi.int_bits;
  if (length == "l")
    return abi.long_bits ? Optional<unsigned>(abi.long_bits) : None;
  if (length == "ll" || length == "L" || length == "q" || length == "j" ||
      length == "I64")
    return 64;
  if (length == "I32")
    return 32;
  if (length == "z" || length == "t" || length == "I")
    return abi.known ? Optional<unsigned>(abi.pointer_bits) : None;
  return None;
}

// A disengaged result means that a precise expectation is unavailable.
Optional<bool> formatTypeMatches(Type *actual,
                                 const FormatFacts::Conversion &directive,
                                 const FormatModel &model,
                                 const FormatAbi &abi) {
  char code = directive.conversion;
  StringRef length = directive.length;
  if (StringRef("diouxX").contains(code)) {
    auto bits = integerBits(length, abi, false);
    return bits ? Optional<bool>(actual->isIntegerTy(*bits)) : None;
  }
  if (StringRef("aAeEfFgG").contains(code)) {
    if (length.empty() || length == "l")
      return actual->isDoubleTy(); // C default argument promotion of float.
    if (length == "L" || length == "ll") {
      if (!abi.known)
        return None;
      return abi.long_double_is_double
                 ? actual->isDoubleTy()
                 : actual->isX86_FP80Ty() || actual->isFP128Ty() ||
                       actual->isPPC_FP128Ty();
    }
    return None;
  }
  if (code == 'p')
    return actual->isPointerTy();
  if (code == 'n') {
    auto bits = integerBits(length, abi, true);
    if (!bits)
      return None;
    if (!actual->isPointerTy())
      return false;
    auto *pointer = cast<PointerType>(actual);
    return pointer->isOpaque()
               ? Optional<bool>()
               : Optional<bool>(
                     pointer->getPointerElementType()->isIntegerTy(*bits));
  }
  if (code == 'c' || code == 'C') {
    if (length.empty() || length == "h" || length == "l" || length == "w")
      return actual->isIntegerTy(abi.int_bits) ||
             ((length == "l" || length == "w" || code == 'C') &&
              abi.wide_bits && actual->isIntegerTy(abi.wide_bits));
    return None;
  }
  if (code == 's' || code == 'S') {
    bool wide = code == 'S';
    if (abi.microsoft && model.wide)
      wide = !wide;
    if (length == "l" || length == "w")
      wide = true;
    else if (length == "h")
      wide = false;
    else if (!length.empty())
      return None;
    unsigned bits = wide ? abi.wide_bits : 8;
    if (!bits)
      return None;
    if (!actual->isPointerTy())
      return false;
    auto *pointer = cast<PointerType>(actual);
    if (model.wide && length.empty() && !pointer->isOpaque()) {
      // The upstream query accepts the alternate Microsoft/POSIX default
      // character type for wide printf functions when no length is explicit.
      Type *element = pointer->getPointerElementType();
      if (element->isIntegerTy(8) ||
          (abi.wide_bits && element->isIntegerTy(abi.wide_bits)))
        return true;
    }
    return pointer->isOpaque()
               ? Optional<bool>()
               : Optional<bool>(
                     pointer->getPointerElementType()->isIntegerTy(bits));
  }
  return None;
}

void addFinding(CallContractAnalysisResult &result, StringRef id,
                const Instruction &site, std::string message,
                const std::vector<const Instruction *> &evidence = {}) {
  result.findings.push_back({id.str(), &site, std::move(message), evidence});
}

void analyzeFormat(const CallBase &call, const Function &target,
                   const FormatAbi &abi, CallContractAnalysisResult &result) {
  auto model = LibraryModels::format(target);
  // CodeQL's wrong-type-format-argument is for printf-style formatters;
  // scanf destination pointees have separate contracts and are not this ID.
  if (!model || model->scanf || model->format >= call.arg_size())
    return;
  auto text = ValueFacts::constantString(*call.getArgOperand(model->format));
  if (!text)
    return;
  auto facts = parseFormat(*text, false);
  if (!facts)
    return;
  std::set<unsigned> wrong;
  auto argument = [&](unsigned index) -> const Value * {
    if (!index || index - 1 + model->first_argument >= call.arg_size())
      return nullptr;
    return call.getArgOperand(index - 1 + model->first_argument);
  };
  for (const auto &conversion : facts->conversions) {
    if (const auto *value = argument(conversion.argument)) {
      auto matches =
          formatTypeMatches(value->getType(), conversion, *model, abi);
      if (matches && !*matches)
        wrong.insert(conversion.argument);
    }
    for (unsigned index :
         {conversion.width_argument, conversion.precision_argument})
      if (const auto *value = argument(index))
        if (!value->getType()->isIntegerTy(abi.int_bits))
          wrong.insert(index);
  }
  if (!wrong.empty()) {
    std::string indices;
    for (unsigned index : wrong) {
      if (!indices.empty())
        indices += ", ";
      indices += std::to_string(index);
    }
    addFinding(result, "cpp/wrong-type-format-argument", call,
               "Promoted argument(s) " + indices +
                   " do not match the format conversion or width/precision ABI "
                   "contract.");
  }
}

bool isC(const Function &function) {
  const auto *subprogram = function.getSubprogram();
  if (!subprogram || !subprogram->getUnit())
    return false;
  unsigned language = subprogram->getUnit()->getSourceLanguage();
  return language == dwarf::DW_LANG_C || language == dwarf::DW_LANG_C89 ||
         language == dwarf::DW_LANG_C99 || language == dwarf::DW_LANG_C11;
}

Optional<bool> compatibleArgumentTypes(Type *actual, Type *expected,
                                       const DataLayout &layout,
                                       unsigned nesting = 0) {
  if (actual == expected)
    return true;
  if (actual->isIntegerTy() && expected->isIntegerTy())
    return actual->getIntegerBitWidth() == expected->getIntegerBitWidth();
  if (actual->isFloatingPointTy() && expected->isFloatingPointTy())
    return actual->getPrimitiveSizeInBits() ==
           expected->getPrimitiveSizeInBits();
  if (actual->isPointerTy() && expected->isPointerTy()) {
    auto *a = cast<PointerType>(actual);
    auto *e = cast<PointerType>(expected);
    if (a->isOpaque() || e->isOpaque())
      return None;
    Type *ae = a->getPointerElementType(), *ee = e->getPointerElementType();
    // Clang represents void* as i8*. This also deliberately leaves char/void
    // pointer distinctions unknown when LLVM erased them.
    if (ae->isIntegerTy(8) || ee->isIntegerTy(8))
      return true;
    if (nesting < 2)
      return compatibleArgumentTypes(ae, ee, layout, nesting + 1);
    return None;
  }
  if (nesting && (actual->isArrayTy() || expected->isArrayTy())) {
    Type *a = actual->isArrayTy() ? actual->getArrayElementType() : actual;
    Type *e =
        expected->isArrayTy() ? expected->getArrayElementType() : expected;
    return compatibleArgumentTypes(a, e, layout, nesting);
  }
  // Struct identity and source qualifiers cannot be recovered from a lowered
  // byte pointer; avoid conflating them with an arithmetic mismatch.
  if (actual->isStructTy() || expected->isStructTy())
    return None;
  return false;
}

void analyzeResolvedSignature(const CallBase &call, const Function &target,
                              CallContractAnalysisResult &result) {
  if (target.isVarArg() || target.isIntrinsic() || target.isDeclaration() ||
      !isC(*call.getFunction()) ||
      call.getFunctionType() == target.getFunctionType())
    return;
  // An old-style C call retained as a signature cast supplies authoritative
  // target parameters only when a definition exists. Ordinary prototyped
  // calls and unresolved declarations are outside this retained-IR subset.
  if (!call.getCalledOperand()->getType()->isPointerTy() ||
      call.getCalledOperand() == &target)
    return;
  if (call.arg_size() < target.arg_size())
    addFinding(result, "cpp/too-few-arguments", call,
               "Resolved C callee requires " +
                   std::to_string(target.arg_size()) +
                   " arguments; this call supplies " +
                   std::to_string(call.arg_size()) + ".");
  else if (call.arg_size() > target.arg_size())
    addFinding(result, "cpp/futile-params", call,
               "Resolved nonvariadic C callee has " +
                   std::to_string(target.arg_size()) +
                   " parameters; this call supplies " +
                   std::to_string(call.arg_size()) + ".");
  std::vector<unsigned> incompatible;
  for (unsigned i = 0; i < std::min<size_t>(call.arg_size(), target.arg_size());
       ++i) {
    auto matches =
        compatibleArgumentTypes(call.getArgOperand(i)->getType(),
                                target.getFunctionType()->getParamType(i),
                                target.getParent()->getDataLayout());
    if (matches && !*matches)
      incompatible.push_back(i + 1);
  }
  if (!incompatible.empty())
    addFinding(
        result, "cpp/mistyped-function-arguments", call,
        "C argument " + std::to_string(incompatible.front()) +
            " has an ABI type incompatible with the resolved parameter.");
}

Optional<int> sentinel(const Value &value) {
  if (isa<ConstantPointerNull>(value))
    return 0;
  if (const auto *floating = dyn_cast<ConstantFP>(&value)) {
    if (floating->isZero())
      return 0;
    if (floating->getValueAPF().isExactlyValue(-1.0))
      return -1;
  }
  if (auto integer = ValueFacts::integer(value)) {
    if (integer->isZero())
      return 0;
    if (integer->isAllOnes())
      return -1;
  }
  return None;
}

void analyzeSentinels(const Function &target,
                      const std::vector<const CallBase *> &calls,
                      CallContractAnalysisResult &result) {
  if (!target.isVarArg() || LibraryModels::format(target))
    return;
  for (const char *name : {"open", "fcntl", "ptrace", "mremap"})
    if (ValueFacts::hasLibraryName(target, name))
      return;
  for (int end : {0, -1}) {
    std::vector<const Instruction *> supporting;
    bool used_inside = false;
    for (const auto *call : calls) {
      if (call->arg_size()) {
        if (sentinel(*call->getArgOperand(call->arg_size() - 1)) == end)
          supporting.push_back(call);
        for (unsigned i = target.arg_size(); i + 1 < call->arg_size(); ++i)
          used_inside |= sentinel(*call->getArgOperand(i)) == end;
      }
    }
    if (used_inside || supporting.empty() ||
        100 * supporting.size() / calls.size() < 80)
      continue;
    for (const auto *call : calls)
      if (!call->arg_size() ||
          sentinel(*call->getArgOperand(call->arg_size() - 1)) != end)
        addFinding(result, "cpp/unterminated-variadic-call", *call,
                   "Variadic callee uses " + std::to_string(end) +
                       " as its terminal argument in " +
                       std::to_string(supporting.size()) + " of " +
                       std::to_string(calls.size()) +
                       " module calls, but this call does not.",
                   supporting);
  }
}

} // namespace

const std::vector<AnalysisRuleDescriptor> &CallContractAnalysis::catalog() {
  static const std::vector<AnalysisRuleDescriptor> rules = {
      {"cpp/wrong-type-format-argument", "error",
       "Likely Bugs/Format/WrongTypeFormatArguments.ql",
       "Known printf-family literal directives, promoted "
       "integer/floating/pointer types, positional and star arguments, target "
       "ABI lengths, and wide strings; custom formatter attributes and erased "
       "source distinctions remain unknown."},
      {"cpp/unterminated-variadic-call", "warning",
       "Security/CWE/CWE-121/UnterminatedVarargsCall.ql",
       "Resolved module call sites infer 0/-1 termination using the original "
       "80-percent threshold, exclude nonterminal sentinel uses and API "
       "whitelist; cross-module calls are outside the observed convention."},
      {"cpp/too-few-arguments", "error",
       "Likely Bugs/Underspecified Functions/TooFewArguments.ql",
       "Retained C signature-cast calls to resolved nonvariadic definitions "
       "with too few arguments; conflicting declaration histories unavailable "
       "in LLVM remain unsupported."},
      {"cpp/futile-params", "warning",
       "Likely Bugs/Underspecified Functions/TooManyArguments.ql",
       "Retained C signature-cast calls to resolved nonvariadic definitions "
       "with excess arguments; unresolved external declarations remain "
       "unsupported."},
      {"cpp/mistyped-function-arguments", "warning",
       "Likely Bugs/Underspecified Functions/MistypedFunctionArguments.ql",
       "Retained C signature-cast calls with incompatible numeric or "
       "typed-pointer arguments; erased char/void and struct source types "
       "remain unknown."}};
  return rules;
}

CallContractAnalysisResult CallContractAnalysis::analyze(const Module &module) const {
  CallContractAnalysisResult result;
  FormatAbi abi(module);
  std::map<const Function *, std::vector<const CallBase *>> calls;
  for (const auto &function : module)
    for (const auto &instruction : instructions(function))
      if (const auto *call = dyn_cast<CallBase>(&instruction))
        if (const auto *target = ValueFacts::callee(*call)) {
          calls[target].push_back(call);
          analyzeFormat(*call, *target, abi, result);
          analyzeResolvedSignature(*call, *target, result);
        }
  for (const auto &entry : calls)
    analyzeSentinels(*entry.first, entry.second, result);
  return result;
}

} // namespace pdg
