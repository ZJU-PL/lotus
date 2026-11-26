"""RQ1 measurements, including the shared RQ2 sparsification ablation."""

from pathlib import Path

from config import (
    BINARY_PATH, DOT_GLOB, EXTRA_ENVIRONMENT, MEASURED_REPETITIONS,
    MEMORY_LIMIT_GIB, REPOSITORY_ROOT, RESUME_EXISTING_CSV,
    SEARCH_RECURSIVELY, TESTSET_DIRECTORIES, TIMEOUT_SECONDS, WARMUP_REPETITIONS,
)

ENGINE = "unary"
COMMON_ARGUMENTS = ["--bidirect", "--stats"]

# Preserve the original main experiment: RQ2 reuses adaptive/adaptive-direct.
EXPERIMENTS = [
    {"name": "fixed-counter", "algorithm": "fixed-counter", "args": []},
    {"name": "adaptive", "algorithm": "adaptive", "args": []},
    {"name": "adaptive-direct", "algorithm": "adaptive", "args": ["--direct"]},
]
STOP_REPETITIONS_AFTER_FAILURE = False

DATA_DIRECTORY = Path(__file__).resolve().parent / "rq1-data"
OUTPUT_CSV = DATA_DIRECTORY / "rq1-results.csv"
LOG_DIRECTORY = DATA_DIRECTORY / "log"
