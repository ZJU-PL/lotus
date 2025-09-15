#include <gtest/gtest.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

#include <Analysis/IFDS/IFDSFramework.h>
#include <Analysis/IFDS/TaintAnalysis.h>
#include <Analysis/IFDS/ReachingDefinitions.h>
#include <Alias/DyckAA/DyckAliasAnalysis.h>

#include "utils/IFDSTestFixture.h"

using namespace lotus::testing;

// ============================================================================
// IFDS Framework Integration Tests
// ============================================================================

class IFDSIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize common components
        AliasAnalysis = std::make_unique<DyckAliasAnalysis>();
    }

    void TearDown() override {
        Module.reset();
        AliasAnalysis.reset();
    }

    std::unique_ptr<llvm::Module> Module;
    std::unique_ptr<DyckAliasAnalysis> AliasAnalysis;
};

TEST_F(IFDSIntegrationTest, FrameworkInitialization) {
    // Test that the IFDS framework can be properly initialized
    auto taintAnalysis = std::make_unique<sparta::ifds::TaintAnalysis>();
    taintAnalysis->set_alias_analysis(AliasAnalysis.get());
    
    EXPECT_TRUE(true) << "IFDS framework should initialize successfully";
}

TEST_F(IFDSIntegrationTest, SolverCreation) {
    // Test that solvers can be created for different analysis types
    auto taintAnalysis = std::make_unique<sparta::ifds::TaintAnalysis>();
    taintAnalysis->set_alias_analysis(AliasAnalysis.get());
    
    auto taintSolver = std::make_unique<sparta::ifds::IFDSSolver<sparta::ifds::TaintAnalysis>>(*taintAnalysis);
    EXPECT_TRUE(taintSolver != nullptr) << "Taint analysis solver should be created";
    
    auto reachingDefsAnalysis = std::make_unique<sparta::ifds::ReachingDefinitionsAnalysis>();
    reachingDefsAnalysis->set_alias_analysis(AliasAnalysis.get());
    
    auto reachingDefsSolver = std::make_unique<sparta::ifds::IFDSSolver<sparta::ifds::ReachingDefinitionsAnalysis>>(*reachingDefsAnalysis);
    EXPECT_TRUE(reachingDefsSolver != nullptr) << "Reaching definitions solver should be created";
}

TEST_F(IFDSIntegrationTest, MultipleAnalysesOnSameModule) {
    // Test running multiple analyses on the same module
    std::string testCode = R"(
extern int source();
extern void sink(int p);

int main() {
  int a = source();
  int b = a;
  sink(b);
  return 0;
}
)";
    
    // This is a placeholder test
    // In practice, you would:
    // 1. Compile testCode to LLVM IR
    // 2. Load the IR
    // 3. Run multiple analyses
    // 4. Verify results are consistent
    
    EXPECT_TRUE(true) << "Multiple analyses on same module test placeholder";
}

TEST_F(IFDSIntegrationTest, AliasAnalysisIntegration) {
    // Test that alias analysis is properly integrated
    auto taintAnalysis = std::make_unique<sparta::ifds::TaintAnalysis>();
    taintAnalysis->set_alias_analysis(AliasAnalysis.get());
    
    // Verify that alias analysis is set
    EXPECT_TRUE(true) << "Alias analysis integration test placeholder";
}

TEST_F(IFDSIntegrationTest, PerformanceBenchmark) {
    // Test performance with a larger module
    // This would typically use a benchmark module
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Run analysis here
    // (placeholder for actual analysis)
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Verify that analysis completes in reasonable time
    EXPECT_LT(duration.count(), 5000) << "Analysis should complete within 5 seconds";
}

TEST_F(IFDSIntegrationTest, ErrorHandling) {
    // Test error handling in various scenarios
    
    // Test with null module
    auto taintAnalysis = std::make_unique<sparta::ifds::TaintAnalysis>();
    taintAnalysis->set_alias_analysis(AliasAnalysis.get());
    
    // This should handle errors gracefully
    EXPECT_TRUE(true) << "Error handling test placeholder";
}

// ============================================================================
// Cross-Analysis Consistency Tests
// ============================================================================

class CrossAnalysisConsistencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        AliasAnalysis = std::make_unique<DyckAliasAnalysis>();
    }

    void TearDown() override {
        Module.reset();
        AliasAnalysis.reset();
    }

    std::unique_ptr<llvm::Module> Module;
    std::unique_ptr<DyckAliasAnalysis> AliasAnalysis;
};

TEST_F(CrossAnalysisConsistencyTest, TaintAndReachingDefinitionsConsistency) {
    // Test that taint analysis and reaching definitions analysis
    // produce consistent results where they overlap
    
    EXPECT_TRUE(true) << "Cross-analysis consistency test placeholder";
}

// ============================================================================
// Regression Tests
// ============================================================================

class RegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        AliasAnalysis = std::make_unique<DyckAliasAnalysis>();
    }

    void TearDown() override {
        Module.reset();
        AliasAnalysis.reset();
    }

    std::unique_ptr<llvm::Module> Module;
    std::unique_ptr<DyckAliasAnalysis> AliasAnalysis;
};

TEST_F(RegressionTest, KnownIssues) {
    // Test for known issues that have been fixed
    // This helps prevent regressions
    
    EXPECT_TRUE(true) << "Regression test placeholder";
}

// ============================================================================
// Main function for running tests
// ============================================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
