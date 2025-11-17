# Sprattus: Static Program Analysis Toolkit Using Satisfiability
# Integrated into Lotus framework

# Sprattus library sources
set(SPRATTUS_SOURCES
    utils.cpp
    Analyzer.cpp
    FunctionContext.cpp
    ModuleContext.cpp
    Checks.cpp
    Reporting.cpp
    RepresentedValue.cpp
    ConcreteState.cpp
    Fragment.cpp
    FragmentDecomposition.cpp
    InstructionSemantics.cpp
    ValueMapping.cpp
    Z3APIExtension.cpp
    MemoryModel.cpp
    FloatingPointModel.cpp
    DomainConstructor.cpp
    Config.cpp
    SprattusPass.cpp
    PrettyPrinter.cpp
    ResultStore.cpp
    Expression.cpp
    ParamStrategy.cpp
    domains/NumRels.cpp
    domains/SimpleConstProp.cpp
    domains/BitMask.cpp
    domains/Affine.cpp
    domains/Predicates.cpp
    domains/Intervals.cpp
    domains/MemRange.cpp
    domains/MemRegions.cpp
    domains/Boolean.cpp
)

# Create Sprattus library
add_library(SprattusAnalysis STATIC ${SPRATTUS_SOURCES})

# Link LLVM libraries
llvm_map_components_to_libnames(sprattus_llvm_libs 
    asmparser bitreader bitwriter scalaropts transformutils)

target_link_libraries(SprattusAnalysis PUBLIC
    ${Z3_LIBRARIES}
    ${sprattus_llvm_libs}
)

# Installation
install(TARGETS SprattusAnalysis DESTINATION lib)
