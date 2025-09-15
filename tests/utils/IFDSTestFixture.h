#ifndef LOTUS_TESTS_UTILS_IFDSTESTFIXTURE_H
#define LOTUS_TESTS_UTILS_IFDSTESTFIXTURE_H

#include <gtest/gtest.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <Analysis/IFDS/IFDSFramework.h>
#include <Analysis/IFDS/TaintAnalysis.h>
#include <Alias/DyckAA/DyckAliasAnalysis.h>

#include "TestConfig.h"
#include "SourceLocationEntry.h"

namespace lotus {
namespace testing {

// ============================================================================
// Base IFDS Test Fixture
// ============================================================================

template<typename ProblemType>
class IFDSTestFixture : public ::testing::Test {
protected:
    std::unique_ptr<llvm::Module> Module;
    std::unique_ptr<ProblemType> Problem;
    std::unique_ptr<DyckAliasAnalysis> AliasAnalysis;
    
    static constexpr auto PathToLlFiles = LOTUS_BUILD_SUBFOLDER("");
    static inline const std::vector<std::string> EntryPoints = {"main"};

    void SetUp() override {
        // Initialize alias analysis
        AliasAnalysis = std::make_unique<DyckAliasAnalysis>();
    }

    void TearDown() override {
        Module.reset();
        Problem.reset();
        AliasAnalysis.reset();
    }

    // Load LLVM IR from file
    bool loadIRFromFile(const std::string& filename) {
        llvm::SMDiagnostic Err;
        Module = llvm::parseIRFile(filename, Err, llvm::getGlobalContext());
        
        if (!Module) {
            llvm::errs() << "Error loading IR file: " << filename << "\n";
            Err.print("IRLoader", llvm::errs());
            return false;
        }
        
        return true;
    }

    // Initialize the analysis problem
    void initializeProblem() {
        if (!Module) {
            GTEST_FAIL() << "Module not loaded. Call loadIRFromFile first.";
            return;
        }
        
        Problem = std::make_unique<ProblemType>();
        Problem->set_alias_analysis(AliasAnalysis.get());
    }

    // Run the analysis
    void runAnalysis() {
        if (!Problem || !Module) {
            GTEST_FAIL() << "Problem or Module not initialized.";
            return;
        }
        
        sparta::ifds::IFDSSolver<ProblemType> solver(*Problem);
        solver.solve(*Module);
    }

    // Get analysis results
    auto getResults() {
        if (!Problem || !Module) {
            GTEST_FAIL() << "Problem or Module not initialized.";
            return typename ProblemType::FactSet{};
        }
        
        sparta::ifds::IFDSSolver<ProblemType> solver(*Problem);
        solver.solve(*Module);
        return solver.get_all_results();
    }

    // Helper to find function by name
    const llvm::Function* getFunction(const std::string& name) const {
        if (!Module) return nullptr;
        return Module->getFunction(name);
    }

    // Helper to find instruction by line number (simplified)
    const llvm::Instruction* getInstructionAtLine(const llvm::Function* F, uint32_t line) const {
        if (!F) return nullptr;
        
        uint32_t currentLine = 1;
        for (const auto& I : llvm::instructions(F)) {
            if (currentLine == line) {
                return &I;
            }
            currentLine++;
        }
        return nullptr;
    }
};

// ============================================================================
// Taint Analysis Test Fixture
// ============================================================================

class TaintAnalysisTestFixture : public IFDSTestFixture<sparta::ifds::TaintAnalysis> {
protected:
    using Fact = sparta::ifds::TaintFact;
    using FactSet = typename sparta::ifds::TaintAnalysis::FactSet;
    using GroundTruthTy = std::map<TestingSrcLocation, std::set<TestingSrcLocation>>;

    void SetUp() override {
        IFDSTestFixture::SetUp();
        
        // Set up default source and sink functions
        if (Problem) {
            Problem->add_source_function("source");
            Problem->add_source_function("read");
            Problem->add_sink_function("sink");
            Problem->add_sink_function("write");
        }
    }

    // Compare analysis results with ground truth
    void compareResults(const std::map<const llvm::Instruction*, FactSet>& Results, 
                       const GroundTruthTy& GroundTruth) {
        auto GroundTruthEntries = convertTestingLocationSetMapInIR(
            GroundTruth, Module.get());
        
        // Convert results to comparable format
        std::map<const llvm::Instruction*, std::set<const llvm::Value*>> ActualResults;
        for (const auto& [Inst, Facts] : Results) {
            std::set<const llvm::Value*> Values;
            for (const auto& Fact : Facts) {
                if (Fact.is_tainted_var()) {
                    Values.insert(Fact.get_value());
                }
            }
            if (!Values.empty()) {
                ActualResults[Inst] = Values;
            }
        }
        
        EXPECT_EQ(ActualResults, GroundTruthEntries)
            << "Taint Analysis results do not match ground truth";
    }

    // Helper to create ground truth entries
    GroundTruthTy createGroundTruth() {
        return GroundTruthTy{};
    }

    // Helper to add expected leak
    void addExpectedLeak(GroundTruthTy& GroundTruth, 
                        const TestingSrcLocation& Source,
                        const TestingSrcLocation& Sink) {
        GroundTruth[Source].insert(Sink);
    }
};

// ============================================================================
// Test Data Generation Helpers
// ============================================================================

class TestDataGenerator {
public:
    // Generate a simple taint test case
    static std::string generateSimpleTaintTest() {
        return R"(
extern int source();
extern void sink(int p);

int main() {
  int a = source();
  sink(a);
  return 0;
}
)";
    }

    // Generate a taint test with sanitization
    static std::string generateTaintSanitizationTest() {
        return R"(
extern int source();
extern void sink(int p);
extern int sanitize(int x);

int main() {
  int a = source();
  int b = sanitize(a);
  sink(b);
  return 0;
}
)";
    }

    // Generate a taint test with branching
    static std::string generateTaintBranchingTest() {
        return R"(
extern int source();
extern void sink(int p);

int main() {
  int a = source();
  if (a > 0) {
    sink(a);
  } else {
    sink(a);
  }
  return 0;
}
)";
    }
};

} // namespace testing
} // namespace lotus

#endif // LOTUS_TESTS_UTILS_IFDSTESTFIXTURE_H
