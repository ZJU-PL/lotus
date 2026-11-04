#!/usr/bin/env python3
"""Run an external classical-CFL graph dataset through lotus-cfl-solve.

The script deliberately keeps dataset policy outside the solver binary. It
emits one JSON object per run and never invokes the LLVM alias/value-flow
frontends; those remain separate end-to-end bitcode entry points.
"""

import argparse
import concurrent.futures
import ctypes
import json
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple


DEFAULT_CASES = (
    "cactus",
    "deepsjeng",
    "imagick",
    "lbm",
    "leela",
    "mcf",
    "nab",
    "omnetpp",
    "parest",
    "perlbench",
    "povray",
    "x264",
    "xz",
)

ANALYSES = {
    "alias": ("pegs", "aa"),
    "value-flow": ("svfgs", "vf"),
    "taint": ("svfgs", "taint"),
}

DEFAULT_CONFIGURATIONS = (
    "pocr:cfg:count",
    "skewed:ecfg:count",
)

GENERAL_SOLVERS = (
    "sparse-set",
    "sparse-bitvector",
    "graspan",
    "sqid",
    "pearl",
    "skewed",
    "cat",
    "iea",
    "iea-ocr",
    "transitive-closure",
    "pocr",
    "hpocr",
    "focr",
    "endpoint-quotient",
    "cert",
)

ACTIVE_PROCESSES = set()
ACTIVE_PROCESSES_LOCK = threading.Lock()
STOP_REQUESTED = threading.Event()


def terminate_process(process: subprocess.Popen, grace_seconds: float = 1.0) -> None:
    if process.poll() is not None:
        return

    def send(sig) -> None:
        if os.name == "posix":
            try:
                os.killpg(process.pid, sig)
                return
            except ProcessLookupError:
                return
            except (PermissionError, OSError):
                # Some macOS process-group states reject killpg even though
                # the direct child remains ours. Fall back to the child PID.
                pass
        try:
            process.send_signal(sig)
        except (ProcessLookupError, PermissionError, OSError):
            pass

    send(signal.SIGTERM)
    try:
        process.wait(timeout=grace_seconds)
        return
    except (ProcessLookupError, subprocess.TimeoutExpired, PermissionError):
        pass
    if process.poll() is not None:
        return
    send(signal.SIGKILL)
    try:
        process.wait(timeout=grace_seconds)
    except (ProcessLookupError, subprocess.TimeoutExpired, PermissionError):
        # Last-resort direct kill; never let cleanup failure escape a worker.
        try:
            process.kill()
            process.wait(timeout=grace_seconds)
        except (ProcessLookupError, subprocess.TimeoutExpired, PermissionError):
            pass


def terminate_all_processes() -> None:
    with ACTIVE_PROCESSES_LOCK:
        processes = list(ACTIVE_PROCESSES)
    for process in processes:
        terminate_process(process)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--data",
        type=Path,
        default=Path("benchmarks/real-world/CFL/Classical"),
    )
    parser.add_argument(
        "--binary", type=Path, default=Path("build/bin/lotus-cfl-solve")
    )
    parser.add_argument(
        "--case",
        action="append",
        dest="cases",
        help="Repeat or use commas; defaults to all 13 benchmarks",
    )
    parser.add_argument(
        "--analysis",
        action="append",
        dest="analyses",
        help="Repeat or use commas: alias,value-flow,taint; defaults to all",
    )
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument(
        "--configuration",
        action="append",
        dest="configurations",
        metavar="SOLVER:GRAMMAR_SUFFIX:RESULT_SCOPE",
        help="Repeatable, for example pocr:cfg:count or skewed:ecfg:count",
    )
    selection.add_argument(
        "--solver",
        action="append",
        dest="solvers",
        help="Repeat or use commas; uses --grammar-suffix and --result-scope",
    )
    selection.add_argument(
        "--all-solvers",
        action="store_true",
        help="Run every generic SolverSession backend on the original grammar",
    )
    parser.add_argument("--grammar-suffix", default="cfg")
    parser.add_argument(
        "--result-scope", choices=("all", "start", "count"), default="count"
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--resume",
        action="store_true",
        help="Append to --output and skip task keys already recorded there",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="Per-run timeout; zero disables it",
    )
    parser.add_argument(
        "--workers", type=int, default=1, help="Concurrent solver processes"
    )
    parser.add_argument(
        "--memory-limit-mb",
        type=int,
        default=0,
        metavar="MB",
        help="Per-run resident-memory limit; zero disables it",
    )
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--max-attribute-expansions", type=int, default=1_000_000
    )
    args = parser.parse_args()
    if args.workers < 1:
        parser.error("--workers must be positive")
    if args.timeout < 0:
        parser.error("--timeout cannot be negative")
    if args.memory_limit_mb < 0:
        parser.error("--memory-limit-mb cannot be negative")
    if args.memory_limit_mb > 0 and not (
        sys.platform.startswith("linux") or sys.platform == "darwin"
    ):
        parser.error("--memory-limit-mb currently supports Linux and macOS")
    if args.resume and not args.output:
        parser.error("--resume requires --output")
    return args


