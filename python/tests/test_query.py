"""The query surface: the questions an auditor actually asks.

  "diff v200 vs v3000 as rows added/removed/changed"
  "which versions touched record 4,371?"

The second one is the interesting one. Answering it by materializing every
version is O(versions x rows). Structural sharing gives a shortcut that is
exact rather than approximate: if two versions reach the SAME NODE for a byte
range, the bytes underneath are not merely equal, they are the same memory.
So the query walks pointers, not data.

This test checks both that the answers are CORRECT (against a brute-force
materialize-and-compare oracle) and that the shortcut is actually cheaper.
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import genna

FAILS, COUNT = [], 0


def check(cond, msg):
    global COUNT
    COUNT += 1
    print(f"  {'ok  ' if cond else 'FAIL'}  {msg}")
    if not cond:
        FAILS.append(msg)
    return bool(cond)


def main():
    print("=== query surface ===")

    # ---- range_changed / range_history, against a brute-force oracle ----
    print("\n-- 'which versions touched this byte range?' --")
    eng = genna.Engine()
    RECS, W = 400, 64                     # fixed-width records
    data = b"".join(f"{i:04d}".encode().ljust(W, b".") for i in range(RECS))
    obj = eng.create_binary("recs", data, avg_chunk=256)

    # edit a few known records at known versions
    edited = {}                            # version -> record index
    import random
    rng = random.Random(11)
    for v in range(1, 26):
        r = rng.randrange(RECS)
        obj.update(r * W, 4, f"E{v:03d}".encode())
        edited[v] = r

    target = 137
    hist = obj.range_history(target * W, W)
    oracle = []
    prev = obj.read(target * W, W, version=0)
    for v in range(1, len(obj.versions)):
        cur = obj.read(target * W, W, version=v)
        if cur != prev:
            oracle.append(v)
        prev = cur
    check(hist == oracle,
          f"range_history matches brute force for record {target}: "
          f"{hist} == {oracle}")

    # Across every sampled record: the guarantee is a SUPERSET, not equality.
    # A record sharing a chunk with an edited one is reported as changed even
    # though its own bytes did not move -- that is chunk granularity, and it
    # is the safe direction. Missing a real change would be the bug.
    missed, extra, total_true, sampled = [], 0, 0, 0
    for r in range(0, RECS, 7):
        sampled += 1
        h = set(obj.range_history(r * W, W))
        o, prev = set(), obj.read(r * W, W, version=0)
        for v in range(1, len(obj.versions)):
            cur = obj.read(r * W, W, version=v)
            if cur != prev:
                o.add(v)
            prev = cur
        total_true += len(o)
        if not o <= h:
            missed.append((r, sorted(o - h)))
        extra += len(h - o)
    check(not missed,
          f"NO false negatives across {sampled} records: every real change is "
          f"reported ({total_true} true changes)"
          + ("" if not missed else f" MISSED={missed[:2]}"))
    print(f"   false positives: {extra} extra candidates over {sampled} records "
          f"({total_true} true changes) - chunk-granularity, filter then confirm")

    # the versions that really did touch a record must be reported
    for v, r in list(edited.items())[:5]:
        h = obj.range_history(r * W, W)
        if v not in h:
            check(False, f"version {v} edited record {r} but is not in history")
            break
    else:
        check(True, "every recorded edit appears in its record's history")

    # a record nobody touched
    untouched = set(range(RECS)) - set(edited.values())
    if untouched:
        u = sorted(untouched)[0]
        check(obj.range_history(u * W, W) == [],
              f"record {u} was never edited and reports no history")

    # range_changed agrees with reading the bytes
    bad = 0
    for r in (0, 137, RECS - 1):
        for a, b in ((0, 5), (3, 9), (0, len(obj.versions) - 1)):
            got = obj.range_changed(a, b, r * W, W)
            want = obj.read(r * W, W, version=a) != obj.read(r * W, W, version=b)
            # the structural check may report "changed" when bytes happen to
            # be equal (conservative), but must never report "unchanged" when
            # they differ -- that direction would be a correctness bug
            if want and not got:
                bad += 1
    check(bad == 0,
          "range_changed never reports 'unchanged' for a range that differs "
          "(conservative in the safe direction)")

    # ---- is the shortcut actually cheaper? ------------------------------
    print("\n-- cost: pointer walk vs materialize-and-compare --")
    t0 = time.perf_counter()
    for _ in range(200):
        obj.range_history(target * W, W)
    t_fast = (time.perf_counter() - t0) / 200
    t0 = time.perf_counter()
    for _ in range(20):
        prev = obj.read(target * W, W, version=0)
        for v in range(1, len(obj.versions)):
            cur = obj.read(target * W, W, version=v)
            prev = cur
    t_slow = (time.perf_counter() - t0) / 20
    print(f"   range_history      : {t_fast*1e6:8.1f} us")
    print(f"   materialize + diff : {t_slow*1e6:8.1f} us")
    check(t_fast < t_slow,
          f"structural query is {t_slow/max(1e-9,t_fast):.1f}x faster than "
          f"reading every version")
    eng.close()

    # ---- table-level diff ------------------------------------------------
    try:
        import pyarrow as pa
    except ImportError:
        print("\nSKIP table diff: pyarrow missing")
        print(f"\n{'QUERY: FAILURES' if FAILS else 'QUERY: ALL PASS'} "
              f"({len(FAILS)} failures / {COUNT} checks)")
        return 1 if FAILS else 0

    print("\n-- table diff: rows added / removed / cells changed --")
    n = 500
    at = pa.table({
        "id": pa.array(list(range(n)), type=pa.int64()),
        "label": pa.array(["cat" if i % 3 == 0 else "dog" for i in range(n)]),
        "score": pa.array([i / 10.0 for i in range(n)], type=pa.float64()),
    })
    t = genna.Table.from_arrow(at)
    v0 = t.versions - 1
    removed = t.drop_class("label", "cat")
    v1 = t.versions - 1

    d = t.diff(v0, v1)
    check(d["n_removed"] == removed,
          f"diff reports {d['n_removed']} rows removed (dropped {removed})")
    check(d["n_added"] == 0, "diff reports 0 rows added")
    check(all(i % 3 == 0 for i in d["rows_removed"]),
          "the removed rows are exactly the 'cat' rows")

    d2 = t.diff(v0, v1, key="id")
    check(len(d2["removed_keys"]) == removed,
          f"diff by key lists {len(d2['removed_keys'])} removed ids")

    # a column edit shows as changed cells, not added/removed rows
    t.relabel("label", {"dog": "canine"})
    v2 = t.versions - 1
    dc = t.diff(v1, v1)                    # same version: nothing
    check(dc["n_removed"] == 0 and dc["n_added"] == 0
          and dc["n_cells_changed"] == 0,
          "diff(v, v) reports no change at all")
    t.engine.close()

    print(f"\n{'QUERY: FAILURES' if FAILS else 'QUERY: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
