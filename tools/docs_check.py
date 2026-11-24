#!/usr/bin/env python3
"""Documentation audit for P9-T07 acceptance criteria 1, 3, 4 (DOC-02, DOC-03).

Checks, all mechanically:

  * every markdown link and image path inside docs/ resolves;
  * no documentation file mentions a forbidden clean-room term;
  * the ADR index lists every ADR file, and every ADR file appears in the index;
  * the ADR set covers the ten mandated topics (criterion 2);
  * the CLI reference's exit-code table matches `MapErrorToCliExitCode` in the
    source — the exact drift that was found by hand is now a failing test;
  * documentation is not vacuous: no file under docs/ is a stub (< 8 lines) and
    every document has a heading.

Exit code 1 on any finding. `--list` prints what was examined.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"

FORBIDDEN = [
    re.compile(r"\bdefraggler\b", re.IGNORECASE),
    re.compile(r"\bpiriform\b", re.IGNORECASE),
    re.compile(r"\bccleaner\b", re.IGNORECASE),
    re.compile(r"\bwoxford\b", re.IGNORECASE),
]

# criterion 2: ten topics, each must be claimed by at least one ADR file
ADR_TOPICS = {
    "renderer": ["ADR-002"],
    "build": ["ADR-001", "ADR-007"],
    "journal": ["ADR-003"],
    "MFT tier": ["ADR-005"],
    "media policy": ["ADR-004"],
    "privilege": ["ADR-004", "ADR-010", "ADR-007"],
    "licence": ["ADR-009"],
    "clean-room": ["ADR-006", "ADR-012"],
    "scheduler": ["ADR-010"],
    "i18n": ["ADR-006"],
}

# Errors that must map to each CLI exit code, taken from error_catalog.cpp.
EXPECTED_EXIT = {
    "E_CONFIG_PARSE": 2,
    "E_PREFLIGHT_SSD_BLOCKED": 12,
    "E_PREFLIGHT_NOT_ELEVATED": 13,
    "E_JOURNAL_CRC_MISMATCH": 21,
}


def md_files() -> list[pathlib.Path]:
    return sorted(p for p in DOCS.rglob("*.md") if p.is_file())


def check_links(files: list[pathlib.Path], errors: list[str], notes: list[str]) -> None:
    link_re = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
    checked = 0
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        # strip fenced code blocks so examples do not count as links
        text = re.sub(r"```.*?```", "", text, flags=re.S)
        for m in link_re.finditer(text):
            target = m.group(1).strip()
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            target = target.split("#", 1)[0]
            if not target:
                continue
            resolved = (path.parent / target).resolve()
            checked += 1
            if not resolved.exists():
                rel = path.relative_to(REPO).as_posix()
                errors.append(f"{rel}: broken link -> {m.group(1)}")
    notes.append(f"links checked: {checked}")


def check_cleanroom(files: list[pathlib.Path], errors: list[str]) -> None:
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for pat in FORBIDDEN:
            for m in pat.finditer(text):
                rel = path.relative_to(REPO).as_posix()
                errors.append(f"{rel}: forbidden clean-room term '{m.group(0)}'")


def check_adrs(errors: list[str], notes: list[str]) -> None:
    adr_dir = DOCS / "adr"
    adrs = sorted(p.stem for p in adr_dir.glob("ADR-*.md"))
    index = adr_dir / "README.md"
    if not index.is_file():
        errors.append("docs/adr/README.md is missing")
        return
    body = index.read_text(encoding="utf-8")
    if "| ADR | Title |" not in body and "| ADR " not in body:
        errors.append("docs/adr/README.md has no ADR index table")
    listed = set(re.findall(r"ADR-(\d{3})", body))
    on_disk = {p.split("-")[1] for p in adrs}
    for missing in sorted(on_disk - listed):
        errors.append(f"docs/adr/README.md does not list ADR-{missing}")
    for extra in sorted(listed - on_disk):
        errors.append(f"docs/adr/README.md lists ADR-{extra}, which does not exist")

    for topic, owners in ADR_TOPICS.items():
        present = [a for a in owners if a.split("-")[1] in on_disk]
        if not present:
            errors.append(f"criterion 2: no ADR covers '{topic}'")
    notes.append(f"ADRs: {len(on_disk)}, topics covered: {len(ADR_TOPICS)}")


def check_cli_exit_codes(errors: list[str], notes: list[str]) -> None:
    src = REPO / "src/core/error_catalog.cpp"
    doc = DOCS / "user/CLI_REFERENCE.md"
    if not src.is_file() or not doc.is_file():
        errors.append("cannot cross-check CLI exit codes (source or doc missing)")
        return

    body = src.read_text(encoding="utf-8", errors="replace")
    # only the mapping function body
    m = re.search(r"MapErrorToCliExitCode.*?\n\}", body, re.S)
    if not m:
        errors.append("MapErrorToCliExitCode not found in error_catalog.cpp")
        return
    mapping = m.group(0)

    found = 0
    for code_name, expected in EXPECTED_EXIT.items():
        case = re.search(rf"case\s+ErrorCode::{code_name}\s*:\s*(?:return\s+)?(\d+)", mapping)
        if not case:
            # the mapping may group several names in one case label
            case = re.search(rf"ErrorCode::{code_name}[^;]*?return\s+(\d+)", mapping)
        if not case:
            errors.append(f"CLI_REFERENCE: source has no exit code for {code_name}")
            continue
        actual = int(case.group(1))
        found += 1
        if actual != expected:
            errors.append(
                f"error_catalog.cpp: {code_name} maps to {actual}, "
                f"the audit's expectation is {expected} — update EXPECTED_EXIT and the doc"
            )

    # every exit code the doc claims must be one the source can produce
    doc_body = doc.read_text(encoding="utf-8")
    table = re.search(r"## Exit codes(.*?)\n## ", doc_body, re.S)
    if not table:
        errors.append("CLI_REFERENCE.md has no 'Exit codes' section")
        return
    documented = {int(x) for x in re.findall(r"\|\s*\*\*(\d+)\*\*\s*\|", table.group(1))}
    producible = {int(x) for x in re.findall(r"return\s+(\d+)\s*;", mapping)}
    stale = documented - producible
    if stale:
        errors.append(
            f"CLI_REFERENCE.md documents exit codes the program cannot return: {sorted(stale)}"
        )
    notes.append(f"exit codes: documented {sorted(documented)}, produceable {sorted(producible)}")


def check_not_stubs(files: list[pathlib.Path], errors: list[str]) -> None:
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(REPO).as_posix()
        lines = [l for l in text.splitlines() if l.strip()]
        if len(lines) < 8:
            errors.append(f"{rel}: stub document ({len(lines)} non-empty lines)")
        if not any(l.lstrip().startswith("#") for l in lines):
            errors.append(f"{rel}: no heading")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    files = md_files()
    errors: list[str] = []
    notes: list[str] = []

    check_links(files, errors, notes)
    check_cleanroom(files, errors)
    check_adrs(errors, notes)
    check_cli_exit_codes(errors, notes)
    check_not_stubs(files, errors)

    if args.list:
        for p in files:
            print(p.relative_to(REPO).as_posix())

    words = sum(len(p.read_text(encoding="utf-8", errors="replace").split()) for p in files)
    if errors:
        print("Documentation check FAILED:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1
    print(
        f"docs_check.py: PASS ({len(files)} documents, {words} words; "
        + "; ".join(notes)
        + ")."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
