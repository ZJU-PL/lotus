"""Configuration for the RQ3 interleaved-Dyck bounds experiment."""

from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]

INTERLEAVED_DYCK_BINARY = (
    REPOSITORY_ROOT / "build/bin/lotus-cfl-interleaved-dyck"
)
BINARIES = {
    "staged": INTERLEAVED_DYCK_BINARY,
    "mcfl": INTERLEAVED_DYCK_BINARY,
    "unary": INTERLEAVED_DYCK_BINARY,
}

# RQ3 uses the C/C++ value-flow corpus.  StagedBounds defines the client-aware
# endpoint universe; Union-Dyck supplies the lower bound and ACF tightens the
# final StagedBounds upper bound within that universe.
BENCHMARK_SUITES = [
    {
        "name": "taint",
        "directory": REPOSITORY_ROOT
        / "benchmarks/real-world/CFL/InterleavedDyck/taint",
        "analysis": "taint",
        "enabled": False,
    },
    {
        "name": "valueflow",
        "directory": REPOSITORY_ROOT
        / "benchmarks/real-world/CFL/InterleavedDyck/valueflow",
        "analysis": "value-flow",
        "enabled": True,
    },
]
DOT_GLOB = "*.dot"
SEARCH_RECURSIVELY = True

# Arguments may use {analysis}, which is replaced from BENCHMARK_SUITES.
# artifact_kind controls the machine-readable output captured on the first
# successful measured run.  MCFL remains configured for future experiments but
# is disabled because it does not yet implement the value-flow endpoint model.
EXPERIMENTS = [
    {
        "name": "union-dyck",
        "tool": "staged",
        "args": [
            "staged-bounds",
            "--analysis",
            "{analysis}",
            "--method",
            "underapproximation",
        ],
        "artifact_kind": "union",
        "enabled": True,
    },
    {
        "name": "staged-on-demand",
        "tool": "staged",
        "args": [
            "staged-bounds",
            "--analysis",
            "{analysis}",
            "--method",
            "all",
            "--parity-groups",
            "2",
        ],
        "artifact_kind": "staged",
        "enabled": True,
    },
    {
        "name": "acf",
        "tool": "unary",
        "args": ["unary", "--algorithm", "adaptive", "--bidirect", "--stats"],
        "artifact_kind": "components",
        "enabled": True,
    },
    {
        "name": "mcfl-plus-d1",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "1", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 1,
        "enabled": False,
    },
    {
        "name": "mcfl-plus-d2",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "2", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 2,
        "enabled": False,
    },
    {
        "name": "mcfl-plus-d3",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "3", "--stats"],
        "artifact_kind": "mcfl",
        "family": "plus",
        "dimension": 3,
        "required": False,
        "enabled": False,
    },
    {
        "name": "mcfl-circ-d1",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "1", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 1,
        "enabled": False,
    },
    {
        "name": "mcfl-circ-d2",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "2", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 2,
        "enabled": False,
    },
    {
        "name": "mcfl-circ-d3",
        "tool": "mcfl",
        "args": ["mcfl", "--dimension", "3", "--simple", "--stats"],
        "artifact_kind": "mcfl",
        "family": "circ",
        "dimension": 3,
        "required": False,
        "enabled": False,
    },
]

WARMUP_REPETITIONS = 0
MEASURED_REPETITIONS = 5
TIMEOUT_SECONDS = 3600
# RLIMIT_AS per process on Linux/macOS.  Set to None to disable it.
MEMORY_LIMIT_GIB = 128
STOP_REPETITIONS_AFTER_FAILURE = True
CAPTURE_RELATIONS = True

OUTPUT_CSV = REPOSITORY_ROOT / "scripts/unary/rq3/rq3-runs.csv"
BOUNDS_CSV = REPOSITORY_ROOT / "scripts/unary/rq3/rq3-bounds.csv"
METHODS_CSV = REPOSITORY_ROOT / "scripts/unary/rq3/rq3-methods.csv"
LOG_DIRECTORY = REPOSITORY_ROOT / "scripts/unary/rq3/log"
ARTIFACT_DIRECTORY = REPOSITORY_ROOT / "scripts/unary/rq3/artifacts"
RESUME_EXISTING_CSV = True

EXTRA_ENVIRONMENT = {}
