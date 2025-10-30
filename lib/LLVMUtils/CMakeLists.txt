add_library(CanaryLLVMUtils STATIC
        API.cpp
        Debug.cpp
        Demangle.cpp
        InstructionUtils.cpp
        RecursiveTimer.cpp
        Statistics.cpp
        StringUtils.cpp
        ThreadPool.cpp
        IO/FileUtils.cpp
        IO/ReadIR.cpp
        IO/WriteIR.cpp
        Scheduler/Task.cpp
        Scheduler/PipelineScheduler.cpp
        Scheduler/ParallelSchedulerPass.cpp
)

# Link against LLVMDemangle since Demangle.cpp uses llvm::demangle()
target_link_libraries(CanaryLLVMUtils PUBLIC LLVMDemangle LLVMAnalysis)

