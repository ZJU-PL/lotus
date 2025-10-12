# Concurrency checker library
add_library(ConcurrencyChecker STATIC
    ConcurrencyChecker.cpp
)

target_include_directories(ConcurrencyChecker
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/include/Checker/concurrency
)

target_link_libraries(ConcurrencyChecker
    PUBLIC
        Concurrency  # MHP and LockSet analysis
        CanarySupport
        ${LLVM_LIBS}
)
