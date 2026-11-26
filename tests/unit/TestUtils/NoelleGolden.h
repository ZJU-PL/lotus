#pragma once

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace lotus {
namespace unittest {
namespace noelle_golden {

using Values = std::set<std::string>;

class Parser {
public:
  static void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
              return !std::isspace(ch);
            }));
  }

  static void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(),
                         s.rend(),
                         [](int ch) { return !std::isspace(ch); })
                .base(),
            s.end());
  }

  static void trim(std::string &s) {
    ltrim(s);
    rtrim(s);
  }

  static std::vector<std::string> split(const std::string &s,
                                        const std::string &delimiter) {
    size_t prev_pos = 0;
    size_t pos = 0;
    std::vector<std::string> tokens;
    while (prev_pos < s.length()
           && (pos = s.find(delimiter, prev_pos)) != std::string::npos) {
      tokens.push_back(s.substr(prev_pos, pos - prev_pos));
      prev_pos = pos + delimiter.size();
    }
    if (prev_pos != s.length()) {
      tokens.push_back(s.substr(prev_pos, s.length() - prev_pos));
    }
    return tokens;
  }
};

class GoldenFile {
public:
  explicit GoldenFile(const std::string &filename,
                      const std::string &unorderedDelimiter = "|",
                      const std::string &orderedDelimiter = ";")
      : unorderedDelimiter{unorderedDelimiter},
        orderedDelimiter{orderedDelimiter} {
    std::ifstream file(filename);
    EXPECT_TRUE(file.is_open()) << "Could not open golden file: " << filename;
    if (!file.is_open()) {
      return;
    }

    const std::set<std::string> lineContinuations{orderedDelimiter,
                                                  unorderedDelimiter};
    std::string line;
    std::string group;
    std::vector<std::string> lineSplits;
    while (std::getline(file, line)) {
      Parser::trim(line);
      if (line.empty()) {
        group.clear();
        continue;
      }
      if (line[0] == '#') {
        continue;
      }
      if (group.empty()) {
        group = line;
        groupValues[group].clear();
        continue;
      }

      lineSplits.push_back(line);
      std::string lastChar(1, line.back());
      if (lineContinuations.find(lastChar) == lineContinuations.end()) {
        std::string fullLine;
        for (auto const &part : lineSplits) {
          fullLine += part;
        }
        lineSplits.clear();
        groupValues[group].insert(processDelimitedRow(fullLine));
      }
    }
  }

  const Values &getSection(const std::string &section) const {
    static const Values empty{};
    auto it = groupValues.find(section);
    if (it == groupValues.end()) {
      return empty;
    }
    return it->second;
  }

  bool hasSection(const std::string &section) const {
    return groupValues.find(section) != groupValues.end();
  }

  std::string normalizeValue(const std::string &value) const {
    return processDelimitedRow(value);
  }

private:
  std::string processDelimitedRow(std::string value) const {
    Parser::trim(value);
    std::vector<std::string> unorderedTokens;
    std::vector<std::string> orderedTokens;
    trySplitOrderedAndUnordered(value, orderedTokens, unorderedTokens);
    bool isUnordered = unorderedTokens.size() > 1;
    bool isOrdered = orderedTokens.size() > 1;
    if (!isUnordered && !isOrdered) {
      return canonicalizeValue(value);
    }

    auto &tokens = isUnordered ? unorderedTokens : orderedTokens;
    for (auto &token : tokens) {
      Parser::trim(token);
      token = canonicalizeValue(token);
    }
    if (isUnordered) {
      std::sort(tokens.begin(), tokens.end());
    }

    std::string result = tokens[0];
    for (size_t i = 1; i < tokens.size(); ++i) {
      result += orderedDelimiter + tokens[i];
    }
    return result;
  }

  static std::string canonicalizeValue(std::string value) {
    // Drop trailing LLVM metadata that is not semantically relevant for the
    // NOELLE oracle comparison.
    static const std::regex metadataRegex(", ![A-Za-z0-9._]+ ![0-9]+");
    static const std::regex trailingAlignRegex(",\\s*align\\s+[0-9]+");
    value = std::regex_replace(value, metadataRegex, "");
    value = std::regex_replace(value, trailingAlignRegex, "");

    // Strip semantically irrelevant parameter/value attributes that differ
    // across otherwise equivalent LLVM pipelines.
    static const std::regex noundefRegex("\\bnoundef\\s+");
    static const std::regex nonnullRegex("\\bnonnull\\s+");
    static const std::regex alignRegex("\\balign\\s+[0-9]+\\s+");
    static const std::regex derefRegex("\\bdereferenceable(_or_null)?\\([0-9]+\\)\\s+");
    static const std::regex attrBundleRegex("\\s+#\\d+");
    value = std::regex_replace(value, noundefRegex, "");
    value = std::regex_replace(value, nonnullRegex, "");
    value = std::regex_replace(value, alignRegex, "");
    value = std::regex_replace(value, derefRegex, "");
    value = std::regex_replace(value, attrBundleRegex, "");

    // Canonicalize SSA/value identifiers within each row so differing numbering
    // across equivalent LLVM pipelines does not force a mismatch.
    static const std::regex valueRegex("%[-A-Za-z$._0-9]+");
    std::unordered_map<std::string, std::string> mapping;
    int nextID = 0;

    std::string result;
    std::sregex_iterator begin(value.begin(), value.end(), valueRegex);
    std::sregex_iterator end;
    size_t cursor = 0;
    for (auto it = begin; it != end; ++it) {
      auto matchPos = static_cast<size_t>(it->position());
      auto matchLen = static_cast<size_t>(it->length());
      result.append(value, cursor, matchPos - cursor);

      auto token = it->str();
      auto mapIt = mapping.find(token);
      if (mapIt == mapping.end()) {
        auto canonical = std::string("%v") + std::to_string(nextID++);
        mapIt = mapping.emplace(token, canonical).first;
      }
      result += mapIt->second;
      cursor = matchPos + matchLen;
    }
    result.append(value, cursor, std::string::npos);
    Parser::trim(result);
    return result;
  }

