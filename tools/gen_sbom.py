#!/usr/bin/env python3
"""Regenerate sbom.json from the repository tree (P9-T08 acceptance 4).

The SBOM is derived, never hand-edited: every run recomputes asset hashes from
disk and re-reads licenses.json, so a modified asset or a new dependency shows
up as a diff instead of quietly diverging from the shipped artifact.
"""
import importlib.util
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent


def load_audit():
    spec = importlib.util.spec_from_file_location("cl", REPO / "tools" / "check_licences.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main() -> int:
    mod = load_audit()
    sbom = mod.build_sbom()
    target = REPO / "sbom.json"
    target.write_text(json.dumps(sbom, indent=2) + "\n", encoding="utf-8")
    print(f"gen_sbom.py: wrote {target.relative_to(REPO).as_posix()} — "
          f"{len(sbom['components'])} components, {len(sbom['assets'])} assets, "
          f"{len(sbom['resources'])} resources, {sbom['sourceFileCount']} source files.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
