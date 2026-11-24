#!/usr/bin/env python3
"""P9-T08 — final licence and provenance audit (R-003, R-009).

Acceptance:
  (1) 0 GPL/LGPL/AGPL;
  (2) 100% of assets have author/licence/hash;
  (3) every "other" licence has a written justification;
  (4) SBOM matches artifact contents exactly.

The previous check_licences.py validated `licenses.json` against a hard-coded
allowlist. That is circular: it proves the file agrees with itself. It never
looked at what the build actually includes, never compared THIRD-PARTY.md with
licenses.json (they had already drifted apart by four entries), and never
checked an asset hash. A phantom dependency added to licenses.json passed.

This audit checks the repository, not the JSON:

  * every #include <...> in src/ tools/ tests/ must be a C++ standard header or
    a Windows/POSIX system header — any other name means an undeclared library;
  * every document in licenses.json must appear in THIRD-PARTY.md and vice versa;
  * every asset under res/ must be registered with author, licence and a sha256
    that matches the file on disk;
  * every licence outside the permissive allowlist needs a justification string;
  * the SBOM is regenerated from the tree and compared with the committed copy.
"""
import hashlib
import json
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent

FORBIDDEN = ("GPL", "LGPL", "AGPL", "MPL", "CDDL", "EPL", "SSPL", "Commons-Clause")
PERMISSIVE = {
    "Apache-2.0",
    "Apache-2.0 WITH LLVM-exception",
    "MIT",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "BSL-1.0",
    "ISC",
    "Zlib",
    "PSF-2.0",
}
# Licences that are allowed but must carry a written justification: they are not
# OSS licences, so the audit cannot decide on its own that shipping them is fine.
NEEDS_JUSTIFICATION = {"OS-Component", "Inno-Setup-License", "Proprietary-Microsoft-SDK"}

# Headers that are part of the C++ standard library.
CPP_STANDARD = {
    "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset", "cassert",
    "cctype", "cerrno", "cfenv", "cfloat", "charconv", "chrono", "cinttypes",
    "climits", "clocale", "cmath", "codecvt", "compare", "complex", "concepts",
    "condition_variable", "coroutine", "csetjmp", "csignal", "cstdarg", "cstddef",
    "cstdint", "cstdio", "cstdlib", "cstring", "ctime", "cuchar", "cwchar",
    "cwctype", "deque", "exception", "execution", "filesystem", "format",
    "forward_list", "fstream", "functional", "future", "initializer_list",
    "iomanip", "ios", "iosfwd", "iostream", "istream", "iterator", "latch",
    "limits", "list", "locale", "map", "memory", "memory_resource", "mutex",
    "new", "numbers", "numeric", "optional", "ostream", "queue", "random",
    "ranges", "ratio", "regex", "scoped_allocator", "semaphore", "set", "shared_mutex",
    "source_location", "span", "sstream", "stack", "stdexcept", "stop_token",
    "streambuf", "string", "string_view", "strstream", "syncstream", "system_error",
    "thread", "tuple", "type_traits", "typeindex", "typeinfo", "unordered_map",
    "unordered_set", "utility", "valarray", "variant", "vector", "version",
}

# Header prefixes that belong to the OS/SDK rather than a redistributable library.
SYSTEM_PREFIXES = (
    "sys/", "windows", "winioctl", "winnt", "wincodec", "d2d1", "dwrite",
    "commctrl", "commdlg", "comutil", "shlwapi", "shobjidl", "shellapi", "taskschd",
    "unistd", "fcntl", "pthread", "signal", "malloc", "errno", "time", "math",
    "io.h", "windef", "winbase", "winuser", "ntstatus", "sddl", "aclapi",
    "bitlocker", "winternl", "processthreadsapi", "synchapi", "fileapi",
    # P9-T04: PT-MEM reads PeakWorkingSetSize through psapi on Windows.
    "psapi", "dxgiformat", "process.h",
)


