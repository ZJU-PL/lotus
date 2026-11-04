#include "Analysis/CallGraph/CallGraph.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/Support/raw_ostream.h"

namespace lotus {

void CallGraph::addCallEdge(const llvm::Instruction *CS,
                            const llvm::Function *Callee) {
  if (!CS || !Callee) {
    return;
  }

  auto &Callees = CalleesOf[CS];
  for (const auto *Existing : Callees) {
    if (Existing == Callee) {
      return;
    }
  }
  Callees.push_back(Callee);
  CallersOf[Callee].push_back(CS);
}

llvm::ArrayRef<const llvm::Function *>
CallGraph::getCalleesOfCallAt(const llvm::Instruction *CS) const {
  auto It = CalleesOf.find(CS);
  if (It != CalleesOf.end()) {
    return It->second;
  }
  return {};
}

llvm::ArrayRef<const llvm::Instruction *>
CallGraph::getCallersOf(const llvm::Function *F) const {
  auto It = CallersOf.find(F);
  if (It != CallersOf.end()) {
    return It->second;
  }
  return {};
}

std::vector<const llvm::Function *> CallGraph::getAllVertexFunctions() const {
  std::vector<const llvm::Function *> Result;
  Result.reserve(CallersOf.size());
  for (const auto &[F, _] : CallersOf) {
    Result.push_back(F);
  }
  return Result;
}

std::vector<const llvm::Instruction *>
CallGraph::getAllVertexCallSites() const {
  std::vector<const llvm::Instruction *> Result;
  Result.reserve(CalleesOf.size());
  for (const auto &[CS, _] : CalleesOf) {
    Result.push_back(CS);
  }
  return Result;
}

size_t CallGraph::getNumCallSites() const { return CalleesOf.size(); }

size_t CallGraph::getNumFunctions() const { return CallersOf.size(); }

bool CallGraph::empty() const {
  return CalleesOf.empty() && CallersOf.empty();
}

void CallGraph::print(llvm::raw_ostream &OS) const {
  for (const auto &[CS, Targets] : CalleesOf) {
    OS << *CS << " -> { ";
    for (size_t I = 0; I < Targets.size(); ++I) {
      OS << Targets[I]->getName();
      if (I + 1 < Targets.size()) {
        OS << ", ";
      }
    }
    OS << " }\n";
  }
}

void CallGraph::printAsDot(llvm::raw_ostream &OS) const {
  OS << "digraph CallGraph {\n";
  for (const auto &[CS, Targets] : CalleesOf) {
    const auto *CallerFn = CS->getFunction();
    llvm::StringRef CallerName = CallerFn ? CallerFn->getName() : "unknown";
    for (const auto *Target : Targets) {
      OS << "  \"" << CallerName << "\" -> \"" << Target->getName() << "\";\n";
    }
  }
  OS << "}\n";
}

void CallGraph::printAsJson(llvm::raw_ostream &OS) const {
  OS << "{\n";
  bool FirstEntry = true;
  for (const auto &[F, Callers] : CallersOf) {
    if (!FirstEntry) {
      OS << ",\n";
    }
    FirstEntry = false;
    OS << "  \"" << F->getName() << "\": [\n";
    for (size_t I = 0; I < Callers.size(); ++I) {
      OS << "    \"" << *Callers[I] << "\"";
      if (I + 1 < Callers.size()) {
        OS << ",";
      }
      OS << "\n";
    }
    OS << "  ]";
  }
  OS << "\n}\n";
}

} // namespace lotus
