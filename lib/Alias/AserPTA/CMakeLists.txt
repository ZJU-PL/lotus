add_library(AserPTA STATIC
    PointerAnalysis/CMDOptions.cpp
    PointerAnalysis/Context/Context.cpp
    PointerAnalysis/Model/DefaultHeapModel.cpp
    PointerAnalysis/Model/DefaultLangModel.cpp
    PointerAnalysis/Model/FieldSensitive/FSLayouts.cpp
    PointerAnalysis/Model/MemModels.cpp
    PointerAnalysis/Program/Program.cpp
    PointerAnalysis/Solver/PointsToSet.cpp
    PreProcessing/IRPreProcessor.cpp
    PreProcessing/Passes/CanonicalizeGEPPass.cpp
    PreProcessing/Passes/InsertGlobalCtorCallPass.cpp
    PreProcessing/Passes/LoweringMemCpyPass.cpp
    PreProcessing/Passes/RemoveASMInstPass.cpp
    PreProcessing/Passes/RemoveExceptionHandlerPass.cpp
    PreProcessing/Passes/StandardHeapAPIRewritePass.cpp
    PreProcessing/Passes/WrapperFunIdentifyPass.cpp
    PreProcessing/PreProcPassManagerBuilder.cpp
    Util/Util.cpp
)

target_compile_options(AserPTA PRIVATE
    -DSPDLOG_ACTIVE_LEVEL=2
)

# Link against LLVM libraries
target_link_libraries(AserPTA PUBLIC
    LLVMCore
    #LLVMSupport
    LLVMTransformUtils
    LLVMAnalysis
)

