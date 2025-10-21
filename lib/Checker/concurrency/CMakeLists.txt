# Concurrency checker library
add_library(ConcurrencyChecker STATIC
    ConcurrencyChecker.cpp
)

target_link_libraries(ConcurrencyChecker
    PUBLIC
        Concurrency  # MHP and LockSet analysis
)
