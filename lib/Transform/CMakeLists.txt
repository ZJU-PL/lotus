
add_library(CanaryTransform STATIC
        LowerConstantExpr.cpp
        LowerGlobalConstantArraySelect.cpp
        LowerSelect.cpp
        MergeReturn.cpp
        ModuleOptimizer.cpp
        NameBlock.cpp
        RemoveDeadBlock.cpp
        RemoveNoRetFunction.cpp
        SimplifyLatch.cpp
        # FlattenInit.cpp
        # SoftFloat.cpp
        UnrollVectors.cpp
        ExpandAssume.cpp
        ElimPhi.cpp
)
