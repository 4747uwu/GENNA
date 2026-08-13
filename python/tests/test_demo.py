"""The demo is the first thing a new user runs, so it is tested like a feature.

It is also the only path that exercises Dataset curation, save, reopen,
provenance and rollback in one chain -- a break anywhere along it shows up
here rather than in somebody's terminal.

Standalone, like the rest of this suite: no pytest, so the gate can run it
with whatever interpreter it picked.
"""
from __future__ import annotations

import io
import contextlib
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import genna
from genna.demo import main as demo_main

FAILS, COUNT = [], 0


def check(cond, msg):
    global COUNT
    COUNT += 1
    if cond:
        print(f"  ok    {msg}")
    else:
        print(f"  FAIL  {msg}")
        FAILS.append(msg)


def run_demo(rows: int) -> tuple[int, str]:
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = demo_main(["--rows", str(rows)])
    return rc, buf.getvalue()


def main() -> int:
    print("-- the packaged demo --")
    rc, out = run_demo(800)
    check(rc == 0, f"genna-demo exits 0 (got {rc})")

    # The claim the demo exists to make. If v0 stops coming back byte-exact
    # the demo must FAIL, not print a cheerful wall of text.
    check("byte-identical to train.jsonl: True" in out,
          "the demo verifies v0 is byte-identical to the original file")

    for heading in ("A messy dataset arrives", "Curate it", "What it costs",
                    "Time travel", "Provenance", "Rollback", "Durability"):
        check(heading in out, f"section ran: {heading}")

    # Curation must actually remove something, or the generator has drifted
    # and the demo is presenting a no-op as a success.
    removed_lines = [ln for ln in out.splitlines() if "removed" in ln]
    total_removed = 0
    for ln in removed_lines:
        try:
            total_removed += int(ln.split("removed")[1].split()[0])
        except (IndexError, ValueError):
            pass
    check(total_removed > 0,
          f"curation removed records ({total_removed}) rather than no-oping")

    # Rollback must restore records, not merely append a version.
    try:
        before = int(out.split("before rollback:")[1].split("records")[0])
        after = int(out.split("after  rollback:")[1].split("records")[0])
    except (IndexError, ValueError):
        before = after = -1
    check(after > before, f"rollback restored records ({before} -> {after})")

    # A demo that litters temp directories is one people stop running.
    check("working directory kept" not in out,
          "the demo cleans up its working directory")

    print("\n-- runnable as documented --")
    r = subprocess.run([sys.executable, "-m", "genna.demo", "--rows", "400"],
                       capture_output=True, text=True, timeout=600)
    check(r.returncode == 0,
          f"`python -m genna.demo` works (exit {r.returncode})")
    check("Genna demo" in r.stdout, "and prints its banner")

    print("\n-- Dataset.touched_versions (used by the demo) --")
    ds = genna.Dataset.from_records(
        [{"id": i, "text": f"row {i}"} for i in range(200)])
    ds.drop_containing("row 7")
    got = ds.touched_versions(0, limit=16)
    check(isinstance(got, list) and all(isinstance(v, int) for v in got),
          f"returns a list of version indices ({got[:5]})")
    check(len(got) <= 16, f"respects limit=16 (got {len(got)})")

    n_ver = len(ds.versions)
    check(all(0 <= v < n_ver for v in got),
          f"every candidate is a real version index (< {n_ver})")

    raised = False
    try:
        ds.touched_versions(10 ** 6)
    except IndexError:
        raised = True
    check(raised, "an out-of-range record index raises IndexError")
    ds.engine.close()

    print(f"\n{'DEMO: FAILURES' if FAILS else 'DEMO: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
