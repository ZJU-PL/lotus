#include "Checker/TPA/Annotation/ExternalTaintTable.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

namespace annotation {

bool ExternalTaintTable::parse(llvm::StringRef text, ExternalTaintTable &out,
                               llvm::raw_ostream &errors) {
  ExternalTaintTable table;
  unsigned lineNumber = 0;
  while (!text.empty()) {
    auto line = text.split('\n');
    text = line.second;
    ++lineNumber;
    std::istringstream input(line.first.split('#').first.str());
    std::vector<std::string> words;
    for (std::string word; input >> word;)
      words.push_back(word);
    if (words.empty())
      continue;
    auto fail = [&]() {
      errors << "Invalid TPA taint specification at line " << lineNumber << ": "
             << line.first << "\n";
      return false;
    };
    auto position = [](llvm::StringRef token, TPosition &pos) {
      if (token == "Ret") {
        pos = TPosition::getReturnPosition();
        return true;
      }
      bool after = token.consume_front("AfterArg");
      if (!after && !token.consume_front("Arg"))
        return false;
      unsigned index;
      if (token.getAsInteger(10, index) || index > 255)
        return false;
      pos = after ? TPosition::getAfterArgPosition(index)
                  : TPosition::getArgPosition(index);
      return true;
    };
    auto category = [](llvm::StringRef token, TClass &cls) {
      if (token == "V")
        cls = TClass::ValueOnly;
      else if (token == "D")
        cls = TClass::DirectMemory;
      else if (token == "R")
        cls = TClass::ReachableMemory;
      else
        return false;
      return true;
    };
    if (words[0] == "IGNORE") {
      if (words.size() != 2)
        return fail();
      table.summaryMap.try_emplace(words[1]);
      continue;
    }
    if (words.size() < 4)
      return fail();
    auto dst = TPosition::getReturnPosition();
    TClass dstClass;
    if (!position(words[2], dst) || !category(words[3], dstClass))
      return fail();
    auto &summary = table.summaryMap[words[1]];
    if (words[0] == "SOURCE") {
      if (words.size() != 5)
        return fail();
      tpa::taint::TaintLattice val;
      if (words[4] == "T")
        val = tpa::taint::TaintLattice::Tainted;
      else if (words[4] == "U")
        val = tpa::taint::TaintLattice::Untainted;
      else if (words[4] == "E")
        val = tpa::taint::TaintLattice::Either;
      else
        return fail();
      summary.addEntry(TaintEntry::getSourceEntry(dst, dstClass, val));
    } else if (words[0] == "SINK") {
      if (words.size() != 4 || dst.isReturnPosition() ||
          dstClass == TClass::ReachableMemory)
        return fail();
      summary.addEntry(TaintEntry::getSinkEntry(dst, dstClass));
    } else if (words[0] == "PIPE") {
      if (words.size() != 6)
        return fail();
      auto src = TPosition::getReturnPosition();
      TClass srcClass;
      if (!position(words[4], src) || !category(words[5], srcClass) ||
          src.isReturnPosition())
        return fail();
      summary.addEntry(TaintEntry::getPipeEntry(dst, dstClass, src, srcClass));
    } else
      return fail();
  }
  out = std::move(table);
  return true;
}

ExternalTaintTable ExternalTaintTable::loadFromFile(const char *fileName) {
  auto buffer = llvm::MemoryBuffer::getFile(fileName);
  if (!buffer)
    llvm::report_fatal_error(
        llvm::Twine("Cannot read TPA taint specification: ") + fileName);
  ExternalTaintTable table;
  if (!parse((*buffer)->getBuffer(), table, llvm::errs()))
    llvm::report_fatal_error("Cannot parse TPA taint specification");
  return table;
}

const TaintSummary *ExternalTaintTable::lookup(llvm::StringRef name) const {
  auto it = summaryMap.find(name.str());
  return it == summaryMap.end() ? nullptr : &it->second;
}
} // namespace annotation
