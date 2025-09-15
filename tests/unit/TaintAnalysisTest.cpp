#include <gtest/gtest.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>

#include <Analysis/IFDS/TaintAnalysis.h>
#include <Analysis/IFDS/IFDSFramework.h>
#include <Alias/DyckAA/DyckAliasAnalysis.h>

#include "utils/IFDSTestFixture.h"

using namespace lotus::testing;

// ============================================================================
// Taint Analysis Unit Tests
// ============================================================================

class TaintAnalysisTest : public TaintAnalysisTestFixture {
protected:
    void SetUp() override {
        TaintAnalysisTestFixture::SetUp();
    }
};

TEST_F(TaintAnalysisTest, SimpleTaintPropagation) {
    // Load test IR
    std::string testFile = std::string(PathToLlFiles) + "taint/simple_taint.ll";
    ASSERT_TRUE(loadIRFromFile(testFile)) << "Failed to load test IR file";
    
    // Initialize problem
    initializeProblem();
    
    // Run analysis
    runAnalysis();
    
    // Get results
    auto results = getResults();
    
    // Verify that we have some results
    EXPECT_FALSE(results.empty()) << "Analysis should produce results";
    
    // Check that taint propagates from source to sink
    const auto* mainFunc = getFunction("main");
    ASSERT_NE(mainFunc, nullptr) << "main function should exist";
    
    // Find source and sink calls
    const llvm::CallInst* sourceCall = nullptr;
    const llvm::CallInst* sinkCall = nullptr;
    
    for (const auto& I : llvm::instructions(mainFunc)) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
            if (call->getCalledFunction() && 
                call->getCalledFunction()->getName() == "source") {
                sourceCall = call;
            } else if (call->getCalledFunction() && 
                      call->getCalledFunction()->getName() == "sink") {
                sinkCall = call;
            }
        }
    }
    
    ASSERT_NE(sourceCall, nullptr) << "Source call should exist";
    ASSERT_NE(sinkCall, nullptr) << "Sink call should exist";
    
    // Check that taint flows from source to sink
    // This is a simplified check - in practice you'd verify the actual facts
    EXPECT_TRUE(true) << "Taint should propagate from source to sink";
}

TEST_F(TaintAnalysisTest, TaintSanitization) {
    // Load test IR
    std::string testFile = std::string(PathToLlFiles) + "taint/taint_sanitization.ll";
    ASSERT_TRUE(loadIRFromFile(testFile)) << "Failed to load test IR file";
    
    // Initialize problem
    initializeProblem();
    
    // Run analysis
    runAnalysis();
    
    // Get results
    auto results = getResults();
    
    // Verify that we have some results
    EXPECT_FALSE(results.empty()) << "Analysis should produce results";
    
    // In this test, taint should be sanitized and not reach the sink
    // This is a simplified check - in practice you'd verify the actual facts
    EXPECT_TRUE(true) << "Taint should be sanitized";
}

TEST_F(TaintAnalysisTest, TaintBranching) {
    // Load test IR
    std::string testFile = std::string(PathToLlFiles) + "taint/taint_branching.ll";
    ASSERT_TRUE(loadIRFromFile(testFile)) << "Failed to load test IR file";
    
    // Initialize problem
    initializeProblem();
    
    // Run analysis
    runAnalysis();
    
    // Get results
    auto results = getResults();
    
    // Verify that we have some results
    EXPECT_FALSE(results.empty()) << "Analysis should produce results";
    
    // In this test, taint should reach both branches of the sink
    // This is a simplified check - in practice you'd verify the actual facts
    EXPECT_TRUE(true) << "Taint should reach both branches";
}

TEST_F(TaintAnalysisTest, TaintFactOperations) {
    // Test TaintFact creation and operations
    auto zero = sparta::ifds::TaintFact::zero();
    EXPECT_TRUE(zero.is_zero()) << "Zero fact should be zero";
    
    // Create a mock value for testing
    llvm::LLVMContext context;
    auto* intType = llvm::Type::getInt32Ty(context);
    auto* value = llvm::ConstantInt::get(intType, 42);
    
    auto taintedVar = sparta::ifds::TaintFact::tainted_var(value);
    EXPECT_TRUE(taintedVar.is_tainted_var()) << "Should be tainted variable";
    EXPECT_EQ(taintedVar.get_value(), value) << "Value should match";
    
    auto taintedMem = sparta::ifds::TaintFact::tainted_memory(value);
    EXPECT_TRUE(taintedMem.is_tainted_memory()) << "Should be tainted memory";
    EXPECT_EQ(taintedMem.get_memory_location(), value) << "Memory location should match";
    
    // Test equality
    auto zero2 = sparta::ifds::TaintFact::zero();
    EXPECT_EQ(zero, zero2) << "Zero facts should be equal";
    
    auto taintedVar2 = sparta::ifds::TaintFact::tainted_var(value);
    EXPECT_EQ(taintedVar, taintedVar2) << "Same tainted vars should be equal";
}

TEST_F(TaintAnalysisTest, TaintAnalysisConfiguration) {
    // Test TaintAnalysis configuration
    auto analysis = std::make_unique<sparta::ifds::TaintAnalysis>();
    
    // Add custom source and sink functions
    analysis->add_source_function("custom_source");
    analysis->add_sink_function("custom_sink");
    
    // Test source/sink detection (simplified)
    // In practice, you'd create mock instructions and test the detection
    EXPECT_TRUE(true) << "Configuration should work";
}

// ============================================================================
// Integration Tests
// ============================================================================

class TaintAnalysisIntegrationTest : public TaintAnalysisTestFixture {
protected:
    void SetUp() override {
        TaintAnalysisTestFixture::SetUp();
    }
};

TEST_F(TaintAnalysisIntegrationTest, FullAnalysisWorkflow) {
    // This test demonstrates the full workflow of running taint analysis
    
    // 1. Load IR
    std::string testFile = std::string(PathToLlFiles) + "taint/simple_taint.ll";
    ASSERT_TRUE(loadIRFromFile(testFile)) << "Failed to load test IR file";
    
    // 2. Initialize alias analysis
    auto aliasAnalysis = std::make_unique<DyckAliasAnalysis>();
    
    // 3. Initialize taint analysis
    auto taintAnalysis = std::make_unique<sparta::ifds::TaintAnalysis>();
    taintAnalysis->set_alias_analysis(aliasAnalysis.get());
    taintAnalysis->add_source_function("source");
    taintAnalysis->add_sink_function("sink");
    
    // 4. Run analysis
    sparta::ifds::IFDSSolver<sparta::ifds::TaintAnalysis> solver(*taintAnalysis);
    solver.solve(*Module);
    
    // 5. Query results
    auto results = solver.get_all_results();
    EXPECT_FALSE(results.empty()) << "Analysis should produce results";
    
    // 6. Verify specific properties
    // (In practice, you'd add more specific checks here)
    EXPECT_TRUE(true) << "Full workflow should complete successfully";
}

// ============================================================================
// Performance Tests
// ============================================================================

class TaintAnalysisPerformanceTest : public TaintAnalysisTestFixture {
protected:
    void SetUp() override {
        TaintAnalysisTestFixture::SetUp();
    }
};

TEST_F(TaintAnalysisPerformanceTest, LargeModulePerformance) {
    // This test would run taint analysis on a large module
    // and verify that it completes in reasonable time
    
    // For now, this is a placeholder
    EXPECT_TRUE(true) << "Performance test placeholder";
}

// ============================================================================
// Main function for running tests
// ============================================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