def expand_values(values: Optional[List[str]]) -> List[str]:
    expanded = []
    for value in values or []:
        expanded.extend(item.strip() for item in value.split(",") if item.strip())
    return list(dict.fromkeys(expanded))


def parse_configuration(specification: str) -> Tuple[str, str, str]:
    fields = specification.split(":")
    if len(fields) != 3 or fields[2] not in ("all", "start", "count"):
        raise ValueError(
            "configuration must be SOLVER:GRAMMAR_SUFFIX:all|start|count"
        )
    if fields[0] == "stg":
        raise ValueError(
            "STG requires --stg-spec and must be invoked directly with "
            "lotus-cfl-solve"
        )
    return fields[0], fields[1], fields[2]


def paths_for(
    data: Path, analysis: str, case: str, grammar_suffix: str
) -> Tuple[Path, Path]:
    graph_dir, grammar_stem = ANALYSES[analysis]
    graph = data / "spec2017" / graph_dir / f"{case}.g"
    return graph, data / "grammars" / f"{grammar_stem}.{grammar_suffix}"


def command_for(
    binary: Path,
    graph: Path,
    grammar: Path,
    solver: str,
    result_scope: str,
    maximum_expansions: int,
    validate_only: bool,
) -> List[str]:
    command = [
        str(binary),
        "--grammar",
        str(grammar),
        "--graph",
        str(graph),
        "--graph-mode",
        "plain",
        "--max-attribute-expansions",
        str(maximum_expansions),
    ]
    if validate_only:
        return command + ["--validate-only"]
    command += ["--solver", solver]
    if result_scope != "all":
        command += ["--result-scope", result_scope]
    return command + ["--json-stats"]


def output_text(value) -> str:
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def linux_resident_bytes(pid: int) -> Optional[int]:
    try:
        fields = Path(f"/proc/{pid}/statm").read_text().split()
        return int(fields[1]) * os.sysconf("SC_PAGE_SIZE")
    except (FileNotFoundError, IndexError, OSError, ValueError):
        return None


class DarwinProcTaskInfo(ctypes.Structure):
    _fields_ = [
        ("virtual_size", ctypes.c_uint64),
        ("resident_size", ctypes.c_uint64),
        ("total_user", ctypes.c_uint64),
        ("total_system", ctypes.c_uint64),
        ("threads_user", ctypes.c_uint64),
        ("threads_system", ctypes.c_uint64),
    ] + [(f"field_{index}", ctypes.c_int32) for index in range(12)]


DARWIN_LIBPROC = None


def darwin_resident_bytes(pid: int) -> Optional[int]:
    global DARWIN_LIBPROC
    try:
        if DARWIN_LIBPROC is None:
            DARWIN_LIBPROC = ctypes.CDLL("/usr/lib/libproc.dylib")
            DARWIN_LIBPROC.proc_pidinfo.argtypes = [
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_uint64,
                ctypes.c_void_p,
                ctypes.c_int,
            ]
            DARWIN_LIBPROC.proc_pidinfo.restype = ctypes.c_int
        info = DarwinProcTaskInfo()
        size = ctypes.sizeof(info)
        read = DARWIN_LIBPROC.proc_pidinfo(
            pid, 4, 0, ctypes.byref(info), size  # PROC_PIDTASKINFO
        )
        return info.resident_size if read == size else None
    except (AttributeError, OSError):
        return None


