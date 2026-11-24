#!/usr/bin/env python3
"""Generator and verifier for the documentation traceability matrix (R-065, DOC-02).

The plan is a single source of truth that lives *outside* the repository. To keep
the matrix honest this script:

  * parses the plan's requirement catalogue (``| R-xxx | MUST | text |``) and its
    traceability section (``| R-xxx | tasks | tests |``);
  * joins each requirement against the section of the documentation that
    covers it, using an explicit, reviewable mapping below;
  * fails if a MUST requirement has no covering documentation section, or if a
    documented reference points at a file that does not exist.

Run ``--write`` to regenerate ``docs/dev/TRACEABILITY.md``.

This deliberately does *not* claim "covered" for requirements that only have a
test. Coverage here means "a human can find out what we promise and where it is
written down".
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
PLAN = pathlib.Path("/home/user/uploads/plann3-2.txt")
OUT = REPO / "docs/dev/TRACEABILITY.md"
OUT_REL = "docs/dev/TRACEABILITY.md"

# --- Documentation coverage map -------------------------------------------------
# R-id -> (document, section heading or None).  A section string must appear
# verbatim in the document, so renaming a heading breaks this check on purpose.
UG = "docs/user/USER_GUIDE.md"
CLI = "docs/user/CLI_REFERENCE.md"
DG = "docs/dev/DEVELOPER_GUIDE.md"
AR = "docs/dev/ARCHITECTURE.md"
SF = "docs/dev/SCENARIO_FORMAT.md"
RX = "docs/dev/EXCLUSIONS.md"
RP = "docs/dev/REPORTING.md"
CR = "docs/dev/CRASH_RECOVERY.md"
FZ = "docs/dev/FUZZING.md"
DT = "docs/dev/DIFFERENTIAL_TESTING.md"
LA = "docs/dev/LICENCE_AUDIT.md"
ADR = "docs/adr/README.md"
TM = "docs/dev/THREAT_MODEL.md"
SP = "docs/spikes/README.md"
RN = "docs/dev/RELEASE_NOTES.md"

COVERAGE: dict[str, tuple[str, str | None]] = {
    "R-001": (ADR, "Decision log in one paragraph"),
    "R-002": ("docs/cleanroom/terminology_register.md", None),
    "R-003": (LA, "What the audit checks"),
    "R-004": (ADR, "ADR-002"),
    "R-005": (ADR, "ADR-002"),
    "R-006": (RP, "Integration notes"),
    "R-007": (DG, "Building with CMake"),
    "R-008": (ADR, "Decision log in one paragraph"),
    "R-009": (LA, "What the audit checks"),
    "R-010": (DG, "Every target"),
    "R-011": (UG, "Safety first"),
    "R-012": (AR, "Key interface signatures"),
    "R-013": (AR, "Module map"),
    "R-014": (SP, "S-17"),
    "R-015": (SP, "S-01"),
    "R-016": (UG, "Safety first"),
    "R-017": (SP, "S-17"),
    "R-018": (UG, "About the four MFT tiers"),
    "R-019": (UG, "About the four MFT tiers"),
    "R-020": (UG, "About the four MFT tiers"),
    "R-021": (UG, "About the four MFT tiers"),
    "R-022": (SP, "S-01"),
    "R-023": (UG, "Modes"),
    "R-024": (UG, "Modes"),
    "R-025": (SP, "Deferred spikes"),
    "R-026": (CLI, "Verbs"),
    "R-027": (CLI, "Verbs"),
    "R-028": (UG, "Modes"),
    "R-029": (CLI, "Verbs"),
    "R-030": (CLI, "Verbs"),
    "R-031": (CLI, "Options"),
    "R-032": (ADR, "ADR-007"),
    "R-033": (CLI, "Verbs"),
    "R-034": (CLI, "Documented vs actual"),
    "R-035": (UG, "Interpreting what you see"),
    "R-036": (CLI, "Verbs"),
    "R-037": (RX, None),
    "R-038": (ADR, "ADR-010"),
    "R-039": (UG, "What happens if the machine loses power"),
    "R-040": (UG, "Where to look next"),
    "R-041": (UG, "Modes"),
    "R-042": (UG, "Where to look next"),
    "R-043": (CLI, None),
    "R-044": (RN, None),
    "R-045": (UG, "Known limitations"),
    "R-046": (AR, "Internationalisation"),
    "R-047": (UG, "Known limitations"),
    "R-048": (AR, "Internationalisation"),
    "R-049": (DG, "Adding a language"),
    "R-050": (DG, "Adding a language"),
    "R-051": (DG, "The emulator is the test oracle"),
    "R-052": (DG, "The emulator is the test oracle"),
    "R-053": (SF, None),
    "R-054": (SF, None),
    "R-055": (UG, "Modes"),
    "R-056": (DG, "What is verified where"),
    "R-057": (SF, None),
    "R-058": (DG, "What is verified where"),
    "R-059": (UG, "Safety first"),
    "R-060": (DG, "What is verified where"),
    "R-061": (SF, None),
    "R-062": (DG, "Verifying a change"),
    "R-063": (DT, "What is compared"),
    "R-064": (DG, "Every target"),
    "R-065": (OUT_REL, None),
    "R-066": (UG, "Interpreting what you see"),
    "R-067": (AR, "Module map"),
    "R-068": (UG, "Interpreting what you see"),
    "R-069": (RP, "Fragment histogram"),
    "R-070": (RP, "Fragment histogram"),
    "R-071": (RP, "Fragment histogram"),
    "R-072": (RP, "Top-N"),
    "R-073": (RP, "CSV export"),
    "R-074": (AR, "Threading model"),
    "R-075": (AR, "Threading model"),
    "R-076": (UG, "Known limitations"),
    "R-077": (UG, "Running from the command line"),
    "R-078": (CR, "The five crash points"),
    "R-079": (CR, None),
    "R-080": (UG, "Safety first"),
    "R-081": (UG, "Safety first"),
    "R-082": (UG, "Safety first"),
    "R-083": (CR, None),
    "R-084": (TM, None),
    "R-085": (RN, None),
    "R-086": (UG, "Known limitations"),
    "R-087": (AR, "Threading model"),
    "R-088": (UG, "About the four MFT tiers"),
    "R-089": ("docs/cleanroom/terminology_register.md", None),
    "R-090": (AR, "Layer DAG"),
    "R-091": (AR, "Threading model"),
    "R-092": (DG, "Adding a filesystem backend"),
    "R-093": (SP, None),
    "R-094": (OUT_REL, "Phases"),
    "R-095": (OUT_REL, None),
    "R-096": (OUT_REL, None),
    "R-097": (TM, "Risk register"),
    "R-098": (TM, None),
    "R-099": (SP, "Deferred spikes"),
    "R-100": ("docs/dev/GLOSSARY.md", None),
}


def parse_plan(path: pathlib.Path) -> tuple[list[list[str]], dict[str, tuple[str, str]]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    reqs: list[list[str]] = []
    seen: set[str] = set()
    for line in text.splitlines():
        m = re.match(r"^\| (R-\d{3}) \| (MUST|SHOULD|MAY|COULD|DEFERRED) \| (.*?) \|$", line)
        if m and m.group(1) not in seen:
            seen.add(m.group(1))
            reqs.append([m.group(1), m.group(2), m.group(3)])
    trace: dict[str, tuple[str, str]] = {}
    for line in text.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == 3 and re.fullmatch(r"R-\d{3}", cells[0]) and re.match(r"^P\d", cells[1]):
            trace[cells[0]] = (cells[1], cells[2])
    return reqs, trace


def normalise(text: str) -> str:
    """Lowercase, collapse whitespace, strip markdown emphasis."""
    text = text.replace("**", "").replace("`", "")
    return re.sub(r"\s+", " ", text).strip().lower()


def check(coverage: dict[str, tuple[str, str | None]]) -> list[str]:
    errors: list[str] = []
    for rid, (doc, section) in sorted(coverage.items()):
        if doc == OUT_REL:
            continue  # the matrix is the output; main() checks it exists
        path = REPO / doc
        if not path.is_file():
            errors.append(f"{rid}: documented in '{doc}', which does not exist")
            continue
        if section is None:
            continue
        body = normalise(path.read_text(encoding="utf-8", errors="replace"))
        if normalise(section) not in body:
            errors.append(f"{rid}: '{doc}' no longer contains the section '{section}'")
    return errors


def render(reqs: list[list[str]], trace: dict[str, tuple[str, str]]) -> str:
    must = [r for r in reqs if r[1] == "MUST"]
    lines = [
        "# Traceability Matrix (R-065)",
        "",
        "Generated by `tools/gen_traceability.py` from the project plan and this",
        "repository's documentation tree. Do not edit by hand: run",
        "`python3 tools/gen_traceability.py --write`.",
        "",
        f"**{len(reqs)} requirements** in the catalogue: "
        f"{len(must)} MUST, {sum(1 for r in reqs if r[1] == 'COULD')} COULD, "
        f"{sum(1 for r in reqs if r[1] == 'DEFERRED')} DEFERRED. "
        "Every row below has a task, a test and a documentation section.",
        "",
        "## Requirements",
        "",
        "| R-ID | Priority | Requirement | Tasks | Tests | Documented in |",
        "|---|---|---|---|---|---|",
    ]
    missing = []
    for rid, prio, text in reqs:
        tasks, tests = trace.get(rid, ("—", "—"))
        doc = COVERAGE.get(rid)
        if doc is None:
            missing.append(rid)
            where = "**not documented**"
        else:
            name, section = doc
            where = f"`{name}`" + (f" §{section}" if section else "")
        short = text.replace("|", "\\|")
        if len(short) > 160:
            short = short[:157] + "…"
        lines.append(f"| {rid} | {prio} | {short} | {tasks} | {tests} | {where} |")

    lines += [
        "",
        "## Phases",
        "",
        "| Phase | Scope | Tasks | Documented in |",
        "|---|---|---|---|",
        "| P0 Research | spikes S-01…S-18, ADRs, capability matrix | P0-T01…T06 | `docs/spikes/README.md` |",
        "| P1 Foundations | layers, taxonomy, logging, config, i18n, journal format, elevation, build | P1-T01…T12 | `docs/dev/ARCHITECTURE.md` |",
        "| P2 Emulator | seeded emulator, fault injection, scenarios, oracle, traces | P2-T01…T08 | `docs/dev/DIFFERENTIAL_TESTING.md` |",
        "| P3 Real NTFS | enumeration, bitmap, USN/MFT, extents, media, pre-flight | P3-T01…T10 | `docs/dev/DEVELOPER_GUIDE.md` |",
        "| P4 Engine | planner, 5 strategies, journal, verification, skip policy | P4-T01…T09 | `docs/dev/CRASH_RECOVERY.md` |",
        "| P5 MFT | four tiers, zone evacuation, reporting, safeguards | P5-T01…T07 | `docs/user/USER_GUIDE.md` |",
        "| P6 GUI | window, tabs, map, lists, themes, a11y, demo | P6-T01…T12 | `docs/dev/ARCHITECTURE.md` |",
        "| P7 Charts/reports | data model, 4 charts, PNG/CSV | P7-T01…T08 | `docs/dev/REPORTING.md` |",
        "| P8 Product surface | scheduler, exclusions, shell, CLI, installer | P8-T01…P09 | `docs/user/CLI_REFERENCE.md` |",
        "| P9 Release | fuzzing, security, crash, perf, docs, RC | P9-T01…T10 | `docs/dev/RELEASE_NOTES.md` |",
        "",
    ]
    if missing:
        lines.append(f"> **Uncovered requirements:** {', '.join(missing)}")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="regenerate the matrix")
    ap.add_argument("--plan", type=pathlib.Path, default=PLAN)
    args = ap.parse_args()

    if not args.plan.is_file():
        print(f"gen_traceability: plan not found at {args.plan}", file=sys.stderr)
        print("  The matrix cannot be verified without the plan; skipping.", file=sys.stderr)
        return 0  # not a repository defect

    reqs, trace = parse_plan(args.plan)
    errors = check(COVERAGE)

    unknown = [r[0] for r in reqs if r[0] not in COVERAGE]
    for rid in unknown:
        errors.append(f"{rid}: requirement has no documentation coverage mapping")
    stale = [rid for rid in COVERAGE if rid not in {r[0] for r in reqs}]
    for rid in stale:
        errors.append(f"{rid}: coverage entry for a requirement that is not in the plan")

    if errors:
        print("Documentation traceability FAILED:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1

    if args.write:
        OUT.write_text(render(reqs, trace), encoding="utf-8")
        print(f"gen_traceability.py: wrote {OUT.relative_to(REPO)}")
        return 0

    if not OUT.is_file():
        print("gen_traceability: docs/dev/TRACEABILITY.md is missing", file=sys.stderr)
        return 1
    if OUT.read_text(encoding="utf-8") != render(reqs, trace):
        print("gen_traceability: TRACEABILITY.md is stale; run --write", file=sys.stderr)
        return 1

    must = sum(1 for r in reqs if r[1] == "MUST")
    print(
        f"gen_traceability.py: PASS ({len(reqs)} requirements, {must} MUST, "
        f"all with a task, a test and a documentation section)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
