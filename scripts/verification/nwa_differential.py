#!/usr/bin/env python3
"""Compare Lotus NWA operations with an unmodified local Ultimate checkout.

No network access or changes to the checkout are needed. Parser generators ship
with Ultimate; the Eclipse compiler and support jars can come from a local Maven
repository or explicit arguments. Every run retains its corpus, outputs, build
logs, and source/binary fingerprints in a fresh directory.
"""

import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import re
import subprocess
import tempfile
import time


def fingerprint(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def version(path):
    return tuple(int(x) for x in re.findall(r"\d+", path.parent.name))


def latest(pattern, description):
    choices = [p for p in pattern if not p.name.endswith(("-sources.jar", "-javadoc.jar", "-tests.jar"))]
    if not choices:
        raise RuntimeError(f"Missing {description}; supply the corresponding jar explicitly")
    return max(choices, key=version)


def execute(command, work, name, timeout, stdin=None, cwd=None):
    with (work / f"{name}.stdout").open("wb") as stdout, (work / f"{name}.stderr").open("wb") as stderr:
        try:
            result = subprocess.run(
                [str(x) for x in command], input=stdin, stdout=stdout, stderr=stderr,
                cwd=cwd, timeout=timeout, check=False,
            )
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"{name} exceeded {timeout}s; logs in {work}") from error
    if result.returncode:
        details = (work / f"{name}.stderr").read_text(errors="replace")[-6000:]
        raise RuntimeError(f"{name} exited {result.returncode}; logs in {work}\n{details}")
    return (work / f"{name}.stdout").read_bytes()


def build_oracle(args, root, work):
    source = root / "trunk/source"
    if not (source / "Library-Automata/src").is_dir():
        raise RuntimeError(f"Not an Ultimate source checkout: {root}")
    generated = work / "generated"
    generated.mkdir()
    for library in ("SMTInterpol", "SMTSolverBridge"):
        for grammar in sorted((source / library / "src").rglob("*.cup")):
            destination = generated / grammar.parent.relative_to(source / library / "src")
            destination.mkdir(parents=True, exist_ok=True)
            name = library + "-" + grammar.parent.name
            execute([args.java, "-cp", source / "JavaCup/JFlex.jar", "JFlex.Main", "-d",
                     destination, grammar.with_suffix(".flex")], work, name + "-lexer",
                    args.timeout, cwd=destination)
            execute([args.java, "-cp", source / "JavaCup/jh-javacup-1.2.jar",
                     "com.github.jhoenicke.javacup.Main", grammar], work,
                    name + "-parser", args.timeout, cwd=destination)
    maven = args.maven_repository.expanduser()
    ecj = args.ecj or latest((maven / "org/eclipse/jdt/ecj").glob("*/*.jar"), "Eclipse compiler (--ecj)")
    jars = sorted(source.rglob("*.jar")) + list(args.dependency_jar)
    if not args.dependency_jar:
        platform = maven / "org/eclipse/platform"
        if platform.is_dir():
            for artifact in sorted(platform.iterdir()):
                choices = list(artifact.glob("*/*.jar"))
                if choices:
                    jars.append(max(choices, key=version))
        jars.append(latest((maven / "commons-io/commons-io").glob("*/*.jar"),
                           "Commons IO (--dependency-jar)"))
    paths = [generated] + sorted(p for p in source.glob("*/src") if p.is_dir())
    classes = work / "classes"
    classes.mkdir()
    adapter = Path(__file__).resolve().parent / "nwa_reference/UltimateNwaOracle.java"
    execute([args.java, "-jar", ecj, "-21", "-proc:none", "-nowarn", "-d", classes,
             "-sourcepath", os.pathsep.join(map(str, paths)), "-classpath",
             os.pathsep.join(map(str, jars)), adapter], work, "java-compile", args.timeout)
    digest = hashlib.sha256()
    files = sorted((source / "Library-Automata/src").rglob("*.java"))
    for file in files:
        digest.update(str(file.relative_to(source)).encode())
        digest.update(file.read_bytes())
    metadata = {
        "ultimate_checkout": str(root), "automata_source_files": len(files),
        "automata_source_sha256": digest.hexdigest(), "ecj": str(ecj),
        "ecj_sha256": fingerprint(ecj), "java_adapter_sha256": fingerprint(adapter),
        "dependency_jars": [{"path": str(j), "sha256": fingerprint(j)} for j in jars],
        "source_changes": [], "semantic_stubs": [],
    }
    return os.pathsep.join(map(str, [classes] + jars)), metadata


def words(depth):
    # Ultimate's general NWA algorithms exclude pending returns. Keep pending
    # calls, and compare Lotus's pending-return extension in the native tests.
    result = []
    for length in range(depth + 1):
        for word in itertools.product(range(3), repeat=length):
            balance = 0
            for letter in word:
                balance += (letter == 0) - (letter == 1)
                if balance < 0:
                    break
            else:
                result.append(word)
    return result


def automaton(rng, states):
    flags = [(int(q == 0 or rng.random() < 0.1), int(rng.random() < 0.35))
             for q in range(states)]
    transitions = []
    for source in range(states):
        for target in range(states):
            for letter in (0, 2):
                if rng.random() < 0.18:
                    transitions.append((source, letter, target, -1))
            for hierarchy in range(states):
                if rng.random() < 0.09:
                    transitions.append((source, 1, target, hierarchy))
    return {"flags": flags, "transitions": transitions}


