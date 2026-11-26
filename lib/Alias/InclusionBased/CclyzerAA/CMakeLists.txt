# Optional CclyzerAA wrapper (cclyzer++ Datalog-based alias analysis).
# Built only when LOTUS_ENABLE_CCLYZER=ON and cclyzerpp was configured successfully.

add_library(LotusCclyzerAA STATIC CclyzerAA.cpp)

target_include_directories(LotusCclyzerAA
  PUBLIC ${CMAKE_SOURCE_DIR}/include
  PRIVATE ${CMAKE_SOURCE_DIR}/include/Alias/InclusionBased/CclyzerAA)

if(LOTUS_CCLYZERPP_AVAILABLE)
  target_compile_definitions(LotusCclyzerAA PUBLIC LOTUS_HAS_CCLYZERPP=1)
  target_link_libraries(LotusCclyzerAA PRIVATE PAPass)
else()
  target_compile_definitions(LotusCclyzerAA PUBLIC LOTUS_HAS_CCLYZERPP=0)
endif()

if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(LotusCclyzerAA PRIVATE -fno-rtti)
endif()
add_library(Lotus::CclyzerAA ALIAS LotusCclyzerAA)
