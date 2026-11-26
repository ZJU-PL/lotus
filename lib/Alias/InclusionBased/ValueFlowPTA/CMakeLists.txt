add_library(ValueFlowPTA STATIC
  ValueFlowGraph.cpp
  ValueFlowPTA.cpp)

target_link_libraries(ValueFlowPTA
  PUBLIC
    LLVMCore
    LLVMSupport
  PRIVATE
    AliasSpecManager)

add_library(Lotus::ValueFlowPTA ALIAS ValueFlowPTA)
