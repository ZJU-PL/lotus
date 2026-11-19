#!/usr/bin/env python3
"""Compare Lotus/SVF fspta and vfspta on the same LLVM inputs.

Preserves commands, full logs, query coverage, errors and SHA256 fingerprints.
Only alias answers on common queried values are semantic comparisons; raw
points-to cardinalities are diagnostic because the object models differ.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command, directory, stem, timeout, working_directory):
    start = time.monotonic()
    try:
        proc = subprocess.run(command, cwd=working_directory, capture_output=True,
                              text=True, errors="replace", timeout=timeout)
        stdout, stderr, code = proc.stdout, proc.stderr, proc.returncode
    except subprocess.TimeoutExpired as error:
        stdout, stderr, code = error.stdout or b"", error.stderr or b"", "timeout"
        stdout = stdout.decode(errors="replace") if isinstance(stdout, bytes) else stdout
        stderr = stderr.decode(errors="replace") if isinstance(stderr, bytes) else stderr
    (directory / f"{stem}.stdout").write_text(stdout)
    (directory / f"{stem}.stderr").write_text(stderr)
    result = {"command": command, "cwd": str(working_directory), "returncode": code,
              "seconds": time.monotonic() - start}
    if code == 0:
        records = [line.removeprefix("PTA_COMPARISON_JSON ")
                   for line in stdout.splitlines()
                   if line.startswith("PTA_COMPARISON_JSON ")]
        if len(records) == 1:
            result["observations"] = json.loads(records[0])
    return result


def compare(left, right):
    a, b = left.get("observations"), right.get("observations")
    if a is None or b is None:
        return {"status": "error"}
    av = {v["key"]: v["points_to_size"] for v in a["values"]}
    bv = {v["key"]: v["points_to_size"] for v in b["values"]}
    common = av.keys() & bv.keys()
    aa = {tuple(sorted(pair)) for pair in a["may_alias"] if set(pair) <= common}
    ba = {tuple(sorted(pair)) for pair in b["may_alias"] if set(pair) <= common}
    return {"status": "insufficient" if len(common) < 2 else
                      "different" if aa != ba else "agree",
            "common_values": len(common),
            "compared_pairs": len(common) * (len(common) - 1) // 2,
            "only_left_values": sorted(av.keys() - bv.keys()),
            "only_right_values": sorted(bv.keys() - av.keys()),
            "truncated": [a["truncated"], b["truncated"]],
            "only_left_may_alias": sorted(aa - ba),
            "only_right_may_alias": sorted(ba - aa),
            "cardinality_differences": [
                {"key": key, "left": av[key], "right": bv[key]}
                for key in sorted(common) if av[key] != bv[key]]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="*", type=Path)
    parser.add_argument("--lotus", type=Path, default=Path("build/bin/lotus-alias-fspta"))
    parser.add_argument("--svf", type=Path, default=Path("build/svf-differential/svf-fspta-observer"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=30)
    parser.add_argument("--limit", type=int, default=512)
    parser.add_argument("--sample-seed", type=int, default=0)
    parser.add_argument("--generate", type=int, default=0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--llvm-bin", type=Path)
    parser.add_argument("--svf-arg", action="append", default=[])
    parser.add_argument("--working-directory", type=Path,
                        default=Path(__file__).resolve().parents[1],
                        help="Analysis cwd; Lotus loads config/*.spec relative to it")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    lotus, svf = args.lotus.resolve(), args.svf.resolve()
    working_directory = args.working_directory.resolve()
    inputs = []
    for path in args.inputs:
        inputs.extend(sorted(path.rglob("*.ll")) + sorted(path.rglob("*.bc"))
                      if path.is_dir() else [path])
    if args.generate:
        if not args.llvm_bin:
            parser.error("--generate requires --llvm-bin (LLVM 14)")
        spec = importlib.util.spec_from_file_location(
            "generate", Path(__file__).parent / "svf-differential/generate.py")
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        generated = output / "generated"
        generated.mkdir()
        for seed in range(args.seed, args.seed + args.generate):
            source = generated / f"random-{seed:05}.ll"
            source.write_text(generator.generate(seed))
            bitcode = source.with_suffix(".bc")
            subprocess.run([str(args.llvm_bin / "llvm-as"), str(source),
                            "-o", str(bitcode)], check=True)
            inputs.append(bitcode)
    inputs = list(dict.fromkeys(path.resolve() for path in inputs))
    if not inputs:
        parser.error("provide inputs or --generate")
    report = {"schema": 1, "arguments": vars(args),
              "binaries": {str(path): sha256(path) for path in (lotus, svf)},
              "cases": []}
    config_directory = Path(os.environ.get(
        "LOTUS_CONFIG_DIR", working_directory / "config")).resolve()
    report["lotus_config_directory"] = str(config_directory)
    report["lotus_models"] = {
        str(path): sha256(path) for name in ("ptr.spec", "modref.spec")
        if (path := config_directory / name).is_file()}
    if args.llvm_bin:
        report["llvm_as"] = {"path": str(args.llvm_bin / "llvm-as"),
                             "sha256": sha256(args.llvm_bin / "llvm-as")}
    # The observer's compiled-in external model is recorded when supplied.
    for argument in args.svf_arg:
        if argument.startswith("-extapi="):
            model = Path(argument.split("=", 1)[1])
            report["extapi"] = {"path": str(model), "sha256": sha256(model)}
    for index, path in enumerate(inputs):
        directory = output / f"{index:04}-{path.stem}"
        directory.mkdir()
        case = {"input": str(path), "sha256": sha256(path), "runs": {}}
        analyzed = path
        if args.llvm_bin:
            analyzed = directory / "input.bc"
            # Stable names survive SVF's constant-expression preprocessing;
            # numeric SSA slots can otherwise shift and misidentify queries.
            subprocess.run([str(args.llvm_bin / "opt"), "-enable-new-pm=0",
                            "-instnamer", str(path),
                            "-o", str(analyzed)], check=True)
        case["analyzed_input"] = str(analyzed)
        case["analyzed_sha256"] = sha256(analyzed)
        for mode in ("fspta", "vfspta"):
            common = [f"--comparison-limit={args.limit}",
                      f"--comparison-sample-seed={args.sample_seed}"]
            commands = {
                f"lotus-{mode}": [str(lotus), str(analyzed), f"--analysis={mode}",
                                  "--comparison-json", "--dump-stats=false", *common],
                f"svf-{mode}": [str(svf), str(analyzed), f"-{mode}",
                                "-model-consts=true", "-alias-check=false", "-stat=false",
                                *args.svf_arg, *[flag[1:] for flag in common]]}
            for name, command in commands.items():
                case["runs"][name] = run(command, directory, name, args.timeout,
                                         working_directory)
        case["comparisons"] = {}
        for left, right in [(f"lotus-{mode}", f"svf-{mode}") for mode in ("fspta", "vfspta")] + [
                (f"{tool}-fspta", f"{tool}-vfspta") for tool in ("lotus", "svf")]:
            case["comparisons"][f"{left}_vs_{right}"] = compare(
                case["runs"][left], case["runs"][right])
        report["cases"].append(case)
        states = ", ".join(f"{name}: {value['status']}"
                           for name, value in case["comparisons"].items())
        print(f"[{index + 1}/{len(inputs)}] {path.name}: {states}", flush=True)
        (output / "results.json").write_text(json.dumps(report, indent=2, default=str) + "\n")
    report["summary"] = {name: dict(Counter(case["comparisons"][name]["status"]
                                          for case in report["cases"]))
                         for name in report["cases"][0]["comparisons"]}
    (output / "results.json").write_text(json.dumps(report, indent=2, default=str) + "\n")
    print(json.dumps(report["summary"], indent=2))
    return int(any("error" in counts or "insufficient" in counts
                   for counts in report["summary"].values()))


if __name__ == "__main__":
    raise SystemExit(main())
