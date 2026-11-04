add_library(CanaryDebugInfo STATIC
    DebugInfoAnalysis.cpp
    DebugInfoQueries.cpp
    IRExpressionRenderer.cpp
    MetadataManager/MetadataEntry.cpp
    MetadataManager/LoopStructure.cpp
    MetadataManager/MetadataManager.cpp
    MetadataManager/MetadataManagerModule.cpp
    MetadataManager/MetadataManagerInstruction.cpp
    MetadataManager/MetadataManagerAnnotation.cpp
    UniqueIR/UniqueIRConstants.cpp
    UniqueIR/UniqueIRReader.cpp
    UniqueIR/UniqueIRMarker.cpp
    UniqueIR/UniqueIRVerifier.cpp
    UniqueIR/IDToValueMapper.cpp
    UniqueIR/UniqueIRMarkerPass.cpp
)
target_link_libraries(CanaryDebugInfo CanaryLLVMUtils)
