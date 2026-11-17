#include "Checker/TPA/Output/WriteDotFile.h"

#include "Checker/TPA/Output/Printer.h"
#include "Checker/TPA/Program/DefUseFunction.h"

#include <map>
#include <set>

#include <llvm/IR/Function.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/GraphWriter.h>
#include <llvm/Support/ToolOutputFile.h>

namespace util::io {
bool writeDotFile(const char *filePath,
                  const tpa::taint::DefUseFunction &function) {
  std::error_code error;
  llvm::ToolOutputFile out(filePath, error, llvm::sys::fs::OF_None);
  if (error) {
    llvm::errs() << "Cannot write def-use graph " << filePath << ": "
                 << error.message() << "\n";
    return false;
  }
  using Node = tpa::taint::DefUseInstruction;
  std::vector<const Node *> nodes{function.getEntryInst()};
  for (const auto &block : function.getFunction())
    for (const auto &inst : block)
      if (auto *node = function.getDefUseInstruction(&inst))
        nodes.push_back(node);
  std::map<const Node *, unsigned> ids;
  for (unsigned i = 0; i < nodes.size(); ++i)
    ids[nodes[i]] = i;
  auto &os = out.os();
  os << "digraph \""
     << llvm::DOT::EscapeString(function.getFunction().getName().str())
     << "\" {\n";
  for (auto *node : nodes) {
    auto id = ids.at(node);
    std::string label;
    llvm::raw_string_ostream text(label);
    text << *node;
    os << "  n" << id << " [shape=box,label=\""
       << llvm::DOT::EscapeString(text.str()) << "\"];\n";
    std::set<unsigned> valueSuccessors;
    for (auto *succ : node->top_succs())
      valueSuccessors.insert(ids.at(succ));
    for (auto succ : valueSuccessors)
      os << "  n" << id << " -> n" << succ << " [style=dotted];\n";
    std::map<unsigned, std::set<std::string>> memorySuccessors;
    for (const auto &entry : node->mem_succs()) {
      std::string object;
      llvm::raw_string_ostream objectText(object);
      objectText << *entry.first;
      for (auto *succ : entry.second)
        memorySuccessors[ids.at(succ)].insert(objectText.str());
    }
    for (const auto &entry : memorySuccessors) {
      std::string objects;
      for (const auto &object : entry.second)
        objects += object + "\n";
      os << "  n" << id << " -> n" << entry.first << " [label=\""
         << llvm::DOT::EscapeString(objects) << "\"];\n";
    }
  }
  os << "}\n";
  os.flush();
  if (os.has_error())
    return false;
  out.keep();
  return true;
}
} // namespace util::io
