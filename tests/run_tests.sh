#!/bin/bash

# Lotus IFDS Testing Script
# Based on PhASAR testing infrastructure

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Default values
BUILD_DIR=""
TEST_TYPE="all"
VERBOSE=false
COVERAGE=false
PARALLEL_JOBS=$(nproc)

# Function to print usage
usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "Options:"
    echo "  -b, --build-dir DIR     Build directory (default: ../build)"
    echo "  -t, --test-type TYPE    Test type: all, unit, integration (default: all)"
    echo "  -j, --jobs N            Number of parallel jobs (default: $(nproc))"
    echo "  -v, --verbose           Verbose output"
    echo "  -c, --coverage          Generate coverage report"
    echo "  -h, --help              Show this help message"
    exit 1
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -t|--test-type)
            TEST_TYPE="$2"
            shift 2
            ;;
        -j|--jobs)
            PARALLEL_JOBS="$2"
            shift 2
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -c|--coverage)
            COVERAGE=true
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "Unknown option: $1"
            usage
            ;;
    esac
done

# Set default build directory if not provided
if [ -z "$BUILD_DIR" ]; then
    BUILD_DIR="../build"
fi

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory '$BUILD_DIR' does not exist${NC}"
    echo "Please build the project first using:"
    echo "  mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

# Change to build directory
cd "$BUILD_DIR"

echo -e "${GREEN}Running Lotus IFDS Tests${NC}"
echo "Build directory: $(pwd)"
echo "Test type: $TEST_TYPE"
echo "Parallel jobs: $PARALLEL_JOBS"
echo "Coverage: $COVERAGE"
echo ""

# Set up environment
export LOTUS_BUILD_DIR="$(pwd)"
export LOTUS_SRC_DIR="$(dirname $(pwd))"

# Prepare test data
echo -e "${YELLOW}Preparing test data...${NC}"
if [ -d "tests/test_data" ]; then
    echo "Test data directory already exists"
else
    echo "Creating test data directory"
    mkdir -p tests/test_data
fi

# Compile test data if needed
echo -e "${YELLOW}Compiling test data to LLVM IR...${NC}"
if command -v clang++ &> /dev/null; then
    # Compile C++ test files to LLVM IR
    for cpp_file in ../tests/test_data/taint/*.cpp; do
        if [ -f "$cpp_file" ]; then
            base_name=$(basename "$cpp_file" .cpp)
            output_file="tests/test_data/taint/${base_name}.ll"
            
            if [ ! -f "$output_file" ] || [ "$cpp_file" -nt "$output_file" ]; then
                echo "Compiling $cpp_file -> $output_file"
                clang++ -S -emit-llvm -g -O0 -Xclang -disable-O0-optnone \
                        -I../include "$cpp_file" -o "$output_file"
            fi
        fi
    done
else
    echo -e "${YELLOW}Warning: clang++ not found, skipping test data compilation${NC}"
fi

# Run tests based on type
case $TEST_TYPE in
    "unit")
        echo -e "${GREEN}Running unit tests...${NC}"
        ctest -R "Test$" --output-on-failure -j $PARALLEL_JOBS
        ;;
    "integration")
        echo -e "${GREEN}Running integration tests...${NC}"
        ctest -R "Integration" --output-on-failure -j $PARALLEL_JOBS
        ;;
    "all")
        echo -e "${GREEN}Running all tests...${NC}"
        ctest --output-on-failure -j $PARALLEL_JOBS
        ;;
    *)
        echo -e "${RED}Error: Unknown test type '$TEST_TYPE'${NC}"
        usage
        ;;
esac

# Generate coverage report if requested
if [ "$COVERAGE" = true ]; then
    echo -e "${YELLOW}Generating coverage report...${NC}"
    if command -v lcov &> /dev/null && command -v genhtml &> /dev/null; then
        lcov --directory . --capture --output-file coverage.info
        lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage.info.cleaned
        genhtml -o coverage coverage.info.cleaned
        echo -e "${GREEN}Coverage report generated in coverage/index.html${NC}"
    else
        echo -e "${YELLOW}Warning: lcov or genhtml not found, skipping coverage report${NC}"
    fi
fi

echo -e "${GREEN}Tests completed successfully!${NC}"
