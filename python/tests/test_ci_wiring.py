"""Fail when a check exists but can never run.

This repository's recurring bug is not "the tests are wrong". It is that a
check which never executed is indistinguishable from a check that passed.
The list so far includes a corrupt-store fuzzer that verified nothing, a
Python gate that skipped every file and exited 0, a link probe that could not
fail, and -- the largest -- a CI workflow triggered on `main` in a repository
whose default branch is `master`, which therefore never ran on any commit
while the repository looked fully covered.

That last one is the category in its purest form, and it is what this file
exists to make impossible. Every check here answers the same question about
some other check: CAN IT ACTUALLY FIRE?

Deliberately dependency-free, and it fails rather than skips when it cannot
parse something. A wiring test that silently gives up is the bug it is
looking for.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
WORKFLOWS = ROOT / ".github" / "workflows"

FAILS, COUNT = [], 0

# Test files intentionally not wired into CI. An exclusion must be listed
# HERE, in the open, with a reason -- so that not running something is a
# decision somebody made rather than an oversight nobody noticed.
CI_EXEMPT = {
    "test_semantic.py": "downloads a real sentence-transformer; not on every push",
    "smoke_wheel.py":   "run explicitly as the post-install smoke step",
    "test_ci_wiring.py": "this file; named in ci.yml as its own step",
}

# tests/*.c that are not tests. Benchmarks and helpers, so not running them in
# CI is correct rather than an oversight.
C_NOT_A_TEST = {
    "gitcmp":    "benchmark vs git",
    "rsynccmp":  "benchmark vs rsync",
    "realbench": "benchmark on real WikiText-2",
    "vbench":    "benchmark, NLE timeline workload",
    "deltabench": "benchmark, delta sizes",
    "readscale": "benchmark, read scaling",
    "leakcheck": "helper used by tools/run_leak.sh",
}

# REAL test suites that CI does not run. Not exempt -- a known gap, printed
# loudly on every run so it cannot be forgotten, and RATCHETED: this set may
# shrink, never grow. A newly added test that nothing executes fails the
# build, which is the whole point of this file. Wiring these in needs their
# harnesses from tools/run_*.sh and is a separate piece of work.
C_KNOWN_GAP = {
    "bin_test":        "needs tools/run_bin.sh harness",
    "concurrent_test": "needs tools/run_concurrent.sh harness",
    "scene_test":      "needs tools/run_scene.sh harness",
    "leak_test":       "needs tools/run_leak.sh + valgrind",
}


def check(cond, msg):
    global COUNT
    COUNT += 1
    print(f"  {'ok  ' if cond else 'FAIL'}  {msg}")
    if not cond:
        FAILS.append(msg)
    return bool(cond)


def git(*args):
    try:
        r = subprocess.run(["git", *args], cwd=str(ROOT), capture_output=True,
                           text=True, timeout=60)
        return r.stdout.strip() if r.returncode == 0 else None
    except Exception:
        return None


def push_branches(text: str):
    """Branches under `on: push: branches:` -- both YAML list forms.

    Returns None if the file has a push trigger whose branches cannot be
    parsed, which is treated as a failure rather than as "no branches".
    """
    m = re.search(r"^on:\s*$", text, re.M)
    if not m:
        return []
    # the `on:` block runs until the next top-level key
    rest = text[m.end():]
    end = re.search(r"^\S", rest, re.M)
    block = rest[:end.start()] if end else rest

    pm = re.search(r"^\s{2}push:\s*$", block, re.M)
    if not pm:
        return []
    prest = block[pm.end():]
    pend = re.search(r"^\s{0,2}\S", prest, re.M)
    pblock = prest[:pend.start()] if pend else prest

    bm = re.search(r"^\s*branches:\s*(.*)$", pblock, re.M)
    if not bm:
        return []                      # push on all branches
    inline = bm.group(1).strip()
    if inline.startswith("["):
        return [b.strip().strip("'\"") for b in
                inline.strip("[]").split(",") if b.strip()]
    if inline:
        return [inline.strip("'\"")]
    # block list form:  branches:\n    - master
    after = pblock[bm.end():]
    out = []
    for line in after.splitlines():
        if not line.strip():
            continue
        lm = re.match(r"^\s+-\s*(.+?)\s*$", line)
        if not lm:
            break
        out.append(lm.group(1).strip("'\""))
    return out or None


def main() -> int:
    print("=== CI wiring: can these checks actually fire? ===")

    wf = sorted(WORKFLOWS.glob("*.yml")) + sorted(WORKFLOWS.glob("*.yaml"))
    check(len(wf) > 0, f"found workflow files to inspect ({len(wf)})")

    # ---- 1. every push-trigger branch must exist -------------------------
    print("\n-- a trigger on a branch that does not exist never fires --")
    local = set((git("branch", "--format=%(refname:short)") or "").split())
    remote = set()
    for line in (git("branch", "-r", "--format=%(refname:short)") or "").split():
        remote.add(line.split("/", 1)[1] if "/" in line else line)
    known = local | remote
    check(len(known) > 0, f"resolved the repository's branches ({sorted(known)})")

    for f in wf:
        text = f.read_text(encoding="utf-8", errors="replace")
        br = push_branches(text)
        if br is None:
            check(False, f"{f.name}: has a push trigger whose branches could "
                         f"not be parsed - refusing to assume it is fine")
            continue
        if not br:
            print(f"  --    {f.name}: no branch filter (fires on all)")
            continue
        for b in br:
            if b in ("**", "*"):
                continue
            check(b in known,
                  f"{f.name}: push trigger on '{b}' - that branch exists")

    # ---- 2. every test file must be wired into CI ------------------------
    print("\n-- a test file CI never names is a test that never runs --")
    ci = WORKFLOWS / "ci.yml"
    check(ci.exists(), "ci.yml is present")
    ci_text = ci.read_text(encoding="utf-8", errors="replace") if ci.exists() else ""

    pytests = sorted(p.name for p in (ROOT / "python" / "tests").glob("*.py")
                     if p.name.startswith(("test_", "smoke_")))
    check(len(pytests) > 0, f"found Python test files ({len(pytests)})")
    for name in pytests:
        if name in CI_EXEMPT:
            print(f"  --    {name}: exempt ({CI_EXEMPT[name]})")
            continue
        check(name in ci_text,
              f"{name} is named in ci.yml")

    # ---- 3. every C test binary must be run ------------------------------
    print("\n-- and the same for the C engine --")
    mk = (ROOT / "Makefile").read_text(encoding="utf-8", errors="replace")
    ci_run = mk.split("ci-run:", 1)[1] if "ci-run:" in mk else ""
    ci_run = ci_run.split("\nclean:", 1)[0]
    c_srcs = sorted(p.stem for p in (ROOT / "tests").glob("*.c"))
    check(len(c_srcs) > 0, f"found C sources under tests/ ({len(c_srcs)})")

    unrun = []
    for stem in c_srcs:
        if stem in C_NOT_A_TEST:
            continue
        if re.search(rf"\./{re.escape(stem)}\b", ci_run):
            check(True, f"{stem} is executed by `make ci-run`")
        else:
            unrun.append(stem)

    # Ratchet. The known gap may shrink and never grow; anything newly
    # unrun is a failure, because a test nothing executes is the bug this
    # file exists to catch.
    for stem in unrun:
        if stem in C_KNOWN_GAP:
            print(f"  GAP   {stem}: NOT RUN BY CI ({C_KNOWN_GAP[stem]})")
    new_gaps = [s for s in unrun if s not in C_KNOWN_GAP]
    check(not new_gaps,
          f"no C test is unrun-and-unaccounted-for "
          f"(new: {new_gaps or 'none'})")
    closed = [s for s in C_KNOWN_GAP if s not in unrun]
    check(not closed,
          f"C_KNOWN_GAP has no stale entries - remove {closed} now that CI "
          f"runs them" if closed else
          "C_KNOWN_GAP has no stale entries (ratchet is honest)")
    print(f"  --    {len(unrun)} C test suites CI does not run: "
          f"{sorted(unrun)}")

    # ---- 4. the workflow must have a chance to gate a merge --------------
    print("\n-- and the default branch must be one CI fires on --")
    head = git("rev-parse", "--abbrev-ref", "HEAD")
    if ci.exists():
        br = push_branches(ci_text)
        if br:
            check(head in br,
                  f"the checked-out branch '{head}' is in ci.yml's push "
                  f"triggers {br} - pushing here runs CI")

    print(f"\n{'CI WIRING: FAILURES' if FAILS else 'CI WIRING: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    print(f"VERDICT: {'FAIL' if FAILS else 'PASS'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
