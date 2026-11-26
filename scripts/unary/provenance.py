"""Content identity and integrity checks shared by the experiment runners."""

import csv
import hashlib
import json
import math
import shlex
from pathlib import Path

from config import REPOSITORY_ROOT, REQUIRE_BINARY_IDENTITY

COLUMNS = ["warmup_repetitions", "input_sha256", "binary_sha256",
           "extra_environment", "timeout_seconds", "memory_limit_bytes",
           "stop_after_failure", "log_sha256", "row_sha256"]


def sha256(path):
  digest = hashlib.sha256()
  with Path(path).open("rb") as stream:
    for block in iter(lambda: stream.read(1024 * 1024), b""):
      digest.update(block)
  return digest.hexdigest()


def json_cell(value):
  return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def stored_path(value):
  path = Path(value)
  return path if path.is_absolute() else REPOSITORY_ROOT / path


def identity(dot, binary, command, settings, fingerprints=None):
  hashes = fingerprints if fingerprints is not None else {
      Path(dot): sha256(dot), Path(binary): sha256(binary)}
  return {
      "input_sha256": hashes[Path(dot)],
      "binary_sha256": hashes[Path(binary)],
      "command": shlex.join(command),
      "extra_environment": json_cell({str(k): str(v) for k, v in settings.EXTRA_ENVIRONMENT.items()}),
      "configured_repetitions": str(settings.MEASURED_REPETITIONS),
      "warmup_repetitions": str(settings.WARMUP_REPETITIONS),
      "timeout_seconds": "" if settings.TIMEOUT_SECONDS is None else str(settings.TIMEOUT_SECONDS),
      "memory_limit_bytes": "" if settings.MEMORY_LIMIT_GIB is None else str(int(settings.MEMORY_LIMIT_GIB * 1024**3)),
      "stop_after_failure": str(settings.STOP_REPETITIONS_AFTER_FAILURE),
  }


def row_digest(row):
  # Normalize exactly as CSV does, including None -> empty cell.
  cells = {key: "" if value is None else str(value)
           for key, value in row.items() if key != "row_sha256"}
  return hashlib.sha256(json_cell(cells).encode()).hexdigest()


def seal(row):
  row["log_sha256"] = json_cell({name: sha256(stored_path(name))
                                for name in json.loads(row["log_files"])})
  row["row_sha256"] = row_digest(row)
  return row


def validate_logs(row):
  names = json.loads(row["log_files"])
  hashes = json.loads(row["log_sha256"])
  if len(names) != int(row["completed_repetitions"]) or len(set(names)) != len(names) or set(names) != set(hashes):
    raise ValueError("log list/hash count mismatch")
  for name in names:
    if sha256(stored_path(name)) != hashes[name]:
      raise ValueError(f"log SHA256 mismatch: {name}")


def validate(row, expected=None, require_success=False):
  """Reject damaged or incompatible rows; do not repair historical records."""
  try:
    if row.get("row_sha256") != row_digest(row):
      raise ValueError("row SHA256 mismatch (or missing provenance)")
    if expected:
      changed = [
          key for key, value in expected.items()
          if str(row.get(key)) != str(value)
          and not (key == "binary_sha256" and not REQUIRE_BINARY_IDENTITY)
      ]
      if changed:
        raise ValueError("run conditions changed: " + ", ".join(changed))
    validate_logs(row)
    n = int(row["completed_repetitions"])
    configured = int(row["configured_repetitions"])
    arrays = {key: json.loads(row[key]) for key in (
        "statuses", "exit_codes", "time_ms", "peak_rss_bytes", "wall_time_seconds", "stats_per_run")}
    if not 0 < n <= configured or any(len(values) != n for values in arrays.values()):
      raise ValueError("incomplete sample arrays")
    successes = arrays["statuses"].count("success")
    if successes != int(row["successful_repetitions"]):
      raise ValueError("success count mismatch")
    if n < configured and not (arrays["statuses"][-1] == "memory-limit" or
        (row["stop_after_failure"] == "True" and arrays["statuses"][-1] != "success")):
      raise ValueError("unfinished measurement repetitions")
    if require_success and (n != configured or successes != configured):
      raise ValueError("RQ1 reuse requires all repetitions to succeed")
    for i, status in enumerate(arrays["statuses"]):
      wall = arrays["wall_time_seconds"][i]
      if not isinstance(wall, (int, float)) or not math.isfinite(wall) or wall < 0:
        raise ValueError("invalid wall time")
      if status != "success":
        continue
      if arrays["exit_codes"][i] != 0:
        raise ValueError("successful run has nonzero exit code")
      for field, stat, minimum in (("time_ms", "Time (ms)", 0),
                                   ("peak_rss_bytes", "process peak RSS (bytes)", 1)):
        value = arrays[field][i]
        if not isinstance(value, int) or value < minimum or value != int(arrays["stats_per_run"][i][stat]):
          raise ValueError(f"invalid {field} sample")
      if "missing_phase_stats" in row and json.loads(row["missing_phase_stats"])[i]:
        raise ValueError("successful RQ2 run is missing phase statistics")
  except (KeyError, TypeError, ValueError, OSError) as error:
    raise RuntimeError(f"invalid experiment record {row.get('dot_path')}: {error}; "
                       "use a fresh DATA_DIRECTORY or --restart") from error


def check_files(dot, binary, expected):
  if sha256(dot) != expected["input_sha256"]:
    raise RuntimeError("input changed during this invocation; use a fresh DATA_DIRECTORY")
  if REQUIRE_BINARY_IDENTITY and sha256(binary) != expected["binary_sha256"]:
    raise RuntimeError("binary changed during this invocation; use a fresh DATA_DIRECTORY")


def resume_rows(path, columns, key_fields, expected):
  with path.open(encoding="utf-8", newline="") as stream:
    reader = csv.DictReader(stream)
    if reader.fieldnames != columns:
      raise RuntimeError(f"CSV header mismatch: {path}; use a fresh DATA_DIRECTORY or --restart")
    rows = list(reader)
  completed = set()
  versions = {(item["binary_sha256"], item["extra_environment"]) for item in expected.values()}
  environments = {item["extra_environment"] for item in expected.values()}
  for row in rows:
    key = tuple(row[field] for field in key_fields)
    if key in completed:
      raise RuntimeError(f"duplicate CSV row: {key}")
    validate(row, expected.get(key))
    env_matches = row["extra_environment"] in environments
    binary_matches = (row["binary_sha256"], row["extra_environment"]) in versions
    if not env_matches or (REQUIRE_BINARY_IDENTITY and not binary_matches):
      raise RuntimeError("CSV mixes binaries/environments; use a fresh DATA_DIRECTORY or --restart")
    if sha256(stored_path(row["dot_path"])) != row["input_sha256"]:
      raise RuntimeError(f"input SHA256 mismatch: {row['dot_path']}")
    completed.add(key)
  return completed
