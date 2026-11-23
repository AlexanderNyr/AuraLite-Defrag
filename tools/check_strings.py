#!/usr/bin/env python3
"""Verify clean-room terminology and string discipline across the repository."""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

FORBIDDEN_TERMS = [
    re.compile(r"\bdefraggler\b", re.IGNORECASE),
    re.compile(r"\bpiriform\b", re.IGNORECASE),
    re.compile(r"\bccleaner\b", re.IGNORECASE),
]

SCAN_DIRS = ["src", "res", "tests", "packaging", "docs/user", "docs/dev"]


def main() -> int:
    errors = []
    for sub in SCAN_DIRS:
        base = REPO / sub
        if not base.exists():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file() or p.suffix not in (".h", ".hpp", ".cpp", ".rc", ".manifest", ".xml", ".iss", ".md", ".json", ".txt"):
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            for pat in FORBIDDEN_TERMS:
                for m in pat.finditer(text):
                    rel = p.relative_to(REPO).as_posix()
                    errors.append(f"{rel}: forbidden clean-room term '{m.group(0)}'")

    # Verify res/strings/ids.h IDs match res/strings/en-US.rc
    ids_h = (REPO / "res/strings/ids.h").read_text(encoding="utf-8")
    rc_txt = (REPO / "res/strings/en-US.rc").read_text(encoding="utf-8")
    defined_ids = set(re.findall(r"#define\s+(IDS_[A-Z0-9_]+)\s+\d+", ids_h))
    rc_ids = set(re.findall(r"^\s*(IDS_[A-Z0-9_]+)\s+\"", rc_txt, re.MULTILINE))
    missing_in_rc = sorted(defined_ids - rc_ids)
    if missing_in_rc:
        errors.append(f"res/strings/en-US.rc missing IDS entries: {missing_in_rc}")

    if errors:
        print("Clean-room / string check FAILED:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        return 1
    print(f"check_strings.py: PASS (0 clean-room violations; {len(defined_ids)} IDS_* entries verified).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
