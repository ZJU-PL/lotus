"""Configuration for the RQ3 upper-approximation comparison."""

from pathlib import Path

from config import (
    BENCHMARK_SUITES, BINARY_PATH, DOT_GLOB, EXTRA_ENVIRONMENT,
    MEASURED_REPETITIONS, MEMORY_LIMIT_GIB, REPOSITORY_ROOT,
    REQUIRE_BINARY_IDENTITY, RESUME_EXISTING_CSV, SEARCH_RECURSIVELY,
    TIMEOUT_SECONDS, WARMUP_REPETITIONS,
)
from rq1.config_rq1 import OUTPUT_CSV as RQ1_OUTPUT_CSV

INTERLEAVED_DYCK_BINARY = BINARY_PATH
BINARIES = {
    "staged": INTERLEAVED_DYCK_BINARY,
    "mcfl": INTERLEAVED_DYCK_BINARY,
    "unary": INTERLEAVED_DYCK_BINARY,
    "spds": INTERLEAVED_DYCK_BINARY,
}

# Arguments may use {analysis}, which is replaced from BENCHMARK_SUITES and
# then mapped through engine_analyses for the selected tool.
# artifact_kind controls the output from the separate relation-capture run.
# MCFL remains configured for future experiments but
# is disabled because it does not yet implement the value-flow endpoint model.
EXPERIMENTS = [
    {
        "name": "spds",
        "tool": "spds",
        "args": ["spds", "--analysis", "{analysis}", "--all-pairs",
                 "--bidirect"],
        "analyses": ["taint", "value-flow"],
        "engine_analyses": {
            "taint": "nonreflexive-taint",
            "value-flow": "value-flow",
        },
        "artifact_kind": "upper",
        "enabled": True,
    },
    {
        "name": "mutual-refinement",
        "tool": "staged",
        "args": ["staged-bounds", "--analysis", "{analysis}",
                 "--method", "mutual-refinement", "--bidirect",
                 "--plain-mr"],
        "artifact_kind": "upper",
        "enabled": True,
    },
    {
        "name": "acf",
        "tool": "unary",
        "args": ["unary", "--algorithm", "adaptive", "--bidirect", "--stats"],
        "artifact_kind": "components",
        "enabled": True,
    },
]

# Stop after the first timeout and move on to the next benchmark.
SKIP_BENCHMARK_AFTER_TIMEOUT = True
# Memory limits always stop repetitions. This flag covers other failures.
STOP_REPETITIONS_AFTER_FAILURE = True
CAPTURE_RELATIONS = True

DATA_DIRECTORY = Path(__file__).resolve().parent / "rq3-data"
OUTPUT_CSV = DATA_DIRECTORY / "rq3-runs.csv"
BOUNDS_CSV = DATA_DIRECTORY / "rq3-bounds.csv"
METHODS_CSV = DATA_DIRECTORY / "rq3-methods.csv"
LOG_DIRECTORY = DATA_DIRECTORY / "log"
ARTIFACT_DIRECTORY = DATA_DIRECTORY / "artifacts"


def supports_analysis(experiment, analysis):
    return analysis in experiment.get("analyses", ["taint", "value-flow"])
