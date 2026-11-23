#!/usr/bin/env python3
"""Validate project license policy (Apache-2.0, zero GPL/LGPL/AGPL dependencies)."""
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

FORBIDDEN_LICENSES = {"GPL", "LGPL", "AGPL", "GPL-2.0", "GPL-3.0", "LGPL-2.1", "LGPL-3.0", "AGPL-3.0"}
ALLOWED_LICENSES = {
    "Apache-2.0",
    "Apache-2.0 WITH LLVM-exception",
    "MIT",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "BSL-1.0",
    "ISC",
    "Zlib",
    "OS-Component",
    "Inno-Setup-License",
}


def main() -> int:
    for req in ("LICENSE", "NOTICE", "THIRD-PARTY.md", "licenses.json"):
        if not (REPO / req).is_file():
            print(f"Missing required governance file: {req}", file=sys.stderr)
            return 1

    data = json.loads((REPO / "licenses.json").read_text(encoding="utf-8"))
    if data.get("projectLicense") != "Apache-2.0":
        print("projectLicense in licenses.json must be Apache-2.0", file=sys.stderr)
        return 1

    entries = data.get("entries", [])
    if not entries:
        print("licenses.json has no entries", file=sys.stderr)
        return 1

    for dep in entries:
        lic = dep.get("license", "")
        if any(bad in lic.upper() for bad in FORBIDDEN_LICENSES):
            print(f"Forbidden copyleft license in {dep}: {lic}", file=sys.stderr)
            return 1
        if lic not in ALLOWED_LICENSES:
            print(f"Non-allowlisted license in {dep}: {lic}", file=sys.stderr)
            return 1

    print(f"check_licences.py: PASS ({len(entries)} entries verified, Apache-2.0 compliant).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