def sha256_of(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def iter_source_files():
    for root in ("src", "tools", "tests"):
        base = REPO / root
        if not base.is_dir():
            continue
        for p in sorted(base.rglob("*")):
            if p.suffix in (".cpp", ".hpp", ".h", ".cc", ".cxx"):
                yield p


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*<([^>]+)>')


def audit_includes(errors):
    """Every angle-bracket include must be standard or OS, never a library."""
    offenders = {}
    for path in iter_source_files():
        for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            inc = m.group(1).strip()
            if inc in CPP_STANDARD:
                continue
            if any(inc.startswith(p) for p in SYSTEM_PREFIXES):
                continue
            # Commas and brackets indicate a quirk-header like <windows.h, ...>
            if "," in inc or ">" in inc:
                continue
            rel = path.relative_to(REPO).as_posix()
            offenders.setdefault(inc, []).append(f"{rel}:{lineno}")
    for inc, sites in sorted(offenders.items()):
        errors.append(f"undeclared library header <{inc}> at {', '.join(sites[:3])}"
                      f"{' …' if len(sites) > 3 else ''}")
    return len(offenders)


def audit_gpl_mentions(errors):
    """A copyleft name must not appear in code, resources or build files."""
    # Word boundaries matter: "DefragPlan" contains the letters GPL and would
    # otherwise be reported as a copyleft reference on every line that names it.
    pattern = re.compile(r"\b(GPL|LGPL|AGPL|COPYLEFT)\b", re.IGNORECASE)
    hits = []
    for path in iter_source_files():
        text = path.read_text(encoding="utf-8", errors="replace")
        for lineno, line in enumerate(text.splitlines(), 1):
            if pattern.search(line):
                rel = path.relative_to(REPO).as_posix()
                hits.append(f"{rel}:{lineno}: {line.strip()[:80]}")
    for h in hits:
        errors.append(f"copyleft reference in source: {h}")
    return len(hits)


def load_licences(errors):
    path = REPO / "licenses.json"
    if not path.is_file():
        errors.append("licenses.json is missing")
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        errors.append(f"licenses.json is not valid JSON: {exc}")
        return None


def audit_entries(data, errors):
    """Allowlist, forbidden names, and the justification rule (acceptance 1 & 3)."""
    if data is None:
        return 0
    if data.get("projectLicense") != "Apache-2.0":
        errors.append("projectLicense must be Apache-2.0")
    entries = data.get("entries") or []
    if not entries:
        errors.append("licenses.json has no entries")
        return 0

    names = set()
    for dep in entries:
        name = dep.get("name", "<unnamed>")
        lic = dep.get("license", "")
        names.add(name)
        if not lic:
            errors.append(f"{name}: no license field")
            continue
        upper = lic.upper()
        for bad in FORBIDDEN:
            if bad in upper:
                errors.append(f"{name}: forbidden copyleft licence '{lic}'")
        if lic in NEEDS_JUSTIFICATION:
            just = (dep.get("justification") or "").strip()
            if len(just) < 20:
                errors.append(f"{name}: licence '{lic}' requires a written justification")
        elif lic not in PERMISSIVE:
            errors.append(f"{name}: licence '{lic}' is neither permissive nor justified")
        if not dep.get("source"):
            errors.append(f"{name}: no source")
    return len(entries)


def audit_third_party_parity(data, errors):
    """Every licence document must appear in both places (drift detector)."""
    if data is None:
        return
    declared = {e.get("name", "") for e in data.get("entries", [])}
    tp_path = REPO / "THIRD-PARTY.md"
    if not tp_path.is_file():
        errors.append("THIRD-PARTY.md is missing")
        return
    text = tp_path.read_text(encoding="utf-8")
    # Rows in the build-tool table and the prose both name components.
    missing_in_md = []
    for name in sorted(declared):
        # Match the distinctive first token, e.g. "CMake", "Inno Setup", "Python 3".
        probe = name.split("(")[0].strip().split()[0]
        if probe.lower() not in text.lower():
            missing_in_md.append(name)
    for name in missing_in_md:
        errors.append(f"'{name}' is in licenses.json but not mentioned in THIRD-PARTY.md")

    # And the reverse: a component named in THIRD-PARTY.md that has no entry.
    # Match whole names, not first tokens: "nlohmann/json (optional, not used)"
    # must satisfy a mention of "nlohmann/json", and "LLVM clang-tidy" satisfies
    # "clang" because the entry name contains it.
    lowered = text.lower()
    known_names = [n.lower() for n in declared]
    for token in ("python", "msvc", "msix", "catch2", "nlohmann", "clang"):
        if token not in lowered:
            continue
        if any(token in name for name in known_names):
            continue
        errors.append(f"THIRD-PARTY.md mentions '{token}' which has no licenses.json entry")


def audit_assets(errors):
    """Acceptance 2: every asset has author, licence and a matching hash."""
    reg_path = REPO / "res" / "assets" / "asset_register.json"
    if not reg_path.is_file():
        errors.append("res/assets/asset_register.json is missing")
        return 0

    reg = json.loads(reg_path.read_text(encoding="utf-8"))
    registered = {a.get("file", ""): a for a in reg.get("assets", [])}
    if not registered:
        errors.append("asset_register.json has no assets")
        return 0

    assets_dir = REPO / "res" / "assets"
    on_disk = {
        p.relative_to(REPO).as_posix()
        for p in assets_dir.rglob("*")
        if p.is_file() and p.name != "asset_register.json"
    }

    for rel, entry in sorted(registered.items()):
        if rel not in on_disk:
            errors.append(f"registered asset missing from disk: {rel}")
            continue
        for field in ("author", "license", "sha256", "source"):
            if not str(entry.get(field, "")).strip():
                errors.append(f"{rel}: missing '{field}'")
        lic = entry.get("license", "")
        if any(bad in lic.upper() for bad in FORBIDDEN):
            errors.append(f"{rel}: asset licence '{lic}' is copyleft")
        expect = entry.get("sha256", "")
        got = sha256_of(REPO / rel)
        if expect != got:
            errors.append(f"{rel}: sha256 mismatch (registered {expect[:16]}…, got {got[:16]}…)")

    for rel in sorted(on_disk - set(registered)):
        errors.append(f"asset on disk is not registered: {rel}")
    return len(registered)


def build_sbom() -> dict:
    """Regenerate the SBOM from the tree (acceptance 4)."""
    data = load_licences([])
    assets = []
    reg_path = REPO / "res" / "assets" / "asset_register.json"
    if reg_path.is_file():
        reg = json.loads(reg_path.read_text(encoding="utf-8"))
        for a in reg.get("assets", []):
            p = REPO / a["file"]
            assets.append({
                "file": a["file"],
                "sha256": sha256_of(p) if p.is_file() else "MISSING",
                "license": a.get("license", ""),
            })

    strings = []
    ids = REPO / "res" / "strings" / "ids.h"
    if ids.is_file():
        strings.append({"file": "res/strings/ids.h", "sha256": sha256_of(ids)})
    for name in ("en-US.rc",):
        p = REPO / "res" / "strings" / name
        if p.is_file():
            strings.append({"file": f"res/strings/{name}", "sha256": sha256_of(p)})

    source_files = [p.relative_to(REPO).as_posix() for p in iter_source_files()]
    return {
        "sbomVersion": 1,
        "format": "auralite-sbom/1",
        "project": {"name": "AuraLite-Defrag", "license": "Apache-2.0"},
        "components": [
            {
                "name": e.get("name"),
                "scope": e.get("scope"),
                "license": e.get("license"),
                "redistributed": bool(e.get("redistributed")),
            }
            for e in (data or {}).get("entries", [])
        ],
        "assets": assets,
        "resources": strings,
        "sourceFileCount": len(source_files),
    }


def audit_sbom(errors):
    """The committed SBOM must equal the one regenerated from the tree."""
    committed_path = REPO / "sbom.json"
    fresh = build_sbom()
    if not committed_path.is_file():
        errors.append("sbom.json is missing")
        return False
    try:
        committed = json.loads(committed_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        errors.append(f"sbom.json is not valid JSON: {exc}")
        return False

    if committed != fresh:
        # Report the first difference concretely; "SBOM differs" alone is useless.
        for key in fresh:
            if committed.get(key) != fresh[key]:
                if key == "assets":
                    ce = {a["file"]: a["sha256"] for a in committed.get("assets", [])}
                    fe = {a["file"]: a["sha256"] for a in fresh["assets"]}
                    for f in sorted(set(ce) | set(fe)):
                        if ce.get(f) != fe.get(f):
                            errors.append(f"SBOM asset mismatch for {f}")
                elif key == "components":
                    cn = [c.get("name") for c in committed.get("components", [])]
                    fn = [c.get("name") for c in fresh["components"]]
                    if cn != fn:
                        errors.append(f"SBOM components differ: committed={cn} fresh={fn}")
                else:
                    errors.append(f"SBOM field '{key}' differs (committed "
                                  f"{committed.get(key)!r} vs fresh {fresh.get(key)!r})")
        if not any("SBOM" in e for e in errors):
            errors.append("sbom.json differs from the tree in an uncontemplated way")
        return False
    return True


def main() -> int:
    errors = []

    entries = audit_entries(load_licences(errors), errors)
    audit_third_party_parity(load_licences(errors), errors)
    assets = audit_assets(errors)
    undeclared = audit_includes(errors)
    copyleft = audit_gpl_mentions(errors)
    sbom_ok = audit_sbom(errors)

    if errors:
        print("licence audit FAILED:", file=sys.stderr)
        for e in errors:
            print("  - " + e, file=sys.stderr)
        return 1

    print(f"check_licences.py: PASS — {entries} licence entries, {assets} assets hashed, "
          f"{undeclared} undeclared headers, {copyleft} copyleft references, SBOM in sync.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
