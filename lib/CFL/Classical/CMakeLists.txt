add_library(CanaryClassicalCFL STATIC
    Core/Grammar.cpp
    Core/Graph.cpp
    Core/Relation.cpp
    Core/RecursiveStateMachine.cpp
    Core/Validation.cpp
    Solvers/SolverSession.cpp
    Solvers/ConstraintGrounding.cpp
    Solvers/Engines/Common/Reachability.cpp
    Solvers/Engines/Common/BatchSolverEngine.cpp
    Solvers/Engines/CAT/ContextAwareTabulation.cpp
    Solvers/Engines/IEOCE/IterativeEpoch.cpp
    Solvers/Engines/TransitiveClosure.cpp
    Solvers/Engines/POCR/ClientGrammars.cpp
    Solvers/Engines/POCR/FullyOrderedClosure.cpp
    Solvers/Engines/POCR/PairedTreeClosure.cpp
    Solvers/Engines/PEARL/PearlEngine.cpp
    Solvers/Engines/EndpointQuotient/EndpointQuotient.cpp
    Solvers/Engines/EndpointQuotient/EndpointQuotientEngine.cpp
    Solvers/Engines/EndpointQuotient/EndpointQuotientStaging.cpp
    Solvers/Engines/SQID/SqidEngine.cpp
    Solvers/Engines/CERT/CertCFL.cpp
    Solvers/Engines/CERT/CertCFLEngine.cpp
    Solvers/Engines/Skewed/SkewedTabulation.cpp
    Solvers/Engines/Skewed/SkewedTabulationEngine.cpp
    Solvers/Engines/STG/StagedSolver.cpp
    Solvers/Engines/SubcubicAA/SubcubicAA.cpp
    Solvers/Engines/SubcubicAA/SubcubicAAAdapter.cpp
    Solvers/Preprocessing/GraphSimplification.cpp
    Solvers/Preprocessing/RSMFoldability.cpp
)

target_link_libraries(CanaryClassicalCFL PUBLIC
    LLVMSupport
)

add_library(CanaryClassicalCFLAliasClient STATIC
    Clients/Alias/AliasClient.cpp
    Clients/Alias/LLVMAliasAnalysis.cpp
)

target_link_libraries(CanaryClassicalCFLAliasClient PUBLIC
    CanaryClassicalCFL
    AserPTA
    LLVMCore
)

add_library(CanaryClassicalCFLValueFlowClient STATIC
    Clients/ValueFlow/ValueFlowClient.cpp
    Clients/ValueFlow/SVFGPreparation.cpp
)

target_link_libraries(CanaryClassicalCFLValueFlowClient PUBLIC
    CanaryClassicalCFL
    SVFG
    CanaryICFG
)
