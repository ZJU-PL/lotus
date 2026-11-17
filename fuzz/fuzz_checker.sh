#!/bin/bash
# Fuzz the engines exposed by the unified tools/checker frontend.
# Uses CSmith to generate random C, compiles to LLVM IR, then runs each checker.
# export CLANG="/path/to/your/clang"
CLANG="${CLANG:-clang}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
CSMITH="$BUILD_DIR/csmith-install/bin/csmith"
CSMITH_HOME="$BUILD_DIR/csmith-install/include"

if [[ ! -x "$CSMITH" ]]; then
    echo "csmith not found at $CSMITH"
    echo "Run $SCRIPT_DIR/build_csmisth.sh first."
    exit 1
fi

# Get SDK path only on macOS
SDK_PATH=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
fi

while true; do
    C_FILE="$SCRIPT_DIR/test_$$.c"
    BC_FILE="$SCRIPT_DIR/test_$$.bc"

    # Generate random C program
    echo "=== Generating C file: $C_FILE ==="
    CSMITH_CMD="$CSMITH --pointers --structs --unions --arrays --volatile-pointers --const-pointers --jumps --embedded-assigns"
    CSMITH_CMD="$CSMITH_CMD --max-pointer-depth $((RANDOM % 3 + 1))"
    CSMITH_CMD="$CSMITH_CMD --max-struct-fields $((RANDOM % 8 + 3))"
    CSMITH_CMD="$CSMITH_CMD --max-union-fields $((RANDOM % 5 + 2))"
    CSMITH_CMD="$CSMITH_CMD --max-expr-complexity $((RANDOM % 10 + 5))"
    CSMITH_CMD="$CSMITH_CMD --max-block-depth $((RANDOM % 3 + 2))"
    CSMITH_CMD="$CSMITH_CMD --max-block-size $((RANDOM % 3 + 2))"

    if ! timeout 10s bash -c "$CSMITH_CMD > \"$C_FILE\" 2>/dev/null"; then
        echo "✗ Failed to generate C file (timeout or error)"
        continue
    fi
    echo "✓ C file generated"

    # Compile to LLVM IR
    echo "=== Compiling to LLVM IR: $BC_FILE ==="
    CMD="$CLANG ${SDK_PATH:+-isysroot \"$SDK_PATH\"} -I\"$CSMITH_HOME\" -w -emit-llvm -c \"$C_FILE\" -o \"$BC_FILE\""
    echo "Command: $CMD"
    if ! eval "$CMD" 2>&1; then
        echo "Output: Compilation failed"
        rm -f "$C_FILE"
        continue
    fi
    echo "Output: Compilation successful"

    # KINT
    echo "=== Running lotus-check --engine=kint ==="
    if ! "$BUILD_DIR/bin/lotus-check" --engine=kint "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-check kint crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-check kint completed successfully"

    # lotus-taint (with default and a couple of aa types)
    for aa in dyck andersen; do
        echo "=== Running lotus-check --engine=taint-ifds --taint.alias-analysis=$aa ==="
        if ! "$BUILD_DIR/bin/lotus-check" --engine=taint-ifds --taint.alias-analysis="$aa" "$BC_FILE" 2>&1; then
            echo "CRASH: lotus-check taint (--taint.alias-analysis=$aa) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ lotus-check taint (--taint.alias-analysis=$aa) completed successfully"
    done

    # Concurrency (all checks enabled by default; also run analysis mode)
    echo "=== Running lotus-check --engine=concur ==="
    if ! "$BUILD_DIR/bin/lotus-check" --engine=concur "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-check concur crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-check concur completed successfully"
    echo "=== Running lotus-check --engine=concur --concur.mode=analysis ==="
    if ! "$BUILD_DIR/bin/lotus-check" --engine=concur --concur.mode=analysis "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-check concur (--concur.mode=analysis) crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-check concur (--concur.mode=analysis) completed successfully"

    # lotus-pulse (with and without SMT for coverage)
    for no_smt in false true; do
        smt_mode=on
        if [ "$no_smt" = true ]; then
            smt_mode=off
        fi
        echo "=== Running lotus-check --engine=pulse --pulse.smt=$smt_mode ==="
        if ! "$BUILD_DIR/bin/lotus-check" --engine=pulse --pulse.smt="$smt_mode" "$BC_FILE" 2>&1; then
            echo "CRASH: lotus-check pulse (--pulse.smt=$smt_mode) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ lotus-check pulse (--pulse.smt=$smt_mode) completed successfully"
    done

    # FiTx
    echo "=== Running lotus-check --engine=fitx ==="
    if ! "$BUILD_DIR/bin/lotus-check" --engine=fitx "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-check fitx crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-check fitx completed successfully"

    # Cleanup if no crash
    rm -f "$C_FILE" "$BC_FILE"
done
