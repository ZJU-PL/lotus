# CLAM and CRAB configuration
option(ENABLE_CLAM "Enable CLAM abstract interpretation framework" ON)
option(DOWNLOAD_CRAB "Download and build CRAB if not found" ON)
set(CUSTOM_CRAB_ROOT "" CACHE PATH "Path to custom CRAB installation (optional)")

if(ENABLE_CLAM)
    # Determine CRAB root directory
    if(CUSTOM_CRAB_ROOT)
        set(CRAB_ROOT ${CUSTOM_CRAB_ROOT})
        message(STATUS "Using custom CRAB at: ${CRAB_ROOT}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/crab/CMakeLists.txt")
        # Use manually cloned CRAB in source tree
        set(CRAB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/crab")
        message(STATUS "Using CRAB from source tree: ${CRAB_ROOT}")
    elseif(DOWNLOAD_CRAB)
        # Download CRAB to build/deps at configure time
        set(CRAB_ROOT "${CMAKE_BINARY_DIR}/deps/crab")
        if(NOT EXISTS "${CRAB_ROOT}/CMakeLists.txt")
            find_package(Git REQUIRED)
            message(STATUS "CRAB not found, downloading to ${CRAB_ROOT}...")
            
            # Clone CRAB at configure time
            execute_process(
                COMMAND ${GIT_EXECUTABLE} clone https://github.com/seahorn/crab.git ${CRAB_ROOT}
                RESULT_VARIABLE GIT_RESULT
                OUTPUT_VARIABLE GIT_OUTPUT
                ERROR_VARIABLE GIT_ERROR
            )
            
            if(NOT GIT_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to clone CRAB: ${GIT_ERROR}")
            endif()
            
            if(NOT EXISTS "${CRAB_ROOT}/CMakeLists.txt")
                message(FATAL_ERROR "CRAB clone succeeded but CMakeLists.txt not found")
            endif()
            
            message(STATUS "CRAB successfully downloaded to ${CRAB_ROOT}")
        else()
            message(STATUS "Using previously downloaded CRAB at: ${CRAB_ROOT}")
        endif()
    else()
        message(WARNING 
            "CRAB not found. Either:\n"
            "  1. Set -DCUSTOM_CRAB_ROOT=/path/to/crab, or\n"
            "  2. Clone manually: git clone https://github.com/seahorn/crab.git\n"
            "  3. Enable auto-download with -DDOWNLOAD_CRAB=ON\n"
            "CLAM will be disabled.")
        set(ENABLE_CLAM OFF)
    endif()
    
    # Configure CRAB if found
    if(ENABLE_CLAM AND EXISTS "${CRAB_ROOT}/CMakeLists.txt")
        message(STATUS "Configuring CRAB at: ${CRAB_ROOT}")
        
        # Set CRAB options before including it
        set(CRAB_BUILD_LIBS_SHARED OFF)
        set(CRAB_USE_LDD OFF)
        set(CRAB_USE_APRON OFF)
        set(CRAB_USE_ELINA OFF)
        
        # Add CRAB as subdirectory
        add_subdirectory(${CRAB_ROOT} ${CMAKE_BINARY_DIR}/crab)
        
        # Include CRAB headers
        include_directories(BEFORE ${CRAB_ROOT}/include)
        
        # Configure CLAM config.h
        set(HAVE_LLVM_SEAHORN FALSE)
        set(USE_DBM_BIGNUM FALSE)
        set(USE_DBM_SAFEINT FALSE)
        set(INCLUDE_ALL_DOMAINS TRUE)
        set(CLAM_IS_TOPLEVEL FALSE)
        
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/include/clam/config.h.cmake
            ${CMAKE_BINARY_DIR}/include/clam/config.h
        )
        
        include_directories(BEFORE ${CMAKE_BINARY_DIR}/include)
        add_definitions(-DHAVE_CLAM)
        message(STATUS "CLAM integration enabled with CRAB")
    endif()
endif()

