add_executable(lotus-cfl-solve lotus-cfl-solve.cpp)
target_link_libraries(lotus-cfl-solve PRIVATE CanaryClassicalCFL)

add_executable(lotus-cfl-subcubic-aa lotus-cfl-subcubic-aa.cpp)
target_link_libraries(lotus-cfl-subcubic-aa PRIVATE CanaryClassicalCFL)

add_executable(lotus-cfl-alias lotus-cfl-alias.cpp)
target_link_libraries(lotus-cfl-alias PRIVATE
    CanaryClassicalCFLAliasClient
    LLVMIRReader
    LLVMAsmParser
)

add_executable(lotus-cfl-vf lotus-cfl-vf.cpp)
target_link_libraries(lotus-cfl-vf PRIVATE
    CanaryClassicalCFLValueFlowClient
    LLVMIRReader
    LLVMAsmParser
)
