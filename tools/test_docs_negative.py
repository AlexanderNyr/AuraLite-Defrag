#!/usr/bin/env python3
"""Negative tests for the documentation audit (P9-T07, DOC-02/DOC-03).

A documentation check that has never failed is not evidence of anything. Each
case below damages the documentation tree in a specific way, runs the audit, and
asserts that it fails — and, for the exit-code case, that it fails *because* of
the intended defect rather than by accident.

The tree is restored after every case; a mismatch raises rather than leaving a
half-edited repository behind.
"""
from __future__ import annotations

import contextlib
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
PY = sys.executable


@contextlib.contextmanager
def with_backup(*paths: pathlib.Path):
    backups = {}
    try:
        for p in paths:
            if p.is_file():
                backups[p] = p.read_bytes()
            elif p.is_dir():
                backups[p] = ("dir", {q: q.read_bytes() for q in p.rglob("*") if q.is_file()})
            else:
                backups[p] = None
        yield
    finally:
        for p, data in backups.items():
            if data is None:
                if p.exists():
                    p.unlink()
                continue
            if isinstance(data, tuple):
                for q in list(p.rglob("*")):
                    if q.is_file():
                        q.unlink()
                for q, body in data[1].items():
                    q.parent.mkdir(parents=True, exist_ok=True)
                    q.write_bytes(body)
            else:
                p.write_bytes(data)


def run(cmd: list[str]) -> tuple[int, str]:
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr)


CASES: list[str] = []


def case(name):
    def deco(fn):
        CASES.append(name)
        fn._name = name
        fn._check = True
        return fn
    return deco


@case("broken relative link")
def _broken_link():
    doc = REPO / "docs/user/CLI_REFERENCE.md"
    with with_backup(doc):
        doc.write_text(doc.read_text() + "\nSee [the missing page](does_not_exist.md).\n")
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "broken link" in out, out
        return "docs_check rejected a link to a nonexistent file"


@case("clean-room term in an ADR")
def _cleanroom_adr():
    doc = REPO / "docs/adr/ADR-003-binary-journal.md"
    with with_backup(doc):
        doc.write_text(doc.read_text() + "\nInspired by Piriform.\n")
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "clean-room" in out, out
        return "docs_check rejected a prohibited term in docs/adr"


@case("ADR missing from the index")
def _adr_unindexed():
    idx = REPO / "docs/adr/README.md"
    with with_backup(idx):
        text = idx.read_text().replace("| [012](ADR-012-documentation-and-cleanroom.md)", "| [012]")
        idx.write_text(text)
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "does not list ADR-012" in out, out
        return "docs_check noticed an ADR that is not in the index"


@case("ADR index references a missing ADR")
def _adr_phantom():
    idx = REPO / "docs/adr/README.md"
    with with_backup(idx):
        idx.write_text(idx.read_text() + "\n| [013](ADR-013-imaginary.md) | Imaginary | Accepted | nothing |\n")
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "does not exist" in out, out
        return "docs_check noticed an index entry with no file behind it"


@case("mandated ADR topic not covered")
def _adr_topic():
    doc = REPO / "docs/adr/ADR-010-scheduler-integration.md"
    with with_backup(doc):
        doc.unlink()
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "'scheduler'" in out, out
        return "docs_check noticed that deleting ADR-010 removes scheduler coverage"


@case("exit code documented that the program cannot return")
def _stale_exit_code():
    doc = REPO / "docs/user/CLI_REFERENCE.md"
    with with_backup(doc):
        doc.write_text(doc.read_text().replace("| **13** | Elevation required", "| **10** | Elevation required"))
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "cannot return" in out, out
        return "docs_check caught the exact 10-vs-13 drift found by hand"


@case("source mapping changed, docs left behind")
def _source_drift():
    src = REPO / "src/core/error_catalog.cpp"
    with with_backup(src):
        text = src.read_text()
        # NOT_ELEVATED: 13 -> 7, as if someone reshuffled the taxonomy
        text = text.replace("return 13;", "return 7;", 1)
        src.write_text(text)
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "E_PREFLIGHT_NOT_ELEVATED" in out, out
        return "docs_check follows the source, not just the document"


@case("stub document")
def _stub():
    doc = REPO / "docs/dev/GLOSSARY.md"
    with with_backup(doc):
        doc.write_text("# Glossary\n\nTODO.\n")
        code, out = run([PY, "tools/docs_check.py"])
        assert code != 0 and "stub document" in out, out
        return "docs_check rejected a stub"


@case("traceability coverage removed")
def _trace():
    gen = REPO / "tools/gen_traceability.py"
    with with_backup(gen):
        text = gen.read_text().replace('    "R-098": (TM, None),\n', "")
        gen.write_text(text)
        code, out = run([PY, "tools/gen_traceability.py"])
        assert code != 0 and "R-098" in out, out
        return "traceability matrix refused to drop a MUST requirement"


@case("stale traceability matrix")
def _trace_stale():
    out_file = REPO / "docs/dev/TRACEABILITY.md"
    with with_backup(out_file):
        out_file.write_text(out_file.read_text().replace("| R-001 |", "| R-XXX |", 1))
        code, out = run([PY, "tools/gen_traceability.py"])
        assert code != 0 and "stale" in out, out
        return "traceability matrix detected a hand-edit"


def main() -> int:
    failures = 0
    total = 0
    for fn in (f for f in globals().values() if getattr(f, "_check", False)):
        total += 1
        try:
            detail = fn()
            print(f"  [ok] {fn._name}: {detail}")
        except AssertionError as exc:
            failures += 1
            print(f"  [FAIL] {fn._name}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"  [FAIL] {fn._name}: unexpected {type(exc).__name__}: {exc}")

    code, out = run([PY, "tools/docs_check.py"])
    total += 1
    if code == 0:
        print(f"  [ok] clean tree after the damage was undone: {out.strip()}")
    else:
        failures += 1
        print(f"  [FAIL] clean tree does not pass: {out}")

    print(f"\n{total - failures}/{total} documentation negative tests passed.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
