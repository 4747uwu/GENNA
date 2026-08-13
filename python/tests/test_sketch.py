"""Versioned cardinality: distinct-count for ANY historical version, O(1).

The sketch is stored beside each table version, so "how many distinct labels
did this column have at version 1,847?" is a lookup, not a scan of a
reconstructed version.

Two things have to be true for that to be worth anything, and both are tested:
  1. The estimate is close to the truth (checked against exact counts).
  2. Looking it up is actually cheaper than recomputing it.
And one thing has to be admitted: the sketch costs an O(n) pass at write time.
"""
from __future__ import annotations

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import genna
from genna.sketch import HLL

FAILS, COUNT = [], 0


def check(cond, msg):
    global COUNT
    COUNT += 1
    print(f"  {'ok  ' if cond else 'FAIL'}  {msg}")
    if not cond:
        FAILS.append(msg)
    return bool(cond)


def main():
    print("=== versioned sketches ===")
    try:
        import pyarrow as pa
    except ImportError:
        print("SKIP: pyarrow not installed")
        return 0

    # ---- the estimator itself -------------------------------------------
    print("\n-- HLL accuracy vs exact (1 KB of registers) --")
    worst = 0.0
    for true_n in (100, 1_000, 10_000, 100_000):
        est = HLL().add_many(f"v{i}" for i in range(true_n)).estimate()
        err = abs(est - true_n) / true_n
        worst = max(worst, err)
        print(f"   true {true_n:>7,}  est {est:>7,}  err {err*100:5.2f}%")
    check(worst < 0.06, f"worst relative error {worst*100:.2f}% is within the "
                        f"~3-5% expected at p=10")

    a = HLL().add_many(range(5000))
    b = HLL().add_many(range(2500, 7500))
    m = a.merge(b).estimate()
    check(abs(m - 7500) / 7500 < 0.06,
          f"merge is a union: {m:,} vs true 7,500 "
          f"({abs(m-7500)/7500*100:.1f}%) - mergeable, so it composes")

    # ---- versioned lookup -------------------------------------------------
    print("\n-- distinct at any historical version --")
    n = 20000
    at = pa.table({
        "id": pa.array(list(range(n)), type=pa.int64()),
        "label": pa.array([f"c{i % 400}" for i in range(n)]),
    })
    t = genna.Table.from_arrow(at).sketches(True)   # opt-in
    v0 = t.versions - 1
    e0 = t.distinct("label", v0)
    x0 = t.distinct_exact("label", v0)
    check(abs(e0 - x0) / x0 < 0.08,
          f"v{v0}: estimated {e0} distinct labels vs exact {x0}")

    # collapse the label space, then ask about BOTH versions
    t.relabel_lazy("label", {f"c{i}": "merged" for i in range(200)})
    v1 = t.versions - 1
    e1, x1 = t.distinct("label", v1), t.distinct_exact("label", v1)
    check(abs(e1 - x1) / max(1, x1) < 0.10,
          f"v{v1} after collapsing 200 labels: estimated {e1} vs exact {x1}")
    check(e1 < e0, f"cardinality dropped across the edit ({e0} -> {e1})")

    # the old version is still answerable, unchanged
    check(t.distinct("label", v0) == e0,
          "the older version's sketch is unchanged by the later edit")

    # ---- is the lookup actually cheaper? ---------------------------------
    print("\n-- lookup vs recompute --")
    t0 = time.perf_counter()
    for _ in range(50):
        t.distinct("label", v0)
    t_look = (time.perf_counter() - t0) / 50
    t0 = time.perf_counter()
    for _ in range(5):
        t.distinct_exact("label", v0)
    t_scan = (time.perf_counter() - t0) / 5
    print(f"   sketch lookup : {t_look*1000:7.3f} ms")
    print(f"   exact rescan  : {t_scan*1000:7.3f} ms")
    check(t_look < t_scan,
          f"lookup is {t_scan/max(1e-9,t_look):.0f}x cheaper than rescanning "
          f"the historical version")

    # ---- and what it costs at write time ----------------------------------
    print("\n-- what the sketch costs on write --")
    t2 = genna.Table.from_arrow(at)
    t0 = time.perf_counter()
    for i in range(5):
        t2.relabel_lazy("label", {f"c{i}": f"z{i}"})
    t_off = (time.perf_counter() - t0) / 5
    t3 = genna.Table.from_arrow(at).sketches(True)
    t0 = time.perf_counter()
    for i in range(5):
        t3.relabel_lazy("label", {f"c{i}": f"z{i}"})
    t_on = (time.perf_counter() - t0) / 5
    print(f"   commit without sketch: {t_off*1000:7.2f} ms")
    print(f"   commit with sketch   : {t_on*1000:7.2f} ms")
    check(t_on > t_off,
          f"the sketch is not free at write time ({t_on/max(1e-9,t_off):.1f}x) "
          f"- it moves cost from read to write")

    t.engine.close(); t2.engine.close(); t3.engine.close()
    print(f"\n{'SKETCH: FAILURES' if FAILS else 'SKETCH: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
