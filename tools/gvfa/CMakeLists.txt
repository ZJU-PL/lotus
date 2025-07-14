# Find out what libraries are needed by LLVM
llvm_map_components_to_libnames(LLVM_LINK_COMPONENTS
  IRReader
  Target
  TransformUtils
)

add_executable(gvfa-tool gvfa-tool.cpp)

target_link_libraries(gvfa-tool 
    GVFA
    ${LLVM_LINK_COMPONENTS}
) 