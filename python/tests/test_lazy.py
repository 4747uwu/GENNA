"""Deferred transforms: does writing the RULE beat writing the DATA?

The claim under test is specific and falsifiable: relabeling a column should
cost bytes proportional to the *description* of the change, not to the number
of rows it touches. The eager path wrote 31,368 B for 10,000 values; if the
lazy path is not orders of magnitude smaller, it was not worth building.

The cost it charges instead -- read amplification, since every pending op is
replayed on every read -- is measured too, because a write optimization that
quietly makes reads unusable is not an optimization.
"""
from __future__ import annotations

import os
import sys
import tempfile
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


def make(n=20000):
    import pyarrow as pa
    labels = ["cat", "dog", "bird", "fish"]
    return pa.table({
        "id": pa.array(list(range(n)), type=pa.int64()),
        "label": pa.array([labels[i % 4] for i in range(n)]),
        "score": pa.array([float(i % 100) for i in range(n)], type=pa.float64()),
    })


def size(t, tmp, tag):
    p = os.path.join(tmp, f"{tag}.gn")
    t.save(p)
    return os.path.getsize(p)


def main():
    print("=== deferred transforms (lazy tags) ===")
    try:
        import pyarrow as pa
    except ImportError:
        print("SKIP: pyarrow not installed")
        return 0

    tmp = tempfile.mkdtemp(prefix="gn_lazy_")
    at = make()
    n = at.num_rows

    # ---- correctness first ---------------------------------------------
    print("\n-- correctness: lazy must equal eager, exactly --")
    eager = genna.Table.from_arrow(make())
    lazy = genna.Table.from_arrow(make())
    eager.relabel("label", {"cat": "feline", "dog": "canine"})
    lazy.relabel_lazy("label", {"cat": "feline", "dog": "canine"})
    check(eager.column("label").to_pylist() == lazy.column("label").to_pylist(),
          "lazy relabel produces byte-identical values to the eager rewrite")
    check(lazy.pending_ops("label") == 1, "one pending op recorded")

    # numeric range add
    lazy.add_range("score", 5000, 15000, 2.5)
    got = lazy.column("score", live_only=False).to_pylist()
    want = [float(i % 100) + (2.5 if 5000 <= i < 15000 else 0.0)
            for i in range(n)]
    check(got == want, "add_range applies to exactly rows [5000,15000)")
    check(lazy.pending_ops("score") == 1, "range add is one op")

    # ops compose in order
    lazy.add_range("score", 0, n, 1.0)
    got2 = lazy.column("score", live_only=False).to_pylist()
    check(got2 == [w + 1.0 for w in want], "a second op composes on the first")
    check(lazy.pending_ops("score") == 2, "two pending ops")

    # ---- the measurement -------------------------------------------------
    print("\n-- cost of the write --")
    e2 = genna.Table.from_arrow(make())
    l2 = genna.Table.from_arrow(make())
    base_e = size(e2, tmp, "e_base")
    base_l = size(l2, tmp, "l_base")

    e2.relabel("label", {"cat": "feline", "dog": "canine"})
    after_e = size(e2, tmp, "e_after")

    wrote = l2.relabel_lazy("label", {"cat": "feline", "dog": "canine"})
    after_l = size(l2, tmp, "l_after")

    d_e, d_l = after_e - base_e, after_l - base_l
    print(f"   eager relabel : {base_e:,} -> {after_e:,}  (+{d_e:,} B)")
    print(f"   lazy  relabel : {base_l:,} -> {after_l:,}  (+{d_l:,} B, "
          f"op itself {wrote} B)")
    check(d_l < d_e,
          f"lazy write is smaller: {d_l:,} vs {d_e:,} B "
          f"({d_e/max(1,d_l):.1f}x)")
    check(wrote < 200, f"the op itself is {wrote} bytes, independent of the "
                       f"{n//2:,} rows it affects")

    # the real point: cost does not scale with rows affected
    print("\n-- does the write scale with rows affected? --")
    sizes = []
    for rows in (1000, 10000, 20000):
        t = genna.Table.from_arrow(make(rows))
        b = size(t, tmp, f"s{rows}")
        t.relabel_lazy("label", {"cat": "X"})
        a_ = size(t, tmp, f"s{rows}a")
        sizes.append((rows, a_ - b))
        t.engine.close()
    for rows, d in sizes:
        print(f"   {rows:>6,} rows -> +{d:,} B")
    # The claim is sub-linearity, so assert sub-linearity. The old check was
    # `spread < 500 B`, an absolute threshold tuned on one machine -- it read
    # 428 B here, so any platform whose chunk boundaries fell differently
    # would trip it while the underlying property still held. Cost per
    # affected row is the thing that must collapse as rows grow.
    by_rows = dict(sizes)
    per_row_1k = by_rows[1000] / 1000
    per_row_20k = by_rows[20000] / 20000
    ratio = by_rows[20000] / max(1, by_rows[1000])
    print(f"   per affected row: {per_row_1k:.4f} B at 1k -> "
          f"{per_row_20k:.4f} B at 20k")
    check(ratio < 5,
          f"20x the rows costs {ratio:.1f}x the bytes, not 20x "
          f"- sub-linear in rows affected")
    check(per_row_20k < per_row_1k / 2,
          f"cost per affected row falls as rows grow "
          f"({per_row_1k:.4f} -> {per_row_20k:.4f} B/row)")

    # ---- what it charges instead ----------------------------------------
    print("\n-- what the deferral costs: read amplification --")
    t = genna.Table.from_arrow(make())

    def best_of(fn, n=9):
        """Fastest of n runs.

        A single perf_counter sample of a sub-millisecond read is mostly
        scheduler noise. On this developer's Windows box the one-shot version
        happened to order correctly; on every Linux and macOS CI runner it did
        not, and t/lazy was the only red test in the matrix.

        Taking the MINIMUM is the fix rather than adding a tolerance: the
        floor of many samples is the closest thing to the true cost, and it
        keeps the assertion exactly as strong as it was. If replaying 16
        pending ops genuinely did not cost anything, this would still fail --
        which is the point.
        """
        best = float("inf")
        for _ in range(n):
            t0 = time.perf_counter()
            fn()
            best = min(best, time.perf_counter() - t0)
        return best

    read = lambda: t.column("label", live_only=False)  # noqa: E731
    t_clean = best_of(read)
    for i in range(16):
        t.relabel_lazy("label", {"cat": f"c{i}"})
    t_dirty = best_of(read)
    print(f"   read with 0 pending ops : {t_clean*1000:7.2f} ms")
    print(f"   read with 16 pending ops: {t_dirty*1000:7.2f} ms  "
          f"({t_dirty/max(1e-9,t_clean):.1f}x)")
    check(t_dirty > t_clean,
          "reads do get slower with pending ops - this is the trade, not free")

    folded = t.compact_column("label")
    t_comp = best_of(read)
    print(f"   after compact_column()  : {t_comp*1000:7.2f} ms "
          f"({folded} ops folded)")
    check(folded == 16, f"compact folded all {folded} ops")
    check(t_comp < t_dirty, "compaction restores read speed")
    check(t.pending_ops("label") == 0, "no ops pending after compaction")

    # ---- no hidden dependencies -----------------------------------------
    # This is the check that would have caught the bug that made t/lazy the
    # only red test in the matrix. `arr.type.to_pandas_dtype()` imports
    # pandas inside pyarrow; pandas is not declared anywhere in pyproject.
    # The dev box had it installed ambiently, so the suite was green here and
    # ModuleNotFoundError on all six CI jobs.
    #
    # Setting sys.modules["pandas"] = None makes `import pandas` raise
    # ImportError, which is the state of every clean install. If the lazy
    # path ever reaches for pandas again, this fails on the machine that
    # wrote the regression rather than an hour later in CI.
    print("\n-- the lazy path must not need pandas --")
    had_pandas = "pandas" in sys.modules
    saved = sys.modules.get("pandas")
    sys.modules["pandas"] = None       # forces ImportError on `import pandas`
    try:
        tp = genna.Table.from_arrow(make(400))
        tp.add_range("score", 0, 200, 5.0)
        tp.relabel_lazy("label", {"cat": "feline"})
        got_s = tp.column("score", live_only=False).to_pylist()
        got_l = tp.column("label", live_only=False).to_pylist()
        ok = True
    except ImportError as e:
        ok = False
        print(f"        reached for a module that is not a dependency: {e}")
    finally:
        if had_pandas:
            sys.modules["pandas"] = saved
        else:
            sys.modules.pop("pandas", None)
    check(ok, "numeric and remap ops materialize with pandas unimportable")
    if ok:
        check(got_s[0] == 5.0 and got_s[300] == 300.0 % 100,
              "and the range add is still numerically right without pandas")
        check("feline" in got_l and "cat" not in got_l,
              "and the remap is still right without pandas")
        tp.engine.close()
    # Informational, so it is a print and not a check. `check(x or True, ...)`
    # can never fail, and a check that cannot fail is the thing this file is
    # here to stop.
    print(f"        (pandas ambiently importable on this machine: {had_pandas})")

    # ---- persistence -----------------------------------------------------
    print("\n-- pending ops survive save/reopen --")
    t2 = genna.Table.from_arrow(make(500))
    t2.relabel_lazy("label", {"cat": "feline"})
    t2.add_range("score", 0, 250, 10.0)
    want_l = t2.column("label", live_only=False).to_pylist()
    want_s = t2.column("score", live_only=False).to_pylist()
    p = os.path.join(tmp, "lazy.gn")
    t2.save(p)
    t2.engine.close()
    t3 = genna.Table.open(p)
    check(t3.column("label", live_only=False).to_pylist() == want_l,
          "deferred label remap survives reopen")
    check(t3.column("score", live_only=False).to_pylist() == want_s,
          "deferred range add survives reopen")
    check(t3.pending_ops("label") == 1, "the op log came back too")
    t3.engine.close()

    eager.engine.close(); lazy.engine.close()
    e2.engine.close(); l2.engine.close(); t.engine.close()
    for f in os.listdir(tmp):
        try:
            os.remove(os.path.join(tmp, f))
        except OSError:
            pass
    os.rmdir(tmp)

    print(f"\n{'LAZY: FAILURES' if FAILS else 'LAZY: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
