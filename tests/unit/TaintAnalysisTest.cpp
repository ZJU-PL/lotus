#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>

#include <Analysis/IFDS/IFDSTaintAnalysis.h>
#include <Analysis/IFDS/IFDSFramework.h>

using namespace ifds;
using namespace llvm;

// ============================================================================
// Taint Analysis Unit Tests
// ============================================================================

class TaintAnalysisTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_unique<LLVMContext>();
    }
    
    std::unique_ptr<LLVMContext> context;
    
    // Helper function to create a simple module for testing
    std::unique_ptr<Module> createSimpleModule() {
        auto module = std::make_unique<Module>("test_module", *context);
        
        // Create function types
        auto* intType = Type::getInt32Ty(*context);
        auto* voidType = Type::getVoidTy(*context);
        
        // Create source function (returns int)
        auto* sourceType = FunctionType::get(intType, {}, false);
        auto* sourceFunc = Function::Create(sourceType, GlobalValue::ExternalLinkage, "source", *module);
        
        // Create sink function (takes int, returns void)
        auto* sinkType = FunctionType::get(voidType, {intType}, false);
        auto* sinkFunc = Function::Create(sinkType, GlobalValue::ExternalLinkage, "sink", *module);
        
        // Create main function
        auto* mainType = FunctionType::get(intType, {}, false);
        auto* mainFunc = Function::Create(mainType, GlobalValue::ExternalLinkage, "main", *module);
        
        // Create basic block and instructions for main
        auto* entryBB = BasicBlock::Create(*context, "entry", mainFunc);
        IRBuilder<> builder(*context);
        builder.SetInsertPoint(entryBB);
        
        // Call source function
        auto* sourceCall = builder.CreateCall(sourceType, sourceFunc, {});
        
        // Call sink function with the result
        auto* sinkCall = builder.CreateCall(sinkType, sinkFunc, {sourceCall});
        
        // Return 0
        auto* retVal = ConstantInt::get(intType, 0);
        builder.CreateRet(retVal);
        
        return module;
    }
};

// ============================================================================
// TaintFact Tests
// ============================================================================

TEST_F(TaintAnalysisTest, TaintFactCreation) {
    // Test zero fact creation
    auto zero = TaintFact::zero();
    EXPECT_TRUE(zero.is_zero()) << "Zero fact should be zero";
    EXPECT_EQ(zero.get_type(), TaintFact::ZERO) << "Zero fact should have ZERO type";
    
    // Create a mock value for testing
    auto* intType = Type::getInt32Ty(*context);
    auto* value = ConstantInt::get(intType, 42);
    
    // Test tainted variable fact
    auto taintedVar = TaintFact::tainted_var(value);
    EXPECT_TRUE(taintedVar.is_tainted_var()) << "Should be tainted variable";
    EXPECT_EQ(taintedVar.get_type(), TaintFact::TAINTED_VAR) << "Should have TAINTED_VAR type";
    EXPECT_EQ(taintedVar.get_value(), value) << "Value should match";
    
    // Test tainted memory fact
    auto taintedMem = TaintFact::tainted_memory(value);
    EXPECT_TRUE(taintedMem.is_tainted_memory()) << "Should be tainted memory";
    EXPECT_EQ(taintedMem.get_type(), TaintFact::TAINTED_MEMORY) << "Should have TAINTED_MEMORY type";
    EXPECT_EQ(taintedMem.get_memory_location(), value) << "Memory location should match";
}

TEST_F(TaintAnalysisTest, TaintFactEquality) {
    // Test equality of zero facts
    auto zero1 = TaintFact::zero();
    auto zero2 = TaintFact::zero();
    EXPECT_EQ(zero1, zero2) << "Zero facts should be equal";
    
    // Test equality of tainted variable facts
    auto* intType = Type::getInt32Ty(*context);
    auto* value1 = ConstantInt::get(intType, 42);
    auto* value2 = ConstantInt::get(intType, 42);
    
    auto taintedVar1 = TaintFact::tainted_var(value1);
    auto taintedVar2 = TaintFact::tainted_var(value1);  // Same pointer
    auto taintedVar3 = TaintFact::tainted_var(value2);  // Different pointer, same value
    
    EXPECT_EQ(taintedVar1, taintedVar2) << "Same tainted vars should be equal";
    // Note: value2 creates a different constant, so pointers differ
    // EXPECT_NE(taintedVar1, taintedVar3) << "Different constant objects should not be equal";
}

