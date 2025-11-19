add_library(CanaryKint STATIC
    MKintPass.cpp
    MKintPass_bugreport.cpp
    RangeAnalysis.cpp
    KINTTaintAnalysis.cpp
    BugDetection.cpp
    Utils.cpp
    Options.cpp
    Log.cpp
)

target_link_libraries(CanaryKint 
        CanaryReport
        ${Z3_LIBRARIES}
)