  void trySplitOrderedAndUnordered(const std::string &value,
                                   std::vector<std::string> &ordered,
                                   std::vector<std::string> &unordered) const {
    auto unorderedTokens = Parser::split(value, unorderedDelimiter);
    auto orderedTokens = Parser::split(value, orderedDelimiter);
    bool isUnordered = unorderedTokens.size() > 1;
    bool isOrdered = orderedTokens.size() > 1;
    EXPECT_FALSE(isUnordered && isOrdered)
        << "NOELLE golden values cannot mix ordered and unordered delimiters";
    ordered = orderedTokens;
    unordered = unorderedTokens;
  }

  std::string unorderedDelimiter;
  std::string orderedDelimiter;
  std::unordered_map<std::string, Values> groupValues;
};

template <typename T>
inline std::string printToString(T *printable) {
  std::string str;
  llvm::raw_string_ostream stream(str);
  printable->print(stream);
  stream.flush();
  Parser::trim(str);
  return str;
}

template <typename T>
inline std::string printAsOperandToString(T *printable) {
  std::string str;
  llvm::raw_string_ostream stream(str);
  printable->printAsOperand(stream);
  stream.flush();
  Parser::trim(str);
  return str;
}

inline std::string trimProfilerBitcodeInfo(std::string value) {
  auto pos = value.find(", !prof");
  if (pos != std::string::npos) {
    value.erase(value.begin() + pos, value.end());
  }
  return value;
}

inline std::string valueToString(llvm::Value *value) {
  return trimProfilerBitcodeInfo(printToString(value));
}

inline std::string combineValues(std::vector<std::string> values,
                                 const std::string &delimiter) {
  if (values.empty()) {
    return "";
  }
  std::string allValues = values[0];
  for (size_t i = 1; i < values.size(); ++i) {
    allValues += delimiter + values[i];
  }
  return allValues;
}

inline std::string combineOrderedValues(std::vector<std::string> values) {
  return combineValues(std::move(values), ";");
}

inline std::string combineUnorderedValues(std::vector<std::string> values) {
  std::sort(values.begin(), values.end());
  return combineValues(std::move(values), "|");
}

inline void expectSectionMatches(const GoldenFile &golden,
                                 const std::string &section,
                                 const Values &actual) {
  ASSERT_TRUE(golden.hasSection(section)) << "Missing golden section: " << section;
  Values normalizedActual;
  for (auto const &value : actual) {
    normalizedActual.insert(golden.normalizeValue(value));
  }

  const auto &expected = golden.getSection(section);
  for (auto const &value : expected) {
    EXPECT_NE(normalizedActual.find(value), normalizedActual.end())
        << "Missing expected value in section " << section << ": " << value;
  }
  for (auto const &value : normalizedActual) {
    EXPECT_NE(expected.find(value), expected.end())
        << "Unexpected value in section " << section << ": " << value;
  }
}

inline bool sectionMatches(const GoldenFile &golden,
                           const std::string &section,
                           const Values &actual,
                           std::string *diff = nullptr) {
  if (!golden.hasSection(section)) {
    if (diff != nullptr) {
      *diff = "missing golden section: " + section;
    }
    return false;
  }

  Values normalizedActual;
  for (auto const &value : actual) {
    normalizedActual.insert(golden.normalizeValue(value));
  }

  const auto &expected = golden.getSection(section);
  std::vector<std::string> missing;
  std::vector<std::string> unexpected;
  for (auto const &value : expected) {
    if (normalizedActual.find(value) == normalizedActual.end()) {
      missing.push_back(value);
    }
  }
  for (auto const &value : normalizedActual) {
    if (expected.find(value) == expected.end()) {
      unexpected.push_back(value);
    }
  }

  if (missing.empty() && unexpected.empty()) {
    return true;
  }

  if (diff != nullptr) {
    std::string text = "section " + section;
    if (!missing.empty()) {
      text += " missing=" + std::to_string(missing.size());
      text += " [";
      for (size_t i = 0; i < missing.size() && i < 3; ++i) {
        if (i != 0) {
          text += " || ";
        }
        text += missing[i];
      }
      text += "]";
    }
    if (!unexpected.empty()) {
      text += " unexpected=" + std::to_string(unexpected.size());
      text += " [";
      for (size_t i = 0; i < unexpected.size() && i < 3; ++i) {
        if (i != 0) {
          text += " || ";
        }
        text += unexpected[i];
      }
      text += "]";
    }
    *diff = text;
  }
  return false;
}

} // namespace noelle_golden
} // namespace unittest
} // namespace lotus