def resident_bytes(pid: int) -> Optional[int]:
    if sys.platform.startswith("linux"):
        return linux_resident_bytes(pid)
    if sys.platform == "darwin":
        return darwin_resident_bytes(pid)
    return None


def run_task(
    task: Dict, args: argparse.Namespace, binary: Path
) -> Dict:
    graph = task["graph"]
    grammar = task["grammar"]
    record = {
        "analysis": task["analysis"],
        "benchmark": task["benchmark"],
        "solver": task["solver"],
        "grammar_variant": task["grammar_suffix"],
        "requested_result_scope": task["result_scope"],
        "graph": str(graph),
        "grammar": str(grammar),
    }
    if STOP_REQUESTED.is_set():
        record["status"] = "cancelled"
        return record
    missing = [
        str(path) for path in (binary, graph, grammar) if not path.is_file()
    ]
    if missing:
        record.update({"status": "missing-input", "missing": missing})
        return record

    command = command_for(
        binary,
        graph,
        grammar,
        task["solver"],
        task["result_scope"],
        args.max_attribute_expansions,
        args.validate_only,
    )
    record["command"] = command
    record["memory_limit_mb"] = args.memory_limit_mb
    if args.dry_run:
        record["status"] = "dry-run"
        return record

    start = time.monotonic()
    process = None
    stdout = ""
    stderr = ""
    peak_rss_bytes = 0
    memory_observed = False
    termination_status = None
    try:
        process = subprocess.Popen(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=os.name == "posix",
        )
        with ACTIVE_PROCESSES_LOCK:
            ACTIVE_PROCESSES.add(process)
        memory_limit_bytes = args.memory_limit_mb * 1024 * 1024
        while process.poll() is None:
            rss = resident_bytes(process.pid)
            if rss is not None:
                memory_observed = True
                peak_rss_bytes = max(peak_rss_bytes, rss)
                if memory_limit_bytes > 0 and rss > memory_limit_bytes:
                    termination_status = "memory-limit"
                    break
            if STOP_REQUESTED.is_set():
                termination_status = "interrupted"
                break
            if args.timeout > 0 and time.monotonic() - start >= args.timeout:
                termination_status = "timeout"
                break
            time.sleep(0.05)

        if termination_status:
            terminate_process(process)
        try:
            stdout, stderr = process.communicate(timeout=1.0)
        except subprocess.TimeoutExpired:
            terminate_process(process, grace_seconds=0.2)
            stdout, stderr = process.communicate()
    except OSError as error:
        record.update(
            {
                "status": "launch-failed",
                "exit_code": None,
                "error": str(error),
                "wall_seconds": time.monotonic() - start,
            }
        )
        return record
    finally:
        if process is not None:
            with ACTIVE_PROCESSES_LOCK:
                ACTIVE_PROCESSES.discard(process)

    record["peak_rss_bytes"] = peak_rss_bytes
    record["peak_rss_mb"] = peak_rss_bytes / (1024 * 1024)
    record["memory_monitor_available"] = memory_observed
    if termination_status:
        record.update(
            {
                "status": termination_status,
                "exit_code": process.returncode,
                "wall_seconds": time.monotonic() - start,
            }
        )
        if termination_status == "timeout":
            record["timeout_seconds"] = args.timeout
        if termination_status == "memory-limit":
            record["memory_limit_bytes"] = args.memory_limit_mb * 1024 * 1024
        if stdout:
            record["stdout"] = output_text(stdout).strip()
        if stderr:
            record["stderr"] = output_text(stderr).strip()
        return record

    record.update(
        {
            "exit_code": process.returncode,
            "status": (
                "interrupted"
                if STOP_REQUESTED.is_set()
                else ("ok" if process.returncode == 0 else "failed")
            ),
            "wall_seconds": time.monotonic() - start,
        }
    )
    if process.returncode == 0 and not args.validate_only:
        try:
            record.update(json.loads(stdout))
        except json.JSONDecodeError as error:
            record["status"] = "invalid-json"
            record["parse_error"] = str(error)
            record["stdout"] = stdout
    else:
        record["stdout"] = stdout.strip()
    if stderr:
        record["stderr"] = stderr.strip()
    return record


