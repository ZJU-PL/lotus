
add_library(CanaryTransform STATIC
        LowerConstantExpr.cpp
        LowerGlobalConstantArraySelect.cpp
        LowerSelect.cpp
        MergeReturn.cpp
        NameBlock.cpp
        RemoveDeadBlock.cpp
        RemoveNoRetFunction.cpp
        SimplifyLatch.cpp
        # LLVM_Optimizer.cpp
        # FlattenInit.cpp
        # SoftFloat.cpp
        UnrollVectors.cpp
)
