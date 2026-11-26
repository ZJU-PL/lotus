"""Configuration for the RQ2.2 adaptive phase-breakdown experiment."""

from pathlib import Path

from config import (
    BINARY_PATH, DOT_GLOB, EXTRA_ENVIRONMENT, MEASURED_REPETITIONS,
    MEMORY_LIMIT_GIB, REPOSITORY_ROOT, RESUME_EXISTING_CSV,
    SEARCH_RECURSIVELY, TESTSET_DIRECTORIES, TIMEOUT_SECONDS, WARMUP_REPETITIONS,
)
from rq1.config_rq1 import OUTPUT_CSV as ABLATION_CSV

ENGINE = "unary"
STOP_REPETITIONS_AFTER_FAILURE = False

COMMAND_ARGUMENTS = [
    ENGINE,
    "--algorithm",
    "adaptive",
    "--bidirect",
    "--stats",
    "--phase-timing",
]

# CSV column name -> exact key printed by the instrumented C++ binary.
PHASE_STAT_KEYS = [
    ("projection_us", "phase projection (us)"),
    ("quotient_sparsification_us", "phase quotient sparsification (us)"),
    ("decomposition_us", "phase decomposition (us)"),
    ("vertical_construction_us", "phase vertical construction (us)"),
    ("vertical_solving_us", "phase vertical solving (us)"),
    ("horizontal_construction_us", "phase horizontal construction (us)"),
    ("horizontal_solving_us", "phase horizontal solving (us)"),
    ("parent_map_labeling_us", "phase parent-map labeling (us)"),
    ("boundary_unions_us", "phase boundary unions (us)"),
    ("output_lifting_us", "phase output lifting (us)"),
]

DATA_DIRECTORY = Path(__file__).resolve().parent / "rq2-data"
OUTPUT_CSV = DATA_DIRECTORY / "rq2-breakdown.csv"
LOG_DIRECTORY = DATA_DIRECTORY / "log"
