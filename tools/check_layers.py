#!/usr/bin/env python3
"""Enforce L0-L6 upward-only dependency rules across src/."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent / "src"

# Layer assignments by top-level directory under src/
LAYER = {
    "core": 0,
    "interfaces": 0,
    "platform": 1,
    "backend": 2,
    "engine": 4,      # contains L3 (online_mode) + L4 (planner, journal, policy_gate, controller)
    "reporting": 5,
    "differential": 5,
    "ui": 6,
    "cli": 6,
    "scheduler": 6,
    "shell": 6,
    "app": 6,
}

# Allowed same-layer or cross-layer exceptions matching the master plan:
# - backend (L2) implements interfaces (L0) and uses platform (L1) + core (L0)
# - engine (L3/L4) uses interfaces (L0) and core (L0) ONLY (never backend or platform directly!)
# - reporting (L5) uses interfaces (L0) and core (L0)
# - ui/cli/app (L6) wire concrete L2/L4/L5/L1 components
INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def module_of(rel_path: str) -> str:
    return rel_path.split("/")[0]


def main() -> int:
    violations = []
    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in (".h", ".hpp", ".cpp"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        src_mod = module_of(rel)
        src_layer = LAYER.get(src_mod)
        if src_layer is None:
            continue
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            inc = m.group(1)
            if inc.startswith("../"):
                # Relative include to res/strings/ids.h is allowed from L6/L0
                if "res/strings/ids.h" in inc and src_layer == 6:
                    continue
                violations.append(f"{rel}:{lineno}: relative parent include '{inc}'")
                continue
            inc_mod = module_of(inc)
            dst_layer = LAYER.get(inc_mod)
            if dst_layer is None:
                continue
            if dst_layer > src_layer:
                violations.append(
                    f"{rel}:{lineno}: L{src_layer} ({src_mod}) must not depend upward on L{dst_layer} ({inc_mod})"
                )
            # Strict decoupling: engine (L3/L4) must NEVER depend directly on backend (L2) or platform (L1)
            if src_mod == "engine" and inc_mod in ("backend", "platform"):
                violations.append(
                    f"{rel}:{lineno}: engine (L3/L4) must depend on interfaces, not '{inc}'"
                )
            # Strict decoupling: reporting (L5) must NEVER depend on backend, engine, or platform
            if src_mod in ("reporting", "differential") and inc_mod in ("backend", "engine", "platform"):
                violations.append(
                    f"{rel}:{lineno}: reporting (L5) must depend on interfaces/core, not '{inc}'"
                )

    if violations:
        print("Layering violations detected:", file=sys.stderr)
        for v in violations:
            print("  " + v, file=sys.stderr)
        return 1
    print("check_layers.py: PASS (all dependencies obey L0-L6 upward-only DAG).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
