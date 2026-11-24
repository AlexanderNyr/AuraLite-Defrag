#!/usr/bin/env python3
"""Negative tests for the licence audit (P9-T08).

A compliance check that always passes is worse than no check, because it is
believed. The previous check_licences.py accepted a phantom dependency added to
licenses.json — it only ever compared the file with itself.

Each case below breaks one thing and asserts that the audit notices. Everything
is restored afterwards even if a case raises, so a failure here cannot leave the
tree dirty.
"""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
AUDIT = REPO / "tools" / "check_licences.py"

passed = 0
failed = 0


def run_audit() -> tuple[bool, str]:
    r = subprocess.run([sys.executable, str(AUDIT)], capture_output=True, text=True, cwd=REPO)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def check(name: str, expect_pass: bool, must_mention: str = "") -> None:
    global passed, failed
    ok, out = run_audit()
    good = (ok == expect_pass) and (must_mention in out if must_mention else True)
    if good:
        passed += 1
        print(f"  [PASS] {name}")
    else:
        failed += 1
        print(f"  [FAIL] {name}: audit {'passed' if ok else 'failed'}, expected "
              f"{'pass' if expect_pass else 'fail'}")
        for line in out.splitlines()[:6]:
            print("        " + line)


def with_backup(path: pathlib.Path):
    """Context manager that restores a file or directory even on exception."""
    class _Ctx:
        def __enter__(self):
            self.tmp = pathlib.Path(tempfile.mkdtemp()) / path.name
            if path.is_dir():
                shutil.copytree(path, self.tmp)
            else:
                shutil.copy2(path, self.tmp)
            return path

        def __exit__(self, *exc):
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
                shutil.copytree(self.tmp, path)
            else:
                shutil.copy2(self.tmp, path)
            shutil.rmtree(self.tmp.parent, ignore_errors=True)
            return False

    return _Ctx()


def main() -> int:
    print("Licence audit negative tests (P9-T08)")

    check("baseline: clean tree passes", True)

    # 1. A real third-party header appearing in the build.
    cfg = REPO / "src" / "core" / "config.cpp"
    with with_backup(cfg):
        original = cfg.read_text(encoding="utf-8")
        cfg.write_text('#include <nlohmann/json.hpp>\n' + original, encoding="utf-8")
        check("undeclared library include is caught", False, "undeclared library header")

    # 2. A tampered asset (hash must mismatch).
    logo = REPO / "res" / "assets" / "auralite_logo.svg"
    with with_backup(logo):
        logo.write_bytes(logo.read_bytes() + b"x")
        check("tampered asset hash is caught", False, "sha256 mismatch")

    # 3. An unregistered file dropped into the asset directory.
    extra = REPO / "res" / "assets" / "unregistered.png"
    try:
        extra.write_bytes(b"\x89PNG\r\n\x1a\n")
        check("unregistered asset on disk is caught", False, "not registered")
    finally:
        extra.unlink(missing_ok=True)

    # 4. A copyleft dependency added to licenses.json.
    lic = REPO / "licenses.json"
    with with_backup(lic):
        data = json.loads(lic.read_text(encoding="utf-8"))
        data["entries"].append({
            "name": "libcopyleft", "scope": "runtime", "license": "GPL-3.0",
            "source": "invented", "redistributed": True,
        })
        lic.write_text(json.dumps(data, indent=2), encoding="utf-8")
        check("copyleft licence is caught", False, "forbidden copyleft")

    # 5. A non-OSS licence without justification.
    with with_backup(lic):
        data = json.loads(lic.read_text(encoding="utf-8"))
        data["entries"].append({
            "name": "Mystery SDK", "scope": "build", "license": "OS-Component",
            "source": "invented", "redistributed": False,
        })
        lic.write_text(json.dumps(data, indent=2), encoding="utf-8")
        check("unjustified non-OSS licence is caught", False, "requires a written justification")

    # 6. Drift between THIRD-PARTY.md and licenses.json.
    tp = REPO / "THIRD-PARTY.md"
    with with_backup(tp):
        text = tp.read_text(encoding="utf-8")
        tp.write_text(text.replace("| CMake |", "| CMakeX |"), encoding="utf-8")
        lit = REPO / "licenses.json"
        with with_backup(lit):
            data = json.loads(lit.read_text(encoding="utf-8"))
            data["entries"].append({
                "name": "GhostLib", "scope": "build", "license": "MIT",
                "source": "invented", "redistributed": False,
            })
            lit.write_text(json.dumps(data, indent=2), encoding="utf-8")
            check("THIRD-PARTY.md drift is caught", False, "not mentioned in THIRD-PARTY.md")

    # 7. A stale SBOM.
    sbom = REPO / "sbom.json"
    with with_backup(sbom):
        data = json.loads(sbom.read_text(encoding="utf-8"))
        data["sourceFileCount"] = 1
        sbom.write_text(json.dumps(data, indent=2), encoding="utf-8")
        check("stale SBOM is caught", False, "SBOM")

    # 8. A copyleft word in source code.
    probe = REPO / "src" / "core" / "zzz_probe.cpp"
    try:
        probe.write_text("// This file is GPL licensed.\n", encoding="utf-8")
        check("copyleft mention in source is caught", False, "copyleft reference")
    finally:
        probe.unlink(missing_ok=True)

    # Everything restored.
    check("tree restored, audit passes again", True)

    print(f"\nSummary: {passed}/{passed + failed} passed.")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
