#include "Alias/AliasAnalysisWrapper.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <vector>
#include <random>
#include <algorithm>
#include <chrono>

using namespace llvm;
using namespace lotus;

// Command line options
static cl::opt<std::string> InputFilename(cl::Positional,
                                         cl::desc("<input bitcode file>"),
                                         cl::Required);

static cl::opt<unsigned> NumFunctions("num-functions",
                                     cl::desc("Number of random functions (default: 10)"),
                                     cl::init(10));

static cl::opt<unsigned> MaxPointers("max-pointers",
                                    cl::desc("Max pointers per function (default: 50)"),
                                    cl::init(50));

// Analysis statistics
struct AnalysisStats {
  std::string name;
  unsigned noAlias = 0, mayAlias = 0, mustAlias = 0, totalQueries = 0;
  double timeMs = 0.0;
  bool initialized = false;
};

// Collect pointers from a function
std::vector<const Value*> collectPointers(const Function &F) {
  std::vector<const Value*> ptrs;
  for (const Argument &Arg : F.args())
    if (Arg.getType()->isPointerTy()) ptrs.push_back(&Arg);
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (I.getType()->isPointerTy()) ptrs.push_back(&I);
  return ptrs;
}

// Run analysis and collect stats
AnalysisStats runAnalysis(const std::string &name, AliasAnalysisWrapper &analysis,
                          const std::vector<const Value*> &pointers) {
  AnalysisStats stats;
  stats.name = name;
  stats.initialized = analysis.isInitialized();
  
  auto start = std::chrono::high_resolution_clock::now();
  for (size_t i = 0; i < pointers.size(); ++i) {
    for (size_t j = i + 1; j < pointers.size(); ++j) {
      AliasResult result = analysis.query(pointers[i], pointers[j]);
      stats.totalQueries++;
      if (result == AliasResult::NoAlias) stats.noAlias++;
      else if (result == AliasResult::MustAlias) stats.mustAlias++;
      else stats.mayAlias++;
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  stats.timeMs = std::chrono::duration<double, std::milli>(end - start).count();
  return stats;
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, 
    "Alias Analysis Comparison Tool\n"
    "Compares multiple alias analyses on LLVM bitcode.\n");

  // Load bitcode
  LLVMContext Context;
  auto BufferOrError = MemoryBuffer::getFile(InputFilename);
  if (std::error_code EC = BufferOrError.getError()) {
    errs() << "Error: " << EC.message() << "\n";
    return 1;
  }

  auto ModuleOrError = parseBitcodeFile(BufferOrError.get()->getMemBufferRef(), Context);
  if (Error E = ModuleOrError.takeError()) {
    errs() << "Error parsing bitcode\n";
    logAllUnhandledErrors(std::move(E), errs(), "");
    return 1;
  }

  std::unique_ptr<Module> M = std::move(ModuleOrError.get());
  errs() << "Module: " << M->getName() << "\n";

  // Collect non-empty functions
  std::vector<const Function*> functions;
  for (const Function &F : *M)
    if (!F.isDeclaration() && !F.empty()) functions.push_back(&F);
  
  if (functions.empty()) {
    errs() << "No functions found!\n";
    return 1;
  }

  // Randomly select functions
  unsigned numToSelect = std::min(NumFunctions.getValue(), (unsigned)functions.size());
  std::random_device rd;
  std::mt19937 gen(rd());
  std::shuffle(functions.begin(), functions.end(), gen);
  functions.resize(numToSelect);

  // Collect pointers
  std::vector<const Value*> allPointers;
  for (const Function *F : functions) {
    auto pointers = collectPointers(*F);
    if (pointers.size() > MaxPointers) {
      std::shuffle(pointers.begin(), pointers.end(), gen);
      pointers.resize(MaxPointers);
    }
    allPointers.insert(allPointers.end(), pointers.begin(), pointers.end());
  }

  unsigned totalQueries = (allPointers.size() * (allPointers.size() - 1)) / 2;
  errs() << "Functions: " << numToSelect 
         << ", Pointers: " << allPointers.size()
         << ", Queries: " << totalQueries << "\n\n";

  if (allPointers.empty()) {
    errs() << "No pointers found!\n";
    return 0;
  }

  // Define analyses to compare (module-level analyses only)
  // Note: CFLAnders/CFLSteens are function-scoped and can't compare cross-function pointers
  // Note: SRAA, SeaDSA, AllocAA require additional pass manager setup
  std::vector<AAType> analysisTypes = {
    AAType::Andersen,
    AAType::DyckAA,
    AAType::UnderApprox
  };

  // Run all analyses
  errs() << "Running analyses...\n";
  std::vector<AnalysisStats> results;
  for (AAType aaType : analysisTypes) {
    std::string name = AliasAnalysisFactory::getTypeName(aaType);
    errs() << "  " << name << "... ";
    errs().flush();
    
    AliasAnalysisWrapper analysis(*M, aaType);
    AnalysisStats stats = runAnalysis(name, analysis, allPointers);
    results.push_back(stats);
    
    errs() << (stats.initialized ? "✓" : "✗") << " " 
           << format("%.2fms", stats.timeMs) << "\n";
  }

  // Print results table
  errs() << "\n";
  errs() << "┌─────────────────┬──────────┬──────────┬──────────┬──────────┬────────────┬──────┐\n";
  errs() << "│ Analysis        │  Queries │  NoAlias │ MayAlias │MustAlias │   Time(ms) │ Init │\n";
  errs() << "├─────────────────┼──────────┼──────────┼──────────┼──────────┼────────────┼──────┤\n";
  
  for (const auto &stats : results) {
    double noAliasPercent = stats.totalQueries > 0 
      ? (100.0 * stats.noAlias / stats.totalQueries) : 0.0;
    
    errs() << format("│ %-15s │ %8u │ %7u%% │ %8u │ %8u │ %10.2f │ %4s │\n",
                     stats.name.c_str(),
                     stats.totalQueries,
                     (unsigned)noAliasPercent,
                     stats.mayAlias,
                     stats.mustAlias,
                     stats.timeMs,
                     stats.initialized ? "Yes" : "No");
  }
  errs() << "└─────────────────┴──────────┴──────────┴──────────┴──────────┴────────────┴──────┘\n\n";

  // Print analysis insights
  errs() << "Key Insights:\n";
  
  // Find initialized analyses
  const AnalysisStats *andersen = nullptr, *dyckAA = nullptr, *underApprox = nullptr;
  for (const auto &s : results) {
    if (s.name == "Andersen" && s.initialized) andersen = &s;
    if (s.name == "DyckAA" && s.initialized) dyckAA = &s;
    if (s.name == "UnderApprox" && s.initialized) underApprox = &s;
  }
  
  if (andersen) {
    errs() << "  • Andersen: " 
           << format("%.1f%%", 100.0 * andersen->noAlias / andersen->totalQueries) 
           << " NoAlias, " << andersen->mayAlias + andersen->mustAlias << " aliases\n";
  }
  
  if (dyckAA) {
    errs() << "  • DyckAA: " 
           << format("%.1f%%", 100.0 * dyckAA->noAlias / dyckAA->totalQueries) 
           << " NoAlias, " << dyckAA->mayAlias + dyckAA->mustAlias << " aliases\n";
    if (andersen) {
      errs() << "    → " << format("%.2fx", andersen->timeMs / dyckAA->timeMs) 
             << " faster than Andersen, ";
      if (dyckAA->noAlias > andersen->noAlias) {
        errs() << format("%.1f%%", 100.0 * (dyckAA->noAlias - andersen->noAlias) / andersen->totalQueries)
               << " more precise\n";
      } else {
        errs() << format("%.1f%%", 100.0 * (andersen->noAlias - dyckAA->noAlias) / andersen->totalQueries)
               << " less precise\n";
      }
    }
  }
  
  if (underApprox && andersen) {
    errs() << "  • UnderApprox: " 
           << format("%.2fx", andersen->timeMs / underApprox->timeMs) 
           << " faster than Andersen, " << underApprox->mustAlias << " definite must-alias\n";
  }

  return 0;
}
