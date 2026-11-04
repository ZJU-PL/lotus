"""Configuration for the unary interleaved-Dyck benchmark runner."""

from pathlib import Path


# Repository paths.  All other default paths are derived from REPOSITORY_ROOT.
REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
BINARY_PATH = REPOSITORY_ROOT / "build/bin/lotus-cfl-interleaved-dyck"
ENGINE = "unary"

# Every .dot file below each directory is treated as one benchmark project.
TESTSET_DIRECTORIES = [
    REPOSITORY_ROOT / "benchmarks/real-world/CFL/InterleavedDyck/taint",
    REPOSITORY_ROOT / "benchmarks/real-world/CFL/InterleavedDyck/valueflow",
]
DOT_GLOB = "*.dot"
SEARCH_RECURSIVELY = True

# One CSV row is emitted for every (DOT file, experiment configuration) pair.
# Add the following entry when the quotient-sparsification ablation is needed:
# {"name": "adaptive-direct", "algorithm": "adaptive", "args": ["--direct"]}
EXPERIMENTS = [
    {"name": "fixed-counter", "algorithm": "fixed-counter", "args": []},
    {"name": "adaptive", "algorithm": "adaptive", "args": []},
    {"name": "adaptive-direct", "algorithm": "adaptive", "args": ["--direct"]},
]

# These arguments are shared by every experiment above.  The supplied corpus
# is directed, so --bidirect is required for the unary solvers.
COMMON_ARGUMENTS = ["--bidirect", "--stats"]

# Repetition policy.  Warm-up runs are logged but are not included in the CSV
# measurement arrays or averages.
WARMUP_REPETITIONS = 0
MEASURED_REPETITIONS = 5
TIMEOUT_SECONDS = 3600
# Per-process virtual-address-space limit enforced with RLIMIT_AS on Linux.
# Use None to disable the limit.
MEMORY_LIMIT_GIB = 128
STOP_REPETITIONS_AFTER_FAILURE = False

# Output policy.
OUTPUT_CSV = REPOSITORY_ROOT / "scripts/unary/unary-results.csv"
LOG_DIRECTORY = REPOSITORY_ROOT / "scripts/unary/log"

# When true, rows already present in OUTPUT_CSV are skipped.  A row is complete
# only after all of its measured repetitions have been run and flushed to disk.
RESUME_EXISTING_CSV = True

# Extra environment variables for the benchmark process, e.g. thread controls.
# Existing environment variables are retained.
EXTRA_ENVIRONMENT = {}