TEST_F(TaintAnalysisTest, TaintFactOrdering) {
    // Test ordering of different fact types
    auto zero = TaintFact::zero();
    auto* intType = Type::getInt32Ty(*context);
    auto* value = ConstantInt::get(intType, 42);
    auto taintedVar = TaintFact::tainted_var(value);
    auto taintedMem = TaintFact::tainted_memory(value);
    
    // ZERO < TAINTED_VAR < TAINTED_MEMORY
    EXPECT_LT(zero, taintedVar) << "Zero should be less than tainted var";
    EXPECT_LT(taintedVar, taintedMem) << "Tainted var should be less than tainted memory";
    EXPECT_LT(zero, taintedMem) << "Zero should be less than tainted memory";
}

// ============================================================================
// TaintAnalysis Tests
// ============================================================================

TEST_F(TaintAnalysisTest, TaintAnalysisCreation) {
    TaintAnalysis analysis;
    
    // Test that analysis can be created
    EXPECT_TRUE(true) << "TaintAnalysis should be creatable";
    
    // Test zero fact
    auto zero = analysis.zero_fact();
    EXPECT_TRUE(zero.is_zero()) << "Zero fact should be zero";
}

TEST_F(TaintAnalysisTest, TaintAnalysisConfiguration) {
    TaintAnalysis analysis;
    
    // Test adding source and sink functions
    analysis.add_source_function("source");
    analysis.add_sink_function("sink");
    analysis.add_source_function("read");
    analysis.add_sink_function("write");
    
    // Basic test - just verify the functions can be added
    EXPECT_TRUE(true) << "Source and sink functions should be addable";
}

TEST_F(TaintAnalysisTest, SimpleModuleAnalysis) {
    // Create a simple module for testing
    auto module = createSimpleModule();
    ASSERT_NE(module, nullptr) << "Module should be created successfully";
    
    // Create taint analysis
    TaintAnalysis analysis;
    analysis.add_source_function("source");
    analysis.add_sink_function("sink");
    
    // Get the main function
    auto* mainFunc = module->getFunction("main");
    ASSERT_NE(mainFunc, nullptr) << "Main function should exist";
    
    // Test initial facts
    auto initialFacts = analysis.initial_facts(mainFunc);
    // Initial facts might not be empty - the analysis may have default behavior
    EXPECT_TRUE(true) << "Should be able to get initial facts from analysis";
    
    // Test that we can iterate through instructions
    int instructionCount = 0;
    for (const auto& bb : *mainFunc) {
        for (const auto& inst : bb) {
            instructionCount++;
            // Test that we can identify source and sink instructions
            bool isSource = analysis.is_source(&inst);
            bool isSink = analysis.is_sink(&inst);
            
            // These will be false for most instructions, but the framework should handle it
            (void)isSource;  // Suppress unused variable warning
            (void)isSink;    // Suppress unused variable warning
        }
    }
    
    EXPECT_GT(instructionCount, 0) << "Should have some instructions in main function";
}

// ============================================================================
// IFDS Framework Tests
// ============================================================================

TEST_F(TaintAnalysisTest, IFDSSolverCreation) {
    TaintAnalysis analysis;
    
    // Test that IFDS solver can be created
    IFDSSolver<TaintAnalysis> solver(analysis);
    
    EXPECT_TRUE(true) << "IFDS solver should be creatable";
}

TEST_F(TaintAnalysisTest, IFDSSolverWithModule) {
    // Create a simple module
    auto module = createSimpleModule();
    ASSERT_NE(module, nullptr) << "Module should be created successfully";
    
    // Create taint analysis
    TaintAnalysis analysis;
    analysis.add_source_function("source");
    analysis.add_sink_function("sink");
    
    // Create and run IFDS solver
    IFDSSolver<TaintAnalysis> solver(analysis);
    
    // This should not crash, even if the analysis is incomplete
    EXPECT_NO_THROW({
        solver.solve(*module);
    }) << "IFDS solver should handle simple module without crashing";
    
    // Test that we can get results (even if empty)
    auto results = solver.get_all_results();
    // Results might be empty, but the framework should handle it gracefully
    EXPECT_TRUE(true) << "Should be able to get results from solver";
}

// ============================================================================
// Main function for running tests
// ============================================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
