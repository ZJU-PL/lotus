"""Configuration for the RQ2.2 adaptive phase-breakdown experiment."""

import config as base_config


# Reuse the benchmark inputs and execution limits from the main experiment.
REPOSITORY_ROOT = base_config.REPOSITORY_ROOT
BINARY_PATH = base_config.BINARY_PATH
ENGINE = base_config.ENGINE
TESTSET_DIRECTORIES = base_config.TESTSET_DIRECTORIES
DOT_GLOB = base_config.DOT_GLOB
SEARCH_RECURSIVELY = base_config.SEARCH_RECURSIVELY
WARMUP_REPETITIONS = base_config.WARMUP_REPETITIONS
MEASURED_REPETITIONS = base_config.MEASURED_REPETITIONS
TIMEOUT_SECONDS = base_config.TIMEOUT_SECONDS
MEMORY_LIMIT_GIB = base_config.MEMORY_LIMIT_GIB
STOP_REPETITIONS_AFTER_FAILURE = base_config.STOP_REPETITIONS_AFTER_FAILURE

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

OUTPUT_CSV = REPOSITORY_ROOT / "scripts/unary/rq2-breakdown.csv"
LOG_DIRECTORY = REPOSITORY_ROOT / "scripts/unary/log-rq2-breakdown"
RESUME_EXISTING_CSV = True
