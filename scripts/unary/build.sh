#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
build_dir="$script_dir/build"
# --fresh requires CMake >= 3.24 and discards the previous compiler configuration.
cmake --fresh -S "$repo_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

# RQ1: KP (fixed-counter), ACF (adaptive).
# RQ2: ACF --direct ablation and --phase-timing breakdown.
# RQ3: ACF, Mutual Refinement, Regularization, SPDS.
# All algorithms share one executable and the project's CMake configuration.
cmake --build "$build_dir" --config Release --clean-first \
  --target lotus-cfl-interleaved-dyck --parallel "${JOBS:-4}"