def make_corpus(args, work):
    rng = random.Random(args.seed)
    queries = words(args.depth)
    omega = list(itertools.product(
        [(), (2,), (0,)],
        [(2,), (0,), (0, 1), (0, 2, 1), (2, 0, 1), (0, 0, 1), (0, 2), (2, 2, 0, 1)],
    ))
    cases = [(automaton(rng, rng.randint(2, 4)), automaton(rng, rng.randint(2, 4)))
             for _ in range(args.cases)]
    # Deterministic boundary cases: no initial states, no final states, epsilon,
    # a balanced-only accepting path, and a call-only infinite accepting path.
    empty = {"flags": [], "transitions": []}
    epsilon = {"flags": [(1, 1)], "transitions": []}
    nested = {"flags": [(1, 0), (0, 0), (0, 1)],
              "transitions": [(0, 0, 1, -1), (1, 1, 2, 0)]}
    growing = {"flags": [(1, 1)], "transitions": [(0, 0, 0, -1)]}
    cases = [(empty, empty), (epsilon, empty), (nested, epsilon), (growing, nested)] + cases
    lines = [str(len(cases))]
    locations = []
    for index, pair in enumerate(cases):
        for a in pair:
            lines.append(f"{len(a['flags'])} {len(a['transitions'])}")
            lines.extend(" ".join(map(str, flag)) for flag in a["flags"])
            lines.extend(" ".join(map(str, transition)) for transition in a["transitions"])
        lines.append(str(len(queries)))
        locations.append({"case": index, "kind": "emptiness"})
        for word in queries:
            lines.append(" ".join(map(str, (len(word),) + word)))
            locations.append({"case": index, "kind": "membership", "word": word})
        lines.append(str(len(omega)))
        for stem, loop in omega:
            lines.append(" ".join(map(str, (len(stem),) + stem)))
            lines.append(" ".join(map(str, (len(loop),) + loop)))
            locations.append({"case": index, "kind": "omega", "stem": stem, "loop": loop})
    corpus = ("\n".join(lines) + "\n").encode()
    (work / "corpus.txt").write_bytes(corpus)
    (work / "cases.json").write_text(json.dumps(cases, indent=2) + "\n")
    return corpus, locations, len(cases), len(queries), len(omega)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ultimate", type=Path, required=True)
    parser.add_argument("--cpp-runner", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--java", default="java")
    parser.add_argument("--ecj", type=Path)
    parser.add_argument("--dependency-jar", type=Path, action="append", default=[])
    parser.add_argument("--maven-repository", type=Path, default=Path.home() / ".m2/repository")
    parser.add_argument("--cases", type=int, default=40)
    parser.add_argument("--depth", type=int, default=6)
    parser.add_argument("--seed", type=int, default=1729)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if args.cases < 0 or not 0 <= args.depth <= 10 or args.timeout <= 0:
        parser.error("cases must be nonnegative, depth in [0,10], and timeout positive")
    args.work_dir.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="lotus-nwa-differential-", dir=args.work_dir))
    print(f"Artifacts: {work}", flush=True)
    runner = args.cpp_runner.expanduser().resolve(strict=True)
    classpath, metadata = build_oracle(args, args.ultimate.expanduser().resolve(), work)
    corpus, locations, cases, queries, omega = make_corpus(args, work)
    metadata.update({"seed": args.seed, "cases": cases, "words_per_case": queries,
                     "max_word_length": args.depth, "cpp_runner": str(runner),
                     "omega_queries_per_case": omega,
                     "cpp_runner_sha256": fingerprint(runner),
                     "corpus_sha256": hashlib.sha256(corpus).hexdigest(),
                     "common_semantics": "finite open calls; no pending returns"})
    metadata["finite_columns"] = ["A", "B", "det(A)", "det(B)", "A-B", "complement(A)",
                                  "intersection(A,B)", "prune(A)", "direct(A)", "game_direct(A)"]
    metadata["omega_columns"] = ["A", "direct(A)", "delayed(A)", "game_direct(A)", "game_delayed(A)"]
    started = time.monotonic()
    native = execute([runner], work, "lotus", args.timeout, stdin=corpus).splitlines()
    metadata["lotus_seconds"] = time.monotonic() - started
    started = time.monotonic()
    java = execute([args.java, "-ea", "-Xmx1g", "-cp", classpath, "UltimateNwaOracle"],
                   work, "ultimate", args.timeout, stdin=corpus).splitlines()
    metadata["ultimate_seconds"] = time.monotonic() - started
    differences = []
    for index, (left, right) in enumerate(itertools.zip_longest(native, java)):
        if left != right:
            differences.append({"line": index + 1,
                                "location": locations[index] if index < len(locations) else None,
                                "lotus": None if left is None else left.decode(),
                                "ultimate": None if right is None else right.decode()})
    metadata["comparisons"] = cases * (5 + len(metadata["finite_columns"]) * queries +
                                      len(metadata["omega_columns"]) * omega)
    metadata["differing_output_lines"] = differences
    metadata["passed"] = not differences and len(native) == len(locations) == len(java)
    (work / "report.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"{metadata['comparisons']} Boolean comparisons; {len(differences)} differing lines", flush=True)
    print(f"Report: {work / 'report.json'}", flush=True)
    return 0 if metadata["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
