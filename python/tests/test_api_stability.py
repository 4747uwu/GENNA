"""Every public name is marked, and every stable name is tested.

"Stable" is a promise. This is what stands behind it, because a promise with
nothing behind it is worse than no promise at all -- it is the same shape as
a green suite that ran nothing.

Three things are enforced:

  1. No unmarked public name. Adding to __all__ without deciding stable or
     unstable fails here, at the moment of the addition.
  2. No stale marks. A name removed from __all__ but left in __stability__
     means the table is drifting away from the thing it describes.
  3. Nothing marked stable without a test exercising it. This is the one that
     costs something, and it is the only one that makes the word mean
     anything.
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import genna

FAILS, COUNT = [], 0
VALID = {"stable", "unstable"}


def check(cond, msg):
    global COUNT
    COUNT += 1
    print(f"  {'ok  ' if cond else 'FAIL'}  {msg}")
    if not cond:
        FAILS.append(msg)
    return bool(cond)


def test_sources() -> dict[str, str]:
    """Every test file's text, excluding this one.

    This file naturally mentions every name, so counting itself as coverage
    would make check 3 pass unconditionally -- exactly the vacuity it exists
    to prevent.
    """
    out = {}
    for p in sorted(HERE.glob("*.py")):
        if p.resolve() == Path(__file__).resolve():
            continue
        out[p.name] = p.read_text(encoding="utf-8", errors="replace")
    return out


def main() -> int:
    print("=== public API stability marks ===\n")

    api = list(genna.__all__)
    marks = dict(genna.__stability__)
    check(len(api) > 0, f"the package exports something ({len(api)} names)")

    print("\n-- every public name is marked --")
    unmarked = [n for n in api if n not in marks]
    check(not unmarked,
          f"no unmarked public name (missing: {unmarked or 'none'})")

    bad = {n: v for n, v in marks.items() if v not in VALID}
    check(not bad, f"every mark is one of {sorted(VALID)} (bad: {bad or 'none'})")

    print("\n-- the table does not drift from __all__ --")
    stale = [n for n in marks if n not in api]
    check(not stale,
          f"no mark for a name that is no longer exported (stale: {stale or 'none'})")

    print("\n-- anything marked stable is exercised by a test --")
    sources = test_sources()
    check(len(sources) > 1,
          f"found other test files to search ({len(sources)})")

    stable = sorted(n for n, v in marks.items() if v == "stable")
    check(len(stable) > 0, f"something is actually claimed stable ({len(stable)})")

    uncovered = []
    coverage = {}
    for name in stable:
        if name == "__version__":
            hits = [f for f, t in sources.items() if "__version__" in t]
        elif name == "open":
            # bare `open` is ambiguous; look for the qualified form
            pat = re.compile(r"genna\.open\s*\(")
            hits = [f for f, t in sources.items() if pat.search(t)]
        else:
            pat = re.compile(rf"\b{re.escape(name)}\b")
            hits = [f for f, t in sources.items() if pat.search(t)]
        coverage[name] = hits
        if not hits:
            uncovered.append(name)

    for name in stable:
        n = len(coverage[name])
        where = ", ".join(sorted(coverage[name])[:3])
        print(f"        {name:16s} {n:>2d} file(s)  {where}")

    check(not uncovered,
          f"every stable name appears in at least one test "
          f"(uncovered: {uncovered or 'none'})")

    print("\n-- the C header marks its surface too --")
    hdr = (HERE.parent.parent / "include" / "genna.h").read_text(
        encoding="utf-8", errors="replace")
    check("API STABILITY" in hdr,
          "include/genna.h carries a stability section")
    check("STABLE" in hdr and "UNSTABLE" in hdr,
          "and names both categories")
    check("it is UNSTABLE" in hdr,
          "and states the default for anything unlisted")

    print(f"\n{'API: FAILURES' if FAILS else 'API: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    print(f"VERDICT: {'FAIL' if FAILS else 'PASS'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
