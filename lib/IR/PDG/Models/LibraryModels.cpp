#include "IR/PDG/Models/LibraryModels.h"

#include "IR/PDG/Models/ValueFacts.h"

#include <algorithm>
#include <cctype>

using namespace llvm;

namespace pdg {

Optional<BufferCopyModel> LibraryModels::copy(const Function &function) {
  struct Entry {
    const char *name;
    unsigned size;
    bool std_name;
    bool bsl_name;
  };
  static const Entry entries[] = {{"strncpy", 2, true, true},
                                  {"wcsncpy", 2, true, true},
                                  {"strxfrm", 2, true, true},
                                  {"wcsxfrm", 2, true, true},
                                  {"_strncpy_l", 2, false, false},
                                  {"_wcsncpy_l", 2, false, false},
                                  {"_mbsncpy", 2, false, false},
                                  {"_mbsncpy_l", 2, false, false},
                                  {"_strxfrm_l", 2, false, false},
                                  {"wcsxfrm_l", 2, false, false},
                                  {"_mbsnbcpy", 2, false, false},
                                  {"stpncpy", 2, false, false},
                                  {"strlcpy", 2, false, false},
                                  {"__builtin___stpncpy_chk", 2, false, false},
                                  {"__builtin___strncpy_chk", 2, false, false},
                                  {"strcpy_s", 1, true, false},
                                  {"wcscpy_s", 1, true, false},
                                  {"_mbscpy_s", 1, false, false}};
  for (const Entry &entry : entries)
    if (ValueFacts::hasLibraryName(function, entry.name, entry.std_name,
                                   entry.bsl_name) &&
        (entry.size != 1 || function.arg_size() == 3))
      return BufferCopyModel{0, entry.size == 1 ? 2u : 1u, entry.size};
  return None;
}

Optional<FormatModel> LibraryModels::format(const Function &function) {
  if (!function.isDeclaration())
    return None;
  struct Entry {
    const char *name;
    unsigned format;
    unsigned first;
    bool scanf;
  };
  static const Entry entries[] = {{"printf", 0, 1, false},
                                  {"wprintf", 0, 1, false},
                                  {"StringCchPrintfA", 2, 3, false},
                                  {"StringCchPrintfW", 2, 3, false},
                                  {"StringCbPrintfA", 2, 3, false},
                                  {"StringCbPrintfW", 2, 3, false},
                                  {"printf_s", 0, 1, false},
                                  {"fprintf", 1, 2, false},
                                  {"fwprintf", 1, 2, false},
                                  {"sprintf", 1, 2, false},
                                  {"snprintf", 2, 3, false},
                                  {"swprintf", 2, 3, false},
                                  {"sprintf_s", 2, 3, false},
                                  {"dprintf", 1, 2, false},
                                  {"asprintf", 1, 2, false},
                                  {"syslog", 1, 2, false},
                                  {"g_printf", 0, 1, false},
                                  {"g_fprintf", 1, 2, false},
                                  {"g_sprintf", 1, 2, false},
                                  {"g_snprintf", 2, 3, false},
                                  {"__printf_chk", 1, 2, false},
                                  {"__fprintf_chk", 2, 3, false},
                                  {"__sprintf_chk", 3, 4, false},
                                  {"__snprintf_chk", 4, 5, false},
                                  {"scanf", 0, 1, true},
                                  {"wscanf", 0, 1, true},
                                  {"sscanf", 1, 2, true},
                                  {"swscanf", 1, 2, true},
                                  {"fscanf", 1, 2, true},
                                  {"fwscanf", 1, 2, true},
                                  {"__isoc99_scanf", 0, 1, true},
                                  {"__isoc99_sscanf", 1, 2, true},
                                  {"__isoc99_fscanf", 1, 2, true}};
  for (const Entry &entry : entries)
    if (ValueFacts::hasLibraryName(function, entry.name, true, true)) {
      const StringRef name(entry.name);
      bool wide = name == "wprintf" || name == "fwprintf" ||
                  name == "swprintf" || name == "wscanf" || name == "fwscanf" ||
                  name == "swscanf" || name == "StringCchPrintfW" ||
                  name == "StringCbPrintfW";
      return FormatModel{entry.format, entry.first, entry.scanf, wide};
    }
  return None;
}

bool LibraryModels::readsOnly(const Function &function) {
  if (!function.isDeclaration())
    return false;
  for (const char *name :
       {"strlen", "wcslen", "strcmp", "strncmp", "memcmp", "wcscmp"})
    if (ValueFacts::hasLibraryName(function, name, true, true))
      return true;
  return false;
}

AllocationKind LibraryModels::allocation(const Function &function) {
  for (const char *name :
       {"malloc", "calloc", "realloc", "aligned_alloc", "strdup", "strndup"})
    if (ValueFacts::hasLibraryName(function, name, true))
      return AllocationKind::Malloc;
  std::string name = ValueFacts::functionBaseName(function);
  // Placement new aliases its destination rather than creating a new object.
  for (const auto &arg : function.args())
    if (arg.getType()->isPointerTy())
      return AllocationKind::Unknown;
  if (name == "operator new")
    return AllocationKind::New;
  if (name == "operator new[]")
    return AllocationKind::NewArray;
  return AllocationKind::Unknown;
}

ReleaseKind LibraryModels::release(const Function &function) {
  if (ValueFacts::hasLibraryName(function, "free", true))
    return ReleaseKind::Free;
  const std::string name = ValueFacts::functionBaseName(function);
  if (name == "operator delete")
    return ReleaseKind::Delete;
  if (name == "operator delete[]")
    return ReleaseKind::DeleteArray;
  return ReleaseKind::Unknown;
}

Optional<FormatFacts> parseFormat(StringRef text, bool scanf) {
  FormatFacts result;
  unsigned next_argument = 1;
  bool positional = false, sequential = false;
  auto digits = [&](size_t &i) -> Optional<unsigned> {
    size_t start = i;
    uint64_t value = 0;
    while (i < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[i]))) {
      value = value * 10 + text[i++] - '0';
      if (value > 1000000)
        return None;
    }
    return i == start ? Optional<unsigned>()
                      : Optional<unsigned>(static_cast<unsigned>(value));
  };
  auto consume = [&](Optional<unsigned> index) {
    if (index) {
      if (*index == 0)
        return false;
      positional = true;
      result.required_arguments = std::max(result.required_arguments, *index);
    } else {
      sequential = true;
      result.required_arguments =
          std::max(result.required_arguments, next_argument++);
    }
    return !(positional && sequential);
  };
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%')
      continue;
    unsigned directive_offset = i;
    if (++i == text.size())
      return None;
    if (text[i] == '%')
      continue;
    size_t start = i;
    auto index = digits(i);
    if (index && i < text.size() && text[i] == '$')
      ++i;
    else {
      index = None;
      i = start;
    }
    bool suppress = false, width = false, allocated = false;
    Optional<unsigned> field_width, precision;
    unsigned width_argument = 0, precision_argument = 0;
    if (scanf) {
      if (i < text.size() && text[i] == '*') {
        suppress = true;
        ++i;
      }
      size_t before = i;
      auto count = digits(i);
      if (i != before && !count)
        return None;
      width = count && *count > 0;
      field_width = count;
      if (i < text.size() && text[i] == 'm') {
        allocated = true;
        ++i;
      }
    } else {
      while (i < text.size() && StringRef("-+ #0'").contains(text[i]))
        ++i;
      for (unsigned part = 0; part < 2; ++part) {
        if (part == 1) {
          if (i == text.size() || text[i] != '.')
            break;
          ++i;
          precision = 0;
        }
        if (i < text.size() && text[i] == '*') {
          ++i;
          auto star = digits(i);
          if (star) {
            if (i == text.size() || text[i++] != '$')
              return None;
          }
          unsigned argument = star ? *star : next_argument;
          (part == 0 ? width_argument : precision_argument) = argument;
          if (!consume(star))
            return None;
        } else {
          size_t before = i;
          auto count = digits(i);
          if (i != before && !count)
            return None;
          if (count)
            (part == 0 ? field_width : precision) = count;
        }
      }
    }
    size_t length_start = i;
    while (i < text.size() && StringRef("hljztLqw").contains(text[i]))
      ++i;
    if (i < text.size() && text[i] == 'I') {
      ++i;
      if (text.substr(i).startswith("32") || text.substr(i).startswith("64"))
        i += 2;
    }
    std::string length = text.slice(length_start, i).str();
    if (i == text.size())
      return None;
    char conversion = text[i];
    if (scanf && conversion == '[') {
      if (i + 1 < text.size() && text[i + 1] == '^')
        ++i;
      if (i + 1 < text.size() && text[i + 1] == ']')
        ++i;
      do {
        ++i;
      } while (i < text.size() && text[i] != ']');
      if (i == text.size())
        return None;
    } else if (!StringRef(scanf ? "diouxXaAeEfFgGcsnp"
                                : "diouxXaAeEfFgGcsnpCmS%")
                    .contains(conversion))
      return None;
    if (!scanf && (conversion == 'm' || conversion == '%'))
      continue;
    if (!suppress) {
      unsigned argument = index ? *index : next_argument;
      if (!consume(index))
        return None;
      result.conversions.push_back(
          {argument, conversion, directive_offset, length, field_width,
           precision, width_argument, precision_argument, allocated});
    }
    result.unbounded_string |=
        scanf && !suppress && !width && !allocated && conversion == 's';
  }
  return result;
}

} // namespace pdg
