#!/usr/bin/env python3
"""Cross-check SVF's external-API model against lotus' ``config/*.spec``.

SVF models library functions in ``svf-llvm/lib/extapi.c``: every model is a C
definition carrying ``__attribute__((annotate("...")))`` tags, compiled to
``extapi.bc`` and merged into the analysed module (see
``LLVMModuleSet::buildFunToFunMap``).  lotus instead describes the same
functions in text specs (``config/ptr.spec``, ``config/modref.spec``, ...) that
``lotus::APISpec`` parses.

This script answers two questions with numbers instead of impressions:

1. Which functions does SVF annotate that lotus' specs do not mention at all?
2. Of those, how many can be expressed *today* with lotus' spec vocabulary
   (``IGNORE``/``ALLOC``/``DEALLOC``/``COPY``/``MOD``/``REF``), and how many
   need vocabulary lotus does not have (argument-side allocation, store-top,
   body-level ``OVERWRITE``)?

Usage::

    # Report (default): gap summary + per-annotation counts
    python3 scripts/compare_svf_extapi.py --svf-root ~/Downloads/SVF-master

    # Emit candidate spec lines for the SVF-only, mappable functions
    python3 scripts/compare_svf_extapi.py --emit-spec /tmp/extapi-extra.spec

    # Emit the same set as a JSON contract (APISpec::loadJSONFile format)
    python3 scripts/compare_svf_extapi.py --emit-json /tmp/extapi-extra.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

# ---------------------------------------------------------------------------
# extapi.c parsing
# ---------------------------------------------------------------------------

# A model = one or more annotate("...") ordinates followed by a C definition.
_FUNC_RE = re.compile(
    r"__attribute__\(\(\s*(?P<attrs>(?:annotate\(\"[^\"]*\"\)\s*,?\s*)+)\)\)"
    r"\s*(?P<sig>[A-Za-z_][^;{}()]*?(?P<name>[A-Za-z_][\w.$]*)\s*\((?P<args>[^;{}]*)\))\s*"
    r"(?P<body>\{[^{}]*\}|\{|\s*;)",
    re.S,
)
_ANNOT_RE = re.compile(r'annotate\("([^"]*)"\)')


class Model:
    __slots__ = ("name", "annotations", "nargs")

    def __init__(self, name: str, annotations: list[str], nargs: int) -> None:
        self.name = name
        self.annotations = annotations
        self.nargs = nargs

    @property
    def kinds(self) -> list[str]:
        """Annotation "kinds": the part before ':' (e.g. STORE_TOP:Arg1+)."""
        return [a.split(":")[0] for a in self.annotations]

    def size_arg(self) -> int | None:
        for a in self.annotations:
            m = re.fullmatch(r"AllocSize:Arg(\d+)", a)
            if m:
                return int(m.group(1))
        return None


def parse_extapi(path: Path) -> list[Model]:
    text = path.read_text(encoding="utf-8", errors="replace")
    models: list[Model] = []
    for m in _FUNC_RE.finditer(text):
        annotations = _ANNOT_RE.findall(m.group("attrs"))
        if not annotations:
            continue
        args = m.group("args").strip()
        nargs = 0 if args in ("", "void") else args.count(",") + 1
        models.append(Model(m.group("name"), annotations, nargs))
    return models


# ---------------------------------------------------------------------------
# lotus spec parsing
# ---------------------------------------------------------------------------


def parse_specs(config_dir: Path) -> tuple[set[str], dict[str, set[str]]]:
    """Return (all declared names, name -> set of files declaring it)."""
    names: set[str] = set()
    where: dict[str, set[str]] = defaultdict(set)
    for spec in sorted(config_dir.glob("*.spec")):
        for raw in spec.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) < 2:
                continue
            names.add(fields[0])
            where[fields[0]].add(spec.name)
    return names, where


# ---------------------------------------------------------------------------
# Annotation -> lotus spec translation
# ---------------------------------------------------------------------------


def translate(model: Model) -> list[tuple[str, str]]:
    """Return candidate lotus spec lines ``(op_class, text)`` for one model.

    ``op_class`` is ``"exact"`` when the translation preserves SVF's meaning
    with lotus' current vocabulary and ``"approx"`` when it only approximates
    it.  Unmappable models return an empty list.
    """
    kinds = set(model.kinds)
    out: list[tuple[str, str]] = []
    n = model.name

    if "ALLOC_HEAP_RET" in kinds:
        size = model.size_arg()
        out.append(("exact", f"{n} ALLOC Arg{size}" if size is not None else f"{n} ALLOC"))

    # SVF: memcpy/strcpy/strcat copy from the region of arg1 into arg0.
    if kinds & {"MEMCPY", "STRCPY", "STRCAT"}:
        out.append(("exact", f"{n} COPY Arg0 R Arg1 R"))

    # SVF: memset writes a non-pointer value into the region of arg0.
    # lotus convention (see existing memset entry): COPY Arg0 R NULL V.
    if "MEMSET" in kinds:
        out.append(("exact", f"{n} COPY Arg0 R NULL V"))

    # SVF: "reads modeled fields of argument 0".
    if "STRLEN" in kinds:
        out.append(("exact", f"{n} REF Arg0 D"))

    # SVF: SCANF:FormatArgi -- arg i is the format string, outputs follow it.
    for a in model.annotations:
        m = re.fullmatch(r"SCANF:FormatArg(\d+)", a)
        if m:
            i = int(m.group(1))
            out.append(("approx", f"{n} MOD AfterArg{i} D"))
            out.append(("approx", f"{n} REF Arg{i} D"))

    return out


def unsupported_reason(model: Model) -> str:
    kinds = set(model.kinds)
    if "OVERWRITE" in kinds:
        return "needs body-level replacement (OVERWRITE)"
    for k in model.kinds:
        if k.startswith("ALLOC_HEAP_ARG"):
            return "needs argument-side allocation (ALLOC_HEAP_ARGi)"
        if k == "STORE_TOP":
            return "needs store-top / unknown-value summary (STORE_TOP)"
    if "REALLOC_HEAP_RET" in kinds:
        return "needs realloc vocabulary (lotus JSON 'reallocate' only)"
    return "unmapped"


# ---------------------------------------------------------------------------


def main() -> int:
    default_lotus = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--svf-root", default=os.path.expanduser("~/Downloads/SVF-master"))
    ap.add_argument("--lotus-root", default=str(default_lotus))
    ap.add_argument("--emit-spec", help="write candidate spec lines for SVF-only functions")
    ap.add_argument("--emit-json", help="write candidate JSON contracts for SVF-only functions")
    ap.add_argument("--show", type=int, default=15, help="how many example names to print")
    args = ap.parse_args()

    extapi = Path(args.svf_root) / "svf-llvm" / "lib" / "extapi.c"
    if not extapi.is_file():
        print(f"error: {extapi} not found (pass --svf-root)", file=sys.stderr)
        return 2
    config_dir = Path(args.lotus_root) / "config"
    if not config_dir.is_dir():
        print(f"error: {config_dir} not found (pass --lotus-root)", file=sys.stderr)
        return 2

    models = parse_extapi(extapi)
    declared, where = parse_specs(config_dir)

    kind_counts = Counter(k for m in models for k in m.kinds)
    svf_names = {m.name for m in models}
    overlap = sorted(svf_names & declared)
    svf_only = sorted(svf_names - declared)
    lotus_only = sorted(declared - svf_names)

    mappable: list[tuple[Model, list[tuple[str, str]]]] = []
    blocked: dict[str, list[str]] = defaultdict(list)
    for m in sorted(models, key=lambda x: x.name):
        if m.name in declared:
            continue
        lines = translate(m)
        if lines:
            mappable.append((m, lines))
        else:
            blocked[unsupported_reason(m)].append(m.name)

    print("=" * 78)
    print("SVF extapi.c  vs  lotus config/*.spec")
    print("=" * 78)
    print(f"SVF extapi.c models (annotated C definitions) : {len(models):>4}  ({len(svf_names)} unique names)")
    print(f"lotus spec declarations (config/*.spec)       : {len(declared):>4} unique names")
    print(f"  also annotated by SVF                       : {len(overlap):>4}")
    print(f"  SVF-only (candidate imports)                : {len(svf_only):>4}")
    print(f"lotus-only (no SVF counterpart)               : {len(lotus_only):>4}")
    print()
    print("lotus spec files (SVF only models libc/glibc/C++ runtime)")
    print("-" * 78)
    for spec in sorted(config_dir.glob("*.spec")):
        names = [n for n, files in where.items() if spec.name in files]
        hit = len([n for n in names if n in svf_names])
        print(f"  {spec.name:<24} {len(names):>4} names   {hit:>4} also in extapi.c")
    print()
    print("SVF annotation inventory")
    print("-" * 78)
    for kind, count in kind_counts.most_common():
        print(f"  {kind:<20} {count:>4}")
    print()
    print("Disposition of the SVF-only models")
    print("-" * 78)
    exact = sum(1 for _, lines in mappable if all(c == "exact" for c, _ in lines))
    approx = len(mappable) - exact
    print(f"  expressible with lotus' current vocabulary  : {len(mappable):>4} (exact {exact}, approximate {approx})")
    for reason, names in sorted(blocked.items(), key=lambda kv: -len(kv[1])):
        print(f"  blocked: {reason:<45} {len(names):>4}")
    print()
    if overlap:
        print(f"Already covered by lotus specs (sample of {min(args.show, len(overlap))}):")
        print("  " + ", ".join(overlap[: args.show]))
        print()
    if mappable:
        print(f"Importable, currently unmodelled (sample of {min(args.show, len(mappable))}):")
        for model, lines in mappable[: args.show]:
            print(f"  {model.name:<28} <- {', '.join(model.annotations)}")
        print()
    for reason, names in sorted(blocked.items(), key=lambda kv: -len(kv[1])):
        if not names:
            continue
        print(f"{reason} (sample of {min(args.show, len(names))}):")
        print("  " + ", ".join(sorted(names)[: args.show]))
        print()

    if args.emit_spec:
        lines: list[str] = [
            "# Candidate imports from SVF extapi.c (generated by scripts/compare_svf_extapi.py)",
            "# Review before installing: names are libc/glibc/C++-mangled symbols.",
        ]
        for model, translated in mappable:
            for cls, text in translated:
                lines.append(text if cls == "exact" else f"{text}   # approximate")
        Path(args.emit_spec).write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"wrote {args.emit_spec} ({len(lines) - 2} lines)")

    if args.emit_json:
        functions: dict[str, dict] = {}
        for model, _ in mappable:
            kinds = set(model.kinds)
            record: dict[str, object] = {}
            if "ALLOC_HEAP_RET" in kinds or "REALLOC_HEAP_RET" in kinds:
                record["malloc"] = True
                size = model.size_arg()
                if size is not None:
                    record["size-arguments"] = [size]
            if "REALLOC_HEAP_RET" in kinds:
                record["reallocate"] = True
            deref = set()
            for a in model.annotations:
                m = re.fullmatch(r"STORE_TOP:Arg(\d+)\+", a)
                if m:
                    # dereference of a pointer-ish argument region
                    deref.add(int(m.group(1)))
            if kinds & {"MEMCPY", "MEMSET", "STRCPY", "STRCAT"}:
                deref.add(0)
            if kinds & {"MEMCPY", "STRCPY", "STRCAT"}:
                deref.add(1)
            if "STRLEN" in kinds:
                deref.add(0)
            if deref:
                record["dereferenced-arguments"] = sorted(deref)
            if record:
                functions[model.name] = record
        Path(args.emit_json).write_text(json.dumps({"functions": functions}, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.emit_json} ({len(functions)} function records)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
