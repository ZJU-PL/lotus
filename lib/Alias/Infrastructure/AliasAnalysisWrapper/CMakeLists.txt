llvm_map_components_to_libnames(cli_utils_llvm_libs Core IRReader Support)

# CLI utilities library for alias analysis command-line tools
add_library(CanaryAliasCLIUtils STATIC CLIUtils.cpp)
target_link_libraries(CanaryAliasCLIUtils PUBLIC AliasSpecManager ${cli_utils_llvm_libs})

# Use LLVM's CFL (from libLLVMAnalysis) instead of lotus's CFLAA to avoid duplicate
# symbols when linking with other libs that pull in LLVMAnalysis (e.g. CanaryLLVMUtils).
# Use LLVM's CFL from libLLVMAnalysis (LegacyPassManager is a header-only use here;
# the implementation is in the same translation unit as CFL, so Analysis suffices).
llvm_map_components_to_libnames(alias_wrapper_llvm_libs Analysis)

# Alias Analysis Wrapper library
configure_file(${PROJECT_SOURCE_DIR}/include/Alias/Infrastructure/AliasAnalysisWrapper/BuildConfig.h.in
    ${PROJECT_BINARY_DIR}/include/Alias/Infrastructure/AliasAnalysisWrapper/BuildConfig.h
    @ONLY)
add_library(CanaryAliasAnalysisWrapper STATIC
    AliasAnalysisWrapperCore.cpp
    AliasAnalysisWrapperQueries.cpp
    AliasAnalysisWrapperBackend.cpp
    AliasAnalysisFactory.cpp
    AliasAnalysisConfigParser.cpp
)
# These translation units contain identically named local query helpers.
set_source_files_properties(AliasAnalysisWrapperCore.cpp
    AliasAnalysisWrapperQueries.cpp AliasAnalysisWrapperBackend.cpp
    PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

target_link_libraries(CanaryAliasAnalysisWrapper
    PUBLIC
        Andersen
        CanaryDyckAA
        CanaryUnderApproxAA
        ${alias_wrapper_llvm_libs}
)
target_include_directories(CanaryAliasAnalysisWrapper PUBLIC
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>)
if(LOTUS_AA_WRAPPER_ENABLE_DDA)
  target_link_libraries(CanaryAliasAnalysisWrapper PRIVATE DDA)
endif()
if(LOTUS_AA_WRAPPER_ENABLE_TPA)
  target_link_libraries(CanaryAliasAnalysisWrapper PRIVATE
    FSCSPointerAnalysis TPATransforms TPAUtil)
endif()
if(LOTUS_AA_WRAPPER_ENABLE_CCLYZER)
  target_link_libraries(CanaryAliasAnalysisWrapper PRIVATE LotusCclyzerAA)
endif()