def main() -> int:
    args = parse_args()
    data = args.data.expanduser().resolve()
    binary = args.binary.expanduser().resolve()
    cases = expand_values(args.cases) or list(DEFAULT_CASES)
    analyses = expand_values(args.analyses) or list(ANALYSES)
    unknown_analyses = sorted(set(analyses) - set(ANALYSES))
    unknown_cases = sorted(set(cases) - set(DEFAULT_CASES))
    if unknown_analyses:
        raise SystemExit("unknown analyses: " + ", ".join(unknown_analyses))
    if unknown_cases:
        raise SystemExit("unknown benchmarks: " + ", ".join(unknown_cases))

    requested = args.configurations
    if args.all_solvers:
        requested = [f"{solver}:cfg:count" for solver in GENERAL_SOLVERS]
    elif args.solvers:
        solvers = expand_values(args.solvers)
        unknown_solvers = sorted(set(solvers) - set(GENERAL_SOLVERS))
        if unknown_solvers:
            raise SystemExit("unknown solvers: " + ", ".join(unknown_solvers))
        requested = [
            f"{solver}:{args.grammar_suffix}:{args.result_scope}"
            for solver in solvers
        ]
    configurations = [
        parse_configuration(specification)
        for specification in (requested or DEFAULT_CONFIGURATIONS)
    ]
    completed_keys = set()
    if args.resume and args.output.exists():
        with args.output.open(encoding="utf-8") as previous:
            for line_number, line in enumerate(previous, 1):
                try:
                    record = json.loads(line)
                except json.JSONDecodeError as error:
                    raise SystemExit(
                        f"invalid JSONL at {args.output}:{line_number}: {error}"
                    )
                completed_keys.add(
                    (
                        record.get("analysis"),
                        record.get("benchmark"),
                        record.get("solver"),
                        record.get("grammar_variant"),
                        record.get("requested_result_scope"),
                    )
                )
    destination = (
        args.output.open("a" if args.resume else "w", encoding="utf-8")
        if args.output
        else sys.stdout
    )
    tasks = []
    for analysis in analyses:
        for case in cases:
            for solver, grammar_suffix, result_scope in configurations:
                graph, grammar = paths_for(data, analysis, case, grammar_suffix)
                tasks.append(
                    {
                        "analysis": analysis,
                        "benchmark": case,
                        "solver": solver,
                        "grammar_suffix": grammar_suffix,
                        "result_scope": result_scope,
                        "graph": graph,
                        "grammar": grammar,
                    }
                )
    if completed_keys:
        tasks = [
            task
            for task in tasks
            if (
                task["analysis"],
                task["benchmark"],
                task["solver"],
                task["grammar_suffix"],
                task["result_scope"],
            )
            not in completed_keys
        ]

    failed = False

    executor = concurrent.futures.ThreadPoolExecutor(max_workers=args.workers)
    futures = []
    future_tasks = {}
    interrupted = False
    try:
        futures = [executor.submit(run_task, task, args, binary) for task in tasks]
        future_tasks = dict(zip(futures, tasks))
        for future in concurrent.futures.as_completed(futures):
            try:
                record = future.result()
            except Exception as error:
                task = future_tasks[future]
                record = {
                    "analysis": task["analysis"],
                    "benchmark": task["benchmark"],
                    "solver": task["solver"],
                    "grammar_variant": task["grammar_suffix"],
                    "requested_result_scope": task["result_scope"],
                    "status": "driver-error",
                    "error": repr(error),
                }
            print(
                json.dumps(record, sort_keys=True),
                file=destination,
                flush=True,
            )
            failed = failed or record["status"] not in ("ok", "dry-run")
            if failed and args.fail_fast:
                STOP_REQUESTED.set()
                terminate_all_processes()
                for pending in futures:
                    pending.cancel()
                break
    except KeyboardInterrupt:
        interrupted = True
        STOP_REQUESTED.set()
        terminate_all_processes()
        for future in futures:
            future.cancel()
        print("interrupted: terminated active solver processes", file=sys.stderr)
    finally:
        executor.shutdown(wait=True)
        if destination is not sys.stdout:
            destination.close()
    if interrupted:
        return 130
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
