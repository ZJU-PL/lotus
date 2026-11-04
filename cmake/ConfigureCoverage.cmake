if(LOTUS_ENABLE_COVERAGE)
    if(NOT LOTUS_BUILD_TESTS)
        message(FATAL_ERROR
            "LOTUS_ENABLE_COVERAGE requires LOTUS_BUILD_TESTS=ON")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
        message(FATAL_ERROR
            "LOTUS_ENABLE_COVERAGE requires Clang or AppleClang")
    endif()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate")
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-instr-generate")
endif()
