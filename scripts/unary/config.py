"""Shared paths and execution settings for RQ1, RQ2, and RQ3."""

from pathlib import Path


EXPERIMENT_ROOT = Path(__file__).resolve().parent
REPOSITORY_ROOT = EXPERIMENT_ROOT.parents[1]
BINARY_PATH = EXPERIMENT_ROOT / "build/bin/lotus-cfl-interleaved-dyck"

# Every .dot file below each directory is treated as one benchmark project.
BENCHMARK_SUITES = [
    {
        "name": "taint",
        "directory": REPOSITORY_ROOT / "benchmarks/real-world/CFL/InterleavedDyck/taint",
        "analysis": "taint",
        "enabled": True,
    },
    {
        "name": "valueflow",
        "directory": REPOSITORY_ROOT / "benchmarks/real-world/CFL/InterleavedDyck/valueflow",
        "analysis": "value-flow",
        "enabled": False,
    },
]
TESTSET_DIRECTORIES = [
    suite["directory"] for suite in BENCHMARK_SUITES if suite["enabled"]
]
DOT_GLOB = "*.dot"
SEARCH_RECURSIVELY = True

# Repetition policy.  Warm-up runs are logged but are not included in the CSV
# measurement arrays or averages.
WARMUP_REPETITIONS = 0
MEASURED_REPETITIONS = 5
TIMEOUT_SECONDS = 3600
# Per-process virtual-address-space limit enforced with RLIMIT_AS on Linux.
# Use None to disable the limit.
MEMORY_LIMIT_GIB = 128
# Each runner retains its existing CSV resume/completeness policy.
RESUME_EXISTING_CSV = True

# Extra environment variables for the benchmark process, e.g. thread controls.
# Existing environment variables are retained.
EXTRA_ENVIRONMENT = {}

# When True, resumed/reused CSV rows must match the current executable's
# SHA-256 before they are trusted. Set to False to allow resuming or reusing
# rows after the binary has been rebuilt with semantically equivalent changes.
# The binary hash is still recorded in every row for provenance; only the
# identity comparison is relaxed. Input files, commands, and the execution
# environment remain strictly checked.
REQUIRE_BINARY_IDENTITY = False
