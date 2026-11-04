# IFDS Framework - Independent Analysis Library
#
# Layout: Debug/, Analyses/
# Solver implementations are header-only (include/Dataflow/IFDS/Solver/*.tpp).

# Framework core
set(IFDS_FRAMEWORK_SOURCES
  AnalysisSession.cpp
  Debug/IFDSDebugUtils.cpp
)

# Client analyses
set(IFDS_CLIENT_SOURCES
  Analyses/IFDSTaintAnalysis.cpp
  Analyses/IFDSTaintAnalysisFlows.cpp
  Analyses/IFDSTaintAnalysisReporting.cpp
  Analyses/IFDSReachingDefinitions.cpp
  Analyses/IDEConstantPropagation.cpp
  Analyses/IDETypeState.cpp
  Analyses/IDEProtoAnalysis.cpp
  Analyses/IDEExtendedTaintAnalysis.cpp
  Analyses/IDEFeatureTaintAnalysis.cpp
  Analyses/IDESecureHeapPropagation.cpp
  Analyses/IDEInstInteractionAnalysis.cpp
  Analyses/IDEGeneralizedLCA.cpp
  Analyses/IFDSUninitializedVariables.cpp
  Analyses/IFDSConstAnalysis.cpp
  Analyses/IDELinearConstantAnalysis.cpp
  Analyses/IFDSTypeAnalysis.cpp
  Analyses/IFDSSignAnalysis.cpp
)

set(IFDS_SOURCES ${IFDS_FRAMEWORK_SOURCES} ${IFDS_CLIENT_SOURCES})

add_library(IFDS STATIC ${IFDS_SOURCES})

target_link_libraries(IFDS PUBLIC Annotation CanaryPlatform
  CanaryAliasAnalysisWrapper CanaryTypeHierarchy CanaryCallGraph CanaryCFG)
