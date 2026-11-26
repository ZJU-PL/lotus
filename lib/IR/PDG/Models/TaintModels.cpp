#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/KnownBits.h"

#include "IR/PDG/Models/CppLibraryModels.h"
#include "IR/PDG/Models/LibraryModels.h"
#include "IR/PDG/Models/ValueFacts.h"

using namespace llvm;
namespace pdg {
namespace {
TaintEndpoint memory(int argument) { return {argument, TaintChannel::Memory}; }
TaintEndpoint value(int argument) { return {argument, TaintChannel::Value}; }
bool named(const Function &function,
           std::initializer_list<const char *> names) {
  for (const char *name : names)
    if (ValueFacts::hasLibraryName(function, name, true, true))
      return true;
  return false;
}
Optional<uint64_t> argumentCount(const CallBase &call, unsigned index) {
  const Value *argument = ValueFacts::argument(call, index);
  auto integer = argument ? ValueFacts::integer(*argument) : None;
  if (!integer || integer->isNegative() || integer->getActiveBits() > 64)
    return None;
  return integer->getZExtValue();
}
} // namespace

CallTaintModel LibraryModels::taint(const CallBase &call) {
  if (auto cpp = CppLibraryModels::taint(call))
    return *cpp;
  CallTaintModel model;
  const Function *target = ValueFacts::callee(call);
  if (!target || !target->isDeclaration())
    return model;
  auto pipe = [&](TaintEndpoint input, TaintEndpoint output,
                  bool concat = false, bool numeric = false, bool sql = false) {
    model.transfers.push_back({input, output, concat, numeric, sql});
  };
  if (named(*target, {"getenv", "secure_getenv", "_wgetenv"})) {
    model.known = true;
    model.sources = {memory(-1)};
    return model;
  }
  if (named(*target, {"fgets", "gets", "fgetws"})) {
    model.known = true;
    if (!named(*target, {"gets"}) && argumentCount(call, 1) &&
        *argumentCount(call, 1) == 0)
      return model;
    model.sources = {memory(0), memory(-1)};
    return model;
  }
  if (named(*target, {"read", "pread", "recv", "recvfrom"})) {
    model.known = true;
    if (argumentCount(call, 2) && *argumentCount(call, 2) == 0)
      return model;
    model.sources = {memory(1)};
    return model;
  }
  if (named(*target, {"fread"})) {
    model.known = true;
    if ((argumentCount(call, 1) && *argumentCount(call, 1) == 0) ||
        (argumentCount(call, 2) && *argumentCount(call, 2) == 0))
      return model;
    model.sources = {memory(0)};
    return model;
  }
  auto format_model = format(*target);
  if (format_model && format_model->scanf) {
    model.known = true;
    bool parses_string =
        named(*target, {"sscanf", "swscanf", "__isoc99_sscanf"});
    const Value *arg = ValueFacts::argument(call, format_model->format);
    auto text = arg ? ValueFacts::constantString(*arg) : None;
    auto parsed = text ? parseFormat(*text, true) : None;
    std::vector<std::pair<unsigned, bool>> outputs;
    if (parsed)
      for (const auto &conversion : parsed->conversions)
        outputs.push_back(
            {format_model->first_argument + conversion.argument - 1,
             !StringRef("sScC[").contains(conversion.conversion)});
    else
      for (unsigned i = format_model->first_argument; i < call.arg_size(); ++i)
        outputs.push_back({i, false});
    for (auto entry : outputs) {
      unsigned output = entry.first;
      if (output >= call.arg_size() ||
          !call.getArgOperand(output)->getType()->isPointerTy())
        continue;
      auto endpoint = memory(output);
      endpoint.numeric = entry.second;
      if (parses_string)
        pipe(memory(0), endpoint);
      else
        model.sources.push_back(endpoint);
    }
    return model;
  }
  if (named(*target, {"strcpy", "wcscpy", "stpcpy"}) || copy(*target)) {
    model.known = true;
    auto copy_model = copy(*target);
    unsigned source = copy_model ? copy_model->source : 1;
    unsigned dest = copy_model ? copy_model->destination : 0;
    bool whole = !copy_model;
    if (copy_model) {
      auto size = argumentCount(call, copy_model->size);
      const Value *arg = ValueFacts::argument(call, dest);
      auto capacity = arg ? ValueFacts::objectBytes(*arg) : None;
      if (size && *size == 0)
        return model;
      unsigned char_bytes = 1;
      if (target->arg_size() > dest &&
          target->getFunctionType()->getParamType(dest)->isPointerTy()) {
        Type *element = target->getFunctionType()
                            ->getParamType(dest)
                            ->getPointerElementType();
        if (element->isIntegerTy())
          char_bytes =
              call.getModule()->getDataLayout().getTypeAllocSize(element);
      }
      whole = size && capacity && char_bytes &&
              *size >= *capacity / char_bytes + (*capacity % char_bytes != 0);
    }
    if (whole)
      model.overwrites.push_back(memory(dest));
    pipe(memory(source), memory(dest));
    // stpcpy's result points at the newly written NUL, not at the copied
    // user-controlled characters. It is an empty string at this point.
    if (call.getType()->isPointerTy() && !named(*target, {"stpcpy"}))
      pipe(memory(source), memory(-1));
    return model;
  }
  if (named(*target, {"strcat", "strncat", "wcscat", "wcsncat"})) {
    model.known = true;
    if (call.arg_size() > 2 && argumentCount(call, 2) &&
        *argumentCount(call, 2) == 0)
      return model;
    pipe(memory(1), memory(0), true);
    pipe(memory(0), memory(-1));
    pipe(memory(1), memory(-1), true);
    return model;
  }
  if (isa<MemTransferInst>(call) || named(*target, {"memcpy", "memmove"})) {
    model.known = true;
    auto size = argumentCount(call, 2);
    const Value *dest = ValueFacts::argument(call, 0);
    auto capacity = dest ? ValueFacts::objectBytes(*dest) : None;
    if (size && *size == 0)
      return model;
    if (size && capacity && *size >= *capacity)
      model.overwrites.push_back(memory(0));
    pipe(memory(1), memory(0));
    if (call.getType()->isPointerTy())
      pipe(memory(1), memory(-1));
    return model;
  }
  if (isa<MemSetInst>(call) || named(*target, {"memset", "wmemset"})) {
    model.known = true;
    auto size = argumentCount(call, 2);
    const Value *dest = ValueFacts::argument(call, 0);
    auto capacity = dest ? ValueFacts::objectBytes(*dest) : None;
    if (size && capacity && *size >= *capacity)
      model.overwrites.push_back(memory(0));
    return model;
  }
  if (named(*target, {"strdup", "strndup", "strchr", "strrchr", "strstr",
                      "strcasestr"})) {
    model.known = true;
    if (named(*target, {"strndup"}) && argumentCount(call, 1) &&
        *argumentCount(call, 1) == 0)
      return model;
    pipe(memory(0), memory(-1));
    return model;
  }
  if (named(*target, {"gettext", "dgettext", "dcgettext", "ngettext",
                      "dngettext", "dcngettext"})) {
    model.known = true;
    unsigned first =
        named(*target, {"dgettext", "dcgettext", "dngettext", "dcngettext"})
            ? 1
            : 0;
    pipe(memory(first), memory(-1));
    if (named(*target, {"ngettext", "dngettext", "dcngettext"}))
      pipe(memory(first + 1), memory(-1));
    return model;
  }
  if (named(*target, {"atoi", "atol", "atoll", "strtol", "strtoul", "strtoll",
                      "strtoull", "strlen", "strnlen", "wcslen"})) {
    model.known = true;
    pipe(memory(0), value(-1), false, true);
    return model;
  }
  if (named(*target, {"PQescapeStringConn", "mysql_real_escape_string"})) {
    model.known = true;
    model.overwrites = {memory(1)};
    pipe(memory(2), memory(1), false, false, true);
    return model;
  }
  if (named(*target, {"PQescapeString", "mysql_escape_string"})) {
    model.known = true;
    model.overwrites = {memory(0)};
    pipe(memory(1), memory(0), false, false, true);
    return model;
  }
  if (named(*target, {"PQescapeLiteral", "PQescapeIdentifier"})) {
    model.known = true;
    pipe(memory(1), memory(-1), false, false, true);
    return model;
  }
  if (format_model && !format_model->scanf) {
    model.known = true;
    if (!named(*target,
               {"sprintf", "snprintf", "swprintf", "sprintf_s", "g_sprintf",
                "g_snprintf", "StringCchPrintfA", "StringCchPrintfW",
                "StringCbPrintfA", "StringCbPrintfW"}))
      return model;
    bool bounded = format_model->format >= 2;
    auto size = bounded ? argumentCount(call, 1) : None;
    if (size && *size == 0)
      return model;
    if (!bounded || (size && *size > 0))
      model.overwrites = {memory(0)};
    if (size && *size == 1)
      return model; // Produces just the terminator.
    pipe(memory(format_model->format), memory(0));
    const Value *arg = ValueFacts::argument(call, format_model->format);
    auto text = arg ? ValueFacts::constantString(*arg) : None;
    auto parsed = text ? parseFormat(*text, false) : None;
    if (parsed)
      for (const auto &conversion : parsed->conversions)
        if (conversion.conversion == 's' || conversion.conversion == 'S')
          pipe(memory(format_model->first_argument + conversion.argument - 1),
               memory(0), conversion.offset > 0);
    return model;
  }
  model.known = allocation(*target) != AllocationKind::Unknown ||
                release(*target) != ReleaseKind::Unknown ||
                readsOnly(*target) || target->isIntrinsic() ||
                named(*target, {"wordexp"});
  // Sinks and SQL operations do not synthesize unknown output taint.
  for (auto domain :
       {TaintDomain::Process, TaintDomain::Command, TaintDomain::Sql,
        TaintDomain::Path, TaintDomain::Wordexp})
    model.known |= !taintSinks(call, domain).empty();
  return model;
}

Optional<unsigned> LibraryModels::returnAlias(const CallBase &call) {
  if (auto cpp = CppLibraryModels::returnAlias(call))
    return cpp;
  const Function *target = ValueFacts::callee(call);
  if (!target || !target->isDeclaration() || !call.getType()->isPointerTy() ||
      !call.arg_size())
    return None;
  if (named(*target,
            {"memcpy",       "memmove",      "memset",        "strcpy",
             "strncpy",      "strcat",       "strncat",       "wcscpy",
             "wcsncpy",      "wcscat",       "wcsncat",       "wmemcpy",
             "wmemmove",     "wmemset",      "__memcpy_chk",  "__memmove_chk",
             "__memset_chk", "__strcpy_chk", "__strncpy_chk", "__strcat_chk",
             "__strncat_chk"}))
    return 0;
  return None;
}

std::vector<unsigned> LibraryModels::taintSinks(const CallBase &call,
                                                TaintDomain domain) {
  const Function *target = ValueFacts::callee(call);
  if (!target)
    return {};
  if (domain == TaintDomain::Wordexp && named(*target, {"wordexp"}) &&
      target->isDeclaration() && call.arg_size() >= 3) {
    // WRDE_NOCMD is 4 in the upstream rule's POSIX API model. Known bits also
    // retain this guarantee for flags assembled with a dynamic bitwise OR.
    if (sinkGuardDisabled(call, {0, 2, 4}))
      return {};
    return {0};
  }
  if (domain == TaintDomain::Format) {
    auto model = format(*target);
    return model && !model->scanf && model->format < call.arg_size()
               ? std::vector<unsigned>{model->format}
               : std::vector<unsigned>();
  }
  if (domain == TaintDomain::Allocation) {
    if (named(*target, {"malloc", "valloc"}) ||
        allocation(*target) == AllocationKind::New ||
        allocation(*target) == AllocationKind::NewArray)
      return {0};
    if (named(*target, {"calloc"}))
      return {0, 1};
    if (named(*target, {"realloc", "aligned_alloc"}))
      return {1};
  }
  if (domain == TaintDomain::Process &&
      named(*target, {"system", "popen", "execl", "execlp", "execle", "execv",
                      "execvp", "execvpe", "dlopen", "LoadLibrary",
                      "LoadLibraryA", "LoadLibraryW"}))
    return {0};
  if (domain == TaintDomain::Path &&
      named(*target, {"fopen", "_fopen", "_wfopen", "open", "_open", "_wopen",
                      "CreateFile", "CreateFileA", "CreateFileW"}))
    return {0};
  if (domain == TaintDomain::Sql) {
    if (named(*target,
              {"sqlite3_exec", "sqlite3_prepare", "sqlite3_prepare_v2",
               "sqlite3_prepare_v3", "sqlite3_prepare16",
               "sqlite3_prepare16_v2", "mysql_query", "mysql_real_query",
               "mysql_real_query_nonblocking", "PQexec", "PQexecParams",
               "PQsendQuery", "PQsendQueryParams", "SQLExecDirect",
               "SQLExecDirectA", "SQLExecDirectW", "SQLPrepare",
               "cdb2_run_statement", "cdb2_run_statement_typed"}))
      return {1};
    if (named(*target, {"PQprepare", "PQsendPrepare"}))
      return {2};
  }
  if (domain == TaintDomain::Command) {
    if (named(*target, {"system", "popen"}))
      return {0};
    if (named(*target, {"execl", "execlp", "execle"}) && call.arg_size() > 3) {
      auto executable = ValueFacts::constantString(*call.getArgOperand(0));
      auto flag = ValueFacts::constantString(*call.getArgOperand(2));
      if (executable && flag &&
          (*executable == "sh" || *executable == "/bin/sh" ||
           *executable == "bash" || *executable == "/bin/bash") &&
          *flag == "-c") {
        std::vector<unsigned> result;
        unsigned end = call.arg_size() - (named(*target, {"execle"}) ? 1 : 0);
        for (unsigned i = 3; i < end; ++i)
          if (call.getArgOperand(i)->getType()->isPointerTy())
            result.push_back(i);
        return result;
      }
    }
  }
  return {};
}

std::vector<TaintSinkGuard> LibraryModels::taintSinkGuards(const CallBase &call,
                                                           TaintDomain domain) {
  const Function *target = ValueFacts::callee(call);
  if (domain == TaintDomain::Wordexp && target && target->isDeclaration() &&
      named(*target, {"wordexp"}) && call.arg_size() >= 3)
    return {{0, 2, 4}};
  return {};
}

bool LibraryModels::sinkGuardDisabled(const CallBase &call,
                                      const TaintSinkGuard &guard) {
  const Value *flags = ValueFacts::argument(call, guard.flag_argument);
  if (!flags || !flags->getType()->isIntegerTy())
    return false;
  const auto bits = computeKnownBits(flags, call.getModule()->getDataLayout(),
                                     0, nullptr, &call);
  return !(bits.One & APInt(bits.getBitWidth(), guard.disabled_by_any_set_bits))
              .isZero();
}
} // namespace pdg
