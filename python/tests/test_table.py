"""Columnar tables: correctness, then the measurement that justifies them.

The claim being tested is not "it works" but "the column layout costs
materially less than the row layout for the operations ML teams actually
run". If relabeling one column of a 10-column table writes the same bytes as
relabeling the whole table, the layout bought nothing.
"""
from __future__ import annotations

import os
import sys
import tempfile

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


def make_table(n=20000):
    import pyarrow as pa
    labels = ["cat", "dog", "bird", "fish"]
    return pa.table({
        "id":     pa.array(list(range(n)), type=pa.int64()),
        "label":  pa.array([labels[i % 4] for i in range(n)]),
        "text":   pa.array([f"sample number {i} about something" for i in range(n)]),
        "score":  pa.array([(i % 997) / 997.0 for i in range(n)], type=pa.float64()),
        "source": pa.array([f"src-{i % 50}" for i in range(n)]),
    })


def store_bytes(tbl, path):
    tbl.save(path)
    return os.path.getsize(path)


def main():
    print("=== genna columnar tables ===")
    try:
        import pyarrow  # noqa: F401
    except ImportError:
        print("SKIP: pyarrow not installed")
        return 0

    tmp = tempfile.mkdtemp(prefix="gn_tbl_")
    at = make_table()
    n = at.num_rows

    print(f"\n-- ingest: {n:,} rows x {len(at.schema.names)} columns --")
    t = genna.Table.from_arrow(at)
    check(t.num_rows == n, f"{n:,} rows ingested")
    check(t.columns == ["id", "label", "text", "score", "source"],
          f"schema round-trips: {t.columns}")
    back = t.to_arrow()
    check(back.num_rows == n, "to_arrow returns every row")
    check(back.column("label").to_pylist() == at.column("label").to_pylist(),
          "column values are identical after a round trip")
    check(back.column("score").to_pylist() == at.column("score").to_pylist(),
          "float column is exact after a round trip")

    base = store_bytes(t, os.path.join(tmp, "base.gn"))
    print(f"   store: {base:,} bytes")

    # ---- the measurement -------------------------------------------------
    print("\n-- cost of a column-scoped edit (relabel one column) --")
    n_changed = t.relabel("label", {"cat": "feline", "dog": "canine"})
    after = store_bytes(t, os.path.join(tmp, "relabel.gn"))
    check(n_changed == n // 2, f"relabelled {n_changed:,} values")

    # only the label column should have gained a version
    vers = {c: len(t.column_versions(c)) for c in t.columns}
    print(f"   versions per column after relabel: {vers}")
    check(vers["label"] == 2, "label column has 2 versions")
    check(all(vers[c] == 1 for c in ("id", "text", "score", "source")),
          "the other 4 columns were NOT touched (1 version each)")

    # row-major comparison: same logical edit through the JSONL path
    import json
    rows = at.to_pylist()
    # compact separators so the patterns below actually occur: json.dumps
    # defaults to '"label": "cat"' WITH a space, and searching for the
    # space-free form silently matched nothing (the first version of this
    # test "measured" a 3-byte row-major edit that never happened).
    jsonl = "".join(json.dumps(r, separators=(",", ":")) + "\n"
                    for r in rows).encode()
    assert b'"label":"cat"' in jsonl, "row-major fixture is not what we search for"
    ds = genna.Dataset.from_bytes(jsonl, name="rowmajor", train=True)
    row_base = os.path.join(tmp, "row_base.gn")
    ds.save(row_base)
    rb = os.path.getsize(row_base)
    s1 = ds.replace('"label":"cat"', '"label":"feline"')
    s2 = ds.replace('"label":"dog"', '"label":"canine"')
    row_after = os.path.join(tmp, "row_after.gn")
    ds.save(row_after)
    ra = os.path.getsize(row_after)
    check(s1.removed == 0 and len(ds) == n,
          f"row-major edit kept all {n:,} records")
    print(f"   row-major replaced {n//2:,} values across "
          f"{s1.edits + s2.edits:,} splices")

    col_delta, row_delta = after - base, ra - rb
    print(f"   columnar : {base:,} -> {after:,}  (+{col_delta:,} B)")
    print(f"   row-major: {rb:,} -> {ra:,}  (+{row_delta:,} B)")
    check(col_delta < row_delta,
          f"column-scoped relabel costs less than row-major "
          f"({col_delta:,} vs {row_delta:,} B, {row_delta/max(1,col_delta):.1f}x)")
    ds.engine.close()

    # ---- deletion vectors ------------------------------------------------
    print("\n-- drop a label class (deletion vector, not a rewrite) --")
    before_drop = store_bytes(t, os.path.join(tmp, "predrop.gn"))
    v_before = {c: len(t.column_versions(c)) for c in t.columns}
    removed = t.drop_class("label", "bird")
    after_drop = store_bytes(t, os.path.join(tmp, "postdrop.gn"))
    v_after = {c: len(t.column_versions(c)) for c in t.columns}
    check(removed == n // 4, f"dropped {removed:,} rows of class 'bird'")
    check(t.num_rows == n - removed, f"{t.num_rows:,} live rows remain")
    check(v_after == v_before,
          "NO data column was rewritten - only the validity vector")
    print(f"   store {before_drop:,} -> {after_drop:,} "
          f"(+{after_drop-before_drop:,} B for {removed:,} deletions)")
    check(after_drop - before_drop < removed,
          f"deleting {removed:,} rows cost fewer than {removed:,} bytes")

    got = t.to_arrow()
    check(got.num_rows == n - removed, "to_arrow honours the deletion vector")
    check("bird" not in set(got.column("label").to_pylist()),
          "no 'bird' rows survive materialization")

    # ---- dedup on one field ---------------------------------------------
    print("\n-- dedup on a single column --")
    t2 = genna.Table.from_arrow(make_table(2000))
    d = t2.dedup(on="source")          # 50 distinct sources
    check(t2.num_rows == 50, f"dedup on 'source' left {t2.num_rows} rows "
                             f"(50 distinct), removed {d}")
    t2.engine.close()

    # ---- compaction ------------------------------------------------------
    print("\n-- compact reclaims tombstoned rows --")
    phys_before = t.num_rows_physical
    rem = t.compact()
    check(rem == removed, f"compact removed {rem:,} tombstoned rows")
    check(t.num_rows_physical == n - removed,
          f"physical rows {phys_before:,} -> {t.num_rows_physical:,}")
    check(t.to_arrow().num_rows == n - removed, "content unchanged by compaction")

    # ---- persistence -----------------------------------------------------
    print("\n-- save / reopen --")
    p = os.path.join(tmp, "final.gn")
    t.save(p)
    ref = t.to_arrow().to_pylist()
    t.engine.close()
    t3 = genna.Table.open(p)
    check(t3.columns == ["id", "label", "text", "score", "source"],
          "schema survives reopen")
    check(t3.to_arrow().to_pylist() == ref,
          "every cell identical after save -> reopen")
    t3.engine.close()

    for f in os.listdir(tmp):
        try:
            os.remove(os.path.join(tmp, f))
        except OSError:
            pass
    os.rmdir(tmp)

    print(f"\n{'TABLE: FAILURES' if FAILS else 'TABLE: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
