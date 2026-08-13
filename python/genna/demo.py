"""A complete, self-contained tour of Genna. `genna-demo` after installing.

Everything here runs offline in a temporary directory, generates its own data,
and cleans up after itself. It exists because the fastest way to understand a
versioned store is to watch one behave, and because a README claim you can
reproduce in eight seconds is worth more than one you cannot.

Nothing is faked. Every number printed is measured during the run, on the
machine you run it on -- so if a claim here is wrong on your hardware, you
will see it be wrong rather than read that it is right.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import sys
import tempfile
import time

import genna

# Plain ASCII on purpose: this is the first thing a new user runs, and a
# UnicodeEncodeError from a box-drawing character on a cp1252 Windows console
# is a terrible first impression.
RULE = "-" * 72


def _h(n: int, title: str) -> None:
    print("\n" + RULE)
    print("  %d. %s" % (n, title))
    print(RULE)


def _mb(n: float) -> str:
    return "%.2f MB" % (n / 1e6)


def _make_dataset(path: str, n: int) -> int:
    """A messy dataset, the way they actually arrive."""
    random.seed(7)
    rows = []
    topics = ["cats", "dogs", "boats", "weather", "cooking"]
    for i in range(n):
        rows.append({"id": i,
                     "text": "document %d discussing %s in some detail"
                             % (i, topics[i % len(topics)]),
                     "label": topics[i % len(topics)]})

    dupes = max(1, n // 18)
    for i in range(dupes):                       # exact duplicate records
        rows.append(dict(rows[i]))
    empties = max(1, n // 45)
    for i in range(empties):                     # empty text
        rows.append({"id": 900000 + i, "text": "", "label": "cats"})
    junk = max(1, n // 70)
    for i in range(junk):                        # boilerplate
        rows.append({"id": 910000 + i,
                     "text": "LOREM IPSUM PLACEHOLDER", "label": "cats"})

    random.shuffle(rows)
    with open(path, "wb") as f:
        for r in rows:
            f.write(json.dumps(r).encode() + b"\n")
    return len(rows)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="genna-demo",
        description="A guided tour of Genna. Generates its own data, "
                    "runs offline, cleans up after itself.")
    ap.add_argument("--rows", type=int, default=20000,
                    help="base records to generate (default 20000)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the working directory and print its path")
    args = ap.parse_args(argv)

    work = tempfile.mkdtemp(prefix="genna-demo-")
    raw = os.path.join(work, "train.jsonl")
    store = os.path.join(work, "train.genna")
    ok = True

    try:
        print(RULE)
        print("  Genna demo -- a versioned data store that edits in place")
        print("  version %s | %s | python %d.%d"
              % (getattr(genna, "__version__", "?"), sys.platform,
                 sys.version_info[0], sys.version_info[1]))
        print(RULE)

        # ------------------------------------------------------------ 1 ---
        _h(1, "A messy dataset arrives")
        t0 = time.time()
        total = _make_dataset(raw, args.rows)
        raw_size = os.path.getsize(raw)
        print("  wrote %s: %d records, %s  (%.1f s)"
              % (os.path.basename(raw), total, _mb(raw_size), time.time() - t0))
        print("  it contains duplicates, empty rows, and boilerplate.")

        # ------------------------------------------------------------ 2 ---
        _h(2, "Curate it -- records, not byte offsets")
        t0 = time.time()
        ds = genna.Dataset.from_jsonl(raw)
        print("  loaded %d records in %.2f s" % (len(ds), time.time() - t0))
        print()

        # Order matters, and not subtly: dedup() compares the `text` FIELD,
        # so running it first would collapse every empty row into one and
        # make the junk filters look like no-ops.
        plan = [("filter_length(min_bytes=60)", lambda: ds.filter_length(min_bytes=60)),
                ("drop_containing('LOREM IPSUM')", lambda: ds.drop_containing("LOREM IPSUM")),
                ("dedup()", lambda: ds.dedup())]
        steps = []
        for label, fn in plan:
            t0 = time.time()
            st = fn()
            steps.append((label, st))
            print("  %-32s removed %6d   v%d -> v%d   %6.0f ms"
                  % (label, st.removed, st.version_before, st.version_after,
                     (time.time() - t0) * 1000))

        print("\n  %d records left, in %d versions"
              % (len(ds), len(ds.versions)))
        print("  (every removed record is its own version -- that is what")
        print("   makes rollback fine-grained. Roll back to a Step, below.)")

        # ------------------------------------------------------------ 3 ---
        _h(3, "What it costs to keep all of that")
        t0 = time.time()
        ds.engine.save(store)
        save_ms = (time.time() - t0) * 1000
        store_size = os.path.getsize(store)

        # The honest comparison is against what a person actually does:
        # keep the raw file plus one copy after each curation step.
        naive = raw_size
        for _label, st in steps:
            naive += len(ds.version_bytes(st.version_after))

        print("  Genna store        %10s   %d versions, saved in %.0f ms"
              % (_mb(store_size), len(ds.versions), save_ms))
        print("  one raw copy       %10s   1 version" % _mb(raw_size))
        print("  4 files by hand    %10s   raw + one copy per step, which is"
              % _mb(naive))
        print("  %-18s %10s   what you would otherwise keep" % ("", ""))
        if store_size:
            print("\n  -> %.1fx smaller than keeping the 4 files, and it holds"
                  % (naive / store_size))
            print("     %d versions rather than 4." % len(ds.versions))
        if store_size >= naive:
            print("\n  NOTE: the store came out no smaller here. On data this")
            print("  small the fixed overhead dominates; try --rows 200000.")

        # ------------------------------------------------------------ 4 ---
        _h(4, "Time travel -- the original is still exactly the original")
        ds2 = genna.Dataset.open(store)
        print("  reopened from disk: %d records, %d versions"
              % (len(ds2), len(ds2.versions)))

        with open(raw, "rb") as f:
            original = f.read()
        t0 = time.time()
        v0 = ds2.version_bytes(0)
        read_ms = (time.time() - t0) * 1000
        exact = v0 == original
        print("  v0 read back in %.0f ms and is byte-identical to %s: %s"
              % (read_ms, os.path.basename(raw), exact))
        if not exact:
            ok = False
            print("  *** MISMATCH -- this is a bug, please report it ***")

        # ------------------------------------------------------------ 5 ---
        # Deliberately BEFORE the rollback: rollback appends a version and is
        # itself logged, so asking afterwards returns candidates that include
        # the rollback -- true, but confusing in a first demo.
        _h(5, "Provenance -- which versions touched this record?")
        idx = min(42, len(ds2) - 1)
        rec = ds2[idx]
        t0 = time.time()
        touched = ds2.touched_versions(idx, limit=64)
        q_ms = (time.time() - t0) * 1000
        print("  record #%d occupies bytes [%d, %d)"
              % (idx, rec.offset, rec.offset + len(rec)))
        print("  versions that changed it: %s"
              % (touched[:8] if touched else "none"))
        print("  answered in %.2f ms, without reading a single version." % q_ms)
        print("  (a superset: never misses a real change, may over-report)")

        # ------------------------------------------------------------ 6 ---
        _h(6, "Rollback -- undo a curation step")
        before = len(ds2)
        _label, last = steps[-1]
        ds2.rollback(last.version_before)
        print("  before rollback: %d records" % before)
        print("  after  rollback: %d records   (undid %s)"
              % (len(ds2), _label))
        print("  the rolled-back records are back verbatim, not regenerated.")
        print("  it is a NEW version too -- nothing was destroyed to undo.")

        # ------------------------------------------------------------ 7 ---
        _h(7, "Durability")
        print("  write-ahead log active: %s" % ds2.engine.wal_active)
        print("  log healthy (every record written and synced): %s"
              % ds2.engine.wal_healthy)
        print("  an edit that returned to its caller survives SIGKILL;")
        print("  see PERSISTENCE.md for what is and is not guaranteed.")

        # --------------------------------------------------------- close ---
        print("\n" + RULE)
        print("  Try it on your own data:")
        print(RULE)
        print("""
    import genna

    ds = genna.Dataset.from_jsonl("your_data.jsonl")
    ds.dedup()
    ds.filter_length(min_bytes=50)
    ds.engine.save("your_data.genna")

    ds2 = genna.Dataset.open("your_data.genna")
    ds2.version_bytes(0)      # the original, byte for byte
    ds2.rollback(step.version_before)
""")
        print("  Where it does NOT help: changes that touch everything")
        print("  (re-quantizing every row, full-precision checkpoints).")
        print("  README.md has the rule and the measurements.\n")

    finally:
        if args.keep:
            print("  working directory kept: %s" % work)
        else:
            shutil.rmtree(work, ignore_errors=True)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
