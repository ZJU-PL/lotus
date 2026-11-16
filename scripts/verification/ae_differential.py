#!/usr/bin/env python3
"""Run Lotus and SVF AE on identical LLVM 14 bitcode; retain every artifact.

Example:
  python3 scripts/verification/ae_differential.py --llvm-bin /path/to/llvm/bin \
    --svf /path/to/SVF/Release-build/bin/ae --out build/ae-differential

No third-party Python packages are required. Generated cases have deliberately
small, deterministic bounds. --corpus adds existing .c, .ll and .bc files.
Differences are evidence for review, not an assertion that SVF is correct.
"""

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import random
import re
import subprocess
import time


def run(command, cwd, timeout):
    start = time.monotonic()
    try:
        p = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        return {"rc": p.returncode, "seconds": round(time.monotonic() - start, 4),
                "output": p.stdout.decode(errors="replace"),
                "status": "ok" if p.returncode == 0 else "error"}
    except subprocess.TimeoutExpired as e:
        return {"rc": None, "seconds": round(time.monotonic() - start, 4),
                "output": (e.stdout or b"").decode(errors="replace"),
                "status": "timeout"}


def generated(seed, pairs):
    rng = random.Random(seed)
    for i in range(pairs):
        size = rng.randint(2, 32)
        # Boundary perturbations exercise direct, loop and interprocedural access.
        for bad in (False, True):
            label = "oob" if bad else "safe"
            index = size if bad else size - 1
            kind = i % 3
            if kind == 0:
                code = f"int main(void){{int a[{size}];a[{index}]=1;return 0;}}"
            elif kind == 1:
                code = (f"int main(void){{int a[{size}];for(int i=0;i<={index};"
                        "++i)a[i]=1;return 0;}")
            else:
                code = ("__attribute__((noinline)) void put(int*p,int i){p[i]=1;}"
                        f"int main(void){{int a[{size}];put(a,{index});return 0;}}")
            yield f"generated_{i:03d}_{label}", code, ["buffer-overflow"] if bad else []
    for name, char, truth in [("isdigit", 65, False), ("isdigit", 48, True),
                              ("isalpha", 48, False), ("isalpha", 65, True),
                              ("isspace", 65, False), ("isspace", 32, True),
                              ("isupper", 97, False), ("isupper", 65, True)]:
        for negate in (False, True):
            test = f"{name}({char})" if not negate else f"!{name}({char})"
            code = (f"extern int {name}(int);int main(void){{int a[2];"
                    f"if({test})a[2]=1;return 0;}}")
            yield f"ctype_{name}_{char}_{int(negate)}", code, (
                ["buffer-overflow"] if truth != negate else [])
    fixed = {
        "null_direct": ("int main(void){int*p=0;return *p;}", ["null-deref"]),
        "null_guard": ("int main(int n,char**v){int x=1;int*p=n>1?&x:0;"
                       "if(p)return *p;return 0;}", []),
        "negative_index": ("int main(void){int a[4];a[-1]=0;return 0;}",
                           ["buffer-overflow"]),
        "struct_subobject": ("int main(void){struct{int a[2];int b;}s;"
                             "s.a[2]=0;return 0;}", ["buffer-overflow"]),
        "indirect_oob": ("void put(int*p){p[4]=1;}void(*f)(int*)=put;"
                         "int main(void){int a[4];f(a);return 0;}",
                         ["buffer-overflow"]),
        "snprintf_oob": ("extern int snprintf(char*,unsigned long,const char*,...);"
                         "int main(void){char a[2];snprintf(a,5,\"abcd\");return 0;}",
                         ["buffer-overflow"]),
        "guarded_sext": ("int main(int n,char**v){int a[4]={0};signed char i=n;"
                         "if(i>=0&&i<4)return a[i];return 0;}", []),
        "guarded_zext": ("int main(int n,char**v){int a[8]={0};unsigned char i=n;"
                         "if(i<8)return a[i];return 0;}", []),
        "constant_trunc": ("int main(void){int a[4]={0};long i=0x100000003L;"
                           "int j=(int)i;return a[j];}", []),
    }
    for name, (code, expected) in fixed.items():
        yield name, code, expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--svf", type=Path, required=True)
    parser.add_argument("--lotus", type=Path, default=Path("build/bin/lotus-check"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, action="append", default=[])
    parser.add_argument("--no-generated", action="store_true")
    parser.add_argument("--entry", choices=["main", "no-main"], default="main")
    parser.add_argument("--seed", type=int, default=20261116)
    parser.add_argument("--pairs", type=int, default=24)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=15)
    parser.add_argument("--modes", nargs="+", default=["dense", "semi-sparse", "sparse"],
                        choices=["dense", "semi-sparse", "sparse"])
    parser.add_argument("--forms", nargs="+", default=["raw", "mem2reg"],
                        choices=["raw", "mem2reg"])
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    args.svf, args.lotus = args.svf.resolve(), args.lotus.resolve()
    clang, opt = [str(args.llvm_bin.resolve() / name) for name in ("clang", "opt")]
    sources = args.out / "sources"
    sources.mkdir(exist_ok=True)
    cases = []
    for name, code, expected in ([] if args.no_generated else generated(args.seed, args.pairs)):
        path = sources / (name + ".c")
        path.write_text(code + "\n")
        cases.append((name, path, expected))
    for corpus in args.corpus:
        paths = sorted(corpus.rglob("*")) if corpus.is_dir() else [corpus]
        for path in paths:
            if path.suffix in (".c", ".ll", ".bc"):
                # Preserve source/IR inputs as well as compiled bitcode.
                digest = hashlib.sha256(str(path.resolve()).encode()).hexdigest()[:8]
                name = "existing_" + path.stem + "_" + digest
                copied = sources / (name + path.suffix)
                copied.write_bytes(path.read_bytes())
                cases.append((name, copied, None))

    def check(case):
        name, source, expected = case
        work = args.out / name
        work.mkdir(exist_ok=True)
        raw = work / "raw.bc"
        if source.suffix == ".c":
            command = [clang, "-O0", "-g", "-fno-builtin", "-Xclang",
                       "-disable-O0-optnone", "-emit-llvm", "-c", str(source), "-o", str(raw)]
        else:
            command = [opt, str(source), "-o", str(raw)]
        compilation = run(command, work, args.timeout)
        (work / "compile.log").write_text(compilation["output"])
        if compilation["status"] != "ok":
            return [{"case": name, "status": "compile-error"}]
        rows = []
        for form in args.forms:
            bc = raw
            if form == "mem2reg":
                bc = work / "mem2reg.bc"
                promoted = run([opt, "-mem2reg", str(raw), "-o", str(bc)], work, args.timeout)
                (work / "mem2reg.log").write_text(promoted["output"])
                if promoted["status"] != "ok":
                    rows.append({"case": name, "form": form, "status": "opt-error"})
                    continue
            for mode in args.modes:
                row = {"case": name, "form": form, "mode": mode, "expected": expected,
                       "bitcode_sha256": hashlib.sha256(bc.read_bytes()).hexdigest()}
                for engine in ("lotus", "svf"):
                    cwd = work / (form + "-" + mode + "-" + engine)
                    cwd.mkdir(exist_ok=True)
                    report = cwd / "report.json"
                    if engine == "lotus":
                        report.unlink(missing_ok=True)
                        command = [str(args.lotus), "--engine=ae",
                                   "--checks=buffer-overflow,null-deref",
                                   "--ae.state-store=" + mode, "--ae.fun-entry=" + args.entry,
                                   "--report-json=" + str(report), str(bc)]
                    else:
                        command = [str(args.svf), "-overflow", "-null-deref",
                                   "-ae-sparsity=" + mode, "-ae-fun-entry=" + args.entry, str(bc)]
                    result = run(command, cwd, args.timeout)
                    output = result.pop("output")
                    (cwd / "output.log").write_text(output)
                    result["command"] = command
                    counts = {"buffer-overflow": 0, "null-deref": 0}
                    if engine == "lotus":
                        try:
                            data = json.loads(report.read_text())
                            for bug in data["BugTypes"]:
                                key = {"AE Buffer Overflow": "buffer-overflow",
                                       "AE Null Dereference": "null-deref"}.get(bug["Name"])
                                if key:
                                    counts[key] += bug["TotalReports"]
                        except (OSError, ValueError, KeyError):
                            result["status"] = "report-error" if result["status"] == "ok" else result["status"]
                    else:
                        for label, key in [("Buffer Overflow", "buffer-overflow"),
                                           ("Nullptr Dereference", "null-deref")]:
                            counts[key] = sum(map(int, re.findall(
                                re.escape(label) + r" \((\d+) found\)", output)))
                    result["counts"] = counts
                    result["findings"] = sorted(k for k, v in counts.items() if v)
                    row[engine] = result
                row["status"] = "ok" if all(row[e]["status"] == "ok" for e in ("lotus", "svf")) else "incomplete"
                row["different"] = (row["lotus"]["findings"] != row["svf"]["findings"]) if row["status"] == "ok" else None
                rows.append(row)
        return rows

    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(check, cases):
            rows.extend(result)
    metadata = {"seed": args.seed, "pairs": args.pairs, "entry": args.entry,
                "modes": args.modes, "forms": args.forms,
                "corpora": [str(p.resolve()) for p in args.corpus],
                "tools": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (args.lotus, args.svf)},
                "clang_version": run([clang, "--version"], args.out, 5)["output"]}
    (args.out / "results.json").write_text(json.dumps({"metadata": metadata, "rows": rows}, indent=2) + "\n")
    lines = ["case\tform\tmode\tstatus\tlotus\tsvf\tdifferent\texpected"]
    for row in rows:
        lines.append("\t".join(str(v) for v in [row["case"], row.get("form", ""), row.get("mode", ""),
                     row["status"], ",".join(row.get("lotus", {}).get("findings", [])),
                     ",".join(row.get("svf", {}).get("findings", [])), row.get("different"),
                     row.get("expected")]))
    (args.out / "results.tsv").write_text("\n".join(lines) + "\n")
    print(json.dumps({"cases": len(cases), "comparisons": sum("lotus" in r for r in rows),
                      "differences": sum(r.get("different") is True for r in rows),
                      "incomplete": sum(r["status"] != "ok" for r in rows),
                      "compilation_failures": sum(r["status"] == "compile-error" for r in rows),
                      "results": str(args.out / "results.tsv")}, indent=2))
    return int(any(r["status"] != "ok" for r in rows))


if __name__ == "__main__":
    raise SystemExit(main())
