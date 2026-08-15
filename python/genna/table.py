"""Schema-aware, columnar tables over Genna.

Why this exists. The engine is a byte sequence; ML datasets are typed rows and
columns. Storing a dataset as one JSONL blob means "relabel one field" is a
splice through every record's bytes, and "dedup on a field" has to parse every
record to find that field. The operations people actually run on training data
are columnar, so the storage should be too.

Two design decisions do the work:

  COLUMN PER OBJECT   Each column is its own Genna object. Changing one column
                      leaves the other K-1 completely untouched -- they are
                      not merely deduplicated, they are not rewritten at all.

  DELETION VECTORS    Dropping rows flips bits in a validity bitmap rather
                      than rewriting the columns. This is what Iceberg and
                      Delta call merge-on-read, and it turns "drop a label
                      class" from O(dataset) into O(rows/8) bytes.
                      compact() materialises the deletions when you want the
                      space back.

Everything is still a versioned Genna edit, so rollback, save/open and the
crash guarantees apply unchanged.
"""
from __future__ import annotations

import json
import os
from typing import Callable, Iterable, Sequence

from .core import Engine, GennaError, Object, open_store

__all__ = ["Table", "Schema"]

_SCHEMA_OBJ = "__schema"
_VALID_OBJ = "__valid"
_MANIFEST_OBJ = "__manifest"
_COL_PREFIX = "c:"
_XFORM_PREFIX = "x:"

# ---------------------------------------------------------------------------
# DEFERRED TRANSFORMS (lazy tags, one level up from the tree)
#
# Rewriting a column to change its values costs the whole column: relabeling
# 10,000 values in a 20,000-row table wrote 31,368 B. But the *description* of
# that change is a few dozen bytes, and it composes.
#
# So a column is (base data, pending ops). An op is appended -- O(1) bytes --
# and applied when the column is read. compact() folds them into the base.
#
# This is lazy propagation's arithmetic without touching gn_enode. Putting
# tags in the tree node would mean pushing them down on every descent, which
# is the hot read path, and changing the on-disk node format, which is the
# thing whose byte-exactness the rest of this repo is built on proving. The
# overlay gets the same complexity at the layer where the values are typed --
# the tree stores bytes and has no idea what "+1" means.
#
# TWO KINDS, because they answer different questions:
#   RANGE_ADD  rows [lo,hi) += delta        -- a contiguous INDEX range
#   MAP        every occurrence of x -> y   -- a PREDICATE over values
#
# Lazy propagation in the literature is the first kind. "UPDATE t SET label=2
# WHERE label=1" is the second, and is not a range update at all -- the
# matching rows are scattered. It is still O(1) here, but as a value remap,
# not as a range tag. Worth keeping straight.
_OP_RANGE_ADD = "add"
_OP_MAP = "map"

# A TABLE version is not a column version. Every object in the store versions
# independently -- editing one column advances that column and nothing else --
# so "table version 7" has to record which version of each object it means.
# Without this, diff(v0, v1) asked an untouched column for version 1 and got
# IndexError, or worse, silently read the wrong version of a column that
# happened to have enough.
#
# The manifest is one line of JSON per table version, itself a versioned
# Genna object.

# CDC chunk size for column data. 1 KB measured best on evolving float
# geometry; column data is similar in character (dense, locally edited).
_CHUNK = 1024


def _pa():
    try:
        import pyarrow as pa
        return pa
    except ImportError:                                    # pragma: no cover
        raise ImportError(
            "Columnar tables need pyarrow:  pip install \"genna[table]\""
        ) from None


class Schema:
    """Column names and Arrow types, versioned alongside the data."""

    __slots__ = ("names", "types")

    def __init__(self, names: Sequence[str], types: Sequence[str]):
        self.names = list(names)
        self.types = list(types)

    def to_json(self) -> bytes:
        return json.dumps({"names": self.names, "types": self.types},
                          separators=(",", ":")).encode("utf-8")

    @staticmethod
    def from_json(b: bytes) -> "Schema":
        d = json.loads(b.decode("utf-8"))
        return Schema(d["names"], d["types"])

    def __repr__(self) -> str:
        cols = ", ".join(f"{n}:{t}" for n, t in zip(self.names, self.types))
        return f"<Schema {cols}>"


def _col_to_bytes(arr) -> bytes:
    """Serialize one column as an Arrow IPC stream.

    Arrow rather than a hand-rolled encoding because it round-trips every type
    exactly -- nulls, dictionaries, nested lists -- and this layer has no
    business reimplementing that.
    """
    pa = _pa()
    import io
    batch = pa.record_batch([arr], names=["v"])
    sink = io.BytesIO()
    with pa.ipc.new_stream(sink, batch.schema) as w:
        w.write_batch(batch)
    return sink.getvalue()


def _bytes_to_col(b: bytes):
    pa = _pa()
    import io
    with pa.ipc.open_stream(io.BytesIO(b)) as r:
        batches = [x for x in r]
    if not batches:
        return None
    if len(batches) == 1:
        return batches[0].column(0)
    return pa.concat_arrays([x.column(0) for x in batches])


class Table:
    """A versioned, columnar dataset."""

    def __init__(self, engine: Engine, schema: Schema, n_rows: int,
                 steps: list | None = None):
        self._engine = engine
        self._schema = schema
        self._n_rows = n_rows
        self._steps = steps or []
        self._cache: dict[str, object] = {}
        # OFF by default, on the evidence. Sketching a changed column costs a
        # stable-hash pass over every value: measured at 26.9 ms per commit on
        # a 20,000-row column versus 0.06 ms without -- 448x. A table doing
        # thousands of small edits would be crippled to answer a question
        # nobody asked. Turn it on where cardinality-by-version is actually
        # wanted:  t.sketches(True)
        #
        # (The 448x is dominated by BLAKE2b per value. Python's built-in hash()
        # is far faster but randomized per process, which would make persisted
        # sketches disagree between runs -- so it is not an option here.)
        self._sketch_on = False

    # ------------------------------------------------------------------
    # manifest: table version -> per-object versions
    # ------------------------------------------------------------------
    def _column_sketch(self, name: str) -> dict:
        """Distinct-count sketch + cheap stats for the CURRENT column value.

        Recomputed only when the column changed, and stored in the manifest
        entry, so asking "how many distinct labels at version 1,847" later is
        a lookup rather than a scan of a historical version.
        """
        from .sketch import HLL
        arr = self.column(name, live_only=False)
        vals = arr.to_pylist()
        h = HLL().add_many(vals)
        nulls = sum(1 for v in vals if v is None)
        return {"hll": h.dumps(), "n": len(vals), "nulls": nulls}

    def _snapshot_versions(self) -> dict:
        d = {"valid": len(self._engine[_VALID_OBJ].versions) - 1,
             "rows": self._n_rows, "cols": {}, "sk": {}}
        for name in self._schema.names:
            d["cols"][name] = len(self._engine[_COL_PREFIX + name].versions) - 1
            # the deferred-op log versions independently too, so a table
            # version must pin it or reading an old version would replay
            # transforms that had not happened yet
            k = _XFORM_PREFIX + name
            if k in self._engine:
                d["cols"][k] = len(self._engine[k].versions) - 1
        if self._sketch_on:
            prev = self._manifest()[-1] if self._manifest_nonempty() else None
            for name in self._schema.names:
                # only recompute where the column (or its op log) moved
                if prev and prev.get("cols", {}).get(name) == d["cols"].get(name) \
                   and prev.get("cols", {}).get(_XFORM_PREFIX + name) \
                       == d["cols"].get(_XFORM_PREFIX + name) \
                   and name in prev.get("sk", {}):
                    d["sk"][name] = prev["sk"][name]
                else:
                    d["sk"][name] = self._column_sketch(name)
        return d

    def _manifest_nonempty(self) -> bool:
        try:
            return len(self._engine[_MANIFEST_OBJ]) > 0
        except KeyError:
            return False

    def sketches(self, on: bool = True) -> "Table":
        """Enable per-version cardinality sketches. Off by default: see the
        cost note in __init__. Takes effect from the next commit."""
        self._sketch_on = bool(on)
        self._commit()
        return self

    def distinct(self, name: str, version: int | None = None) -> int:
        """Estimated distinct values in a column at a table version, O(1).

        Approximate (HyperLogLog, ~3% at this register count). `exact=True`
        on distinct_exact if you need the real number and can pay the scan.
        """
        from .sketch import HLL
        m = self._manifest()
        if not m:
            raise GennaError("no manifest")
        e = m[-1 if version is None else version]
        sk = e.get("sk", {}).get(name)
        if sk is None:
            raise GennaError(
                f"no sketch for {name!r} at that version "
                f"(sketches were disabled when it was written)")
        return HLL.loads(sk["hll"]).estimate()

    def distinct_exact(self, name: str, version: int | None = None) -> int:
        return len(set(self.column(name, version, live_only=False).to_pylist()))

    def _commit(self) -> None:
        """Record the current per-object versions as a new table version."""
        line = json.dumps(self._snapshot_versions(), separators=(",", ":"))
        obj = self._engine[_MANIFEST_OBJ]
        obj.update(len(obj), 0, (line + "\n").encode("utf-8"))

    def _manifest(self) -> list[dict]:
        raw = self._engine[_MANIFEST_OBJ].bytes().decode("utf-8")
        return [json.loads(l) for l in raw.splitlines() if l.strip()]

    def _resolve(self, table_version: int | None):
        """Map a table version to (valid_ver, {col: ver}). None = latest."""
        if table_version is None:
            return None, None
        m = self._manifest()
        if not -len(m) <= table_version < len(m):
            raise IndexError(
                f"table version {table_version} out of range "
                f"(have 0..{len(m)-1})")
        e = m[table_version]
        return e["valid"], e["cols"]

    # ------------------------------------------------------------------
    # construction
    # ------------------------------------------------------------------
    @classmethod
    def from_arrow(cls, table, engine: Engine | None = None) -> "Table":
        pa = _pa()
        import numpy as np
        eng = engine or Engine()
        names = list(table.schema.names)
        types = [str(table.schema.field(n).type) for n in names]
        schema = Schema(names, types)
        eng.create_binary(_SCHEMA_OBJ, schema.to_json(), avg_chunk=256)

        n = table.num_rows
        for name in names:
            col = table.column(name)
            arr = col.combine_chunks() if hasattr(col, "combine_chunks") else col
            eng.create_binary(_COL_PREFIX + name, _col_to_bytes(arr),
                              avg_chunk=_CHUNK)
        # all rows live initially
        valid = np.ones(n, dtype=bool)
        eng.create_binary(_VALID_OBJ, np.packbits(valid).tobytes(),
                          avg_chunk=_CHUNK)
        eng.create_binary(_MANIFEST_OBJ, b"", avg_chunk=256)
        t = cls(eng, schema, n)
        t._commit()                       # table version 0
        return t

    @classmethod
    def from_parquet(cls, path, engine: Engine | None = None) -> "Table":
        pa = _pa()
        import pyarrow.parquet as pq
        return cls.from_arrow(pq.read_table(os.fspath(path)), engine)

    @classmethod
    def from_jsonl(cls, path, engine: Engine | None = None) -> "Table":
        pa = _pa()
        import pyarrow.json as paj
        return cls.from_arrow(paj.read_json(os.fspath(path)), engine)

    @classmethod
    def open(cls, path) -> "Table":
        eng = open_store(path)
        sch = Schema.from_json(eng[_SCHEMA_OBJ].bytes())
        import numpy as np
        vb = eng[_VALID_OBJ].bytes()
        n = 0
        if sch.names:
            col = _bytes_to_col(eng[_COL_PREFIX + sch.names[0]].bytes())
            n = len(col)
        return cls(eng, sch, n)

    # ------------------------------------------------------------------
    # accessors
    # ------------------------------------------------------------------
    @property
    def schema(self) -> Schema:
        return self._schema

    @property
    def columns(self) -> list[str]:
        return list(self._schema.names)

    @property
    def engine(self) -> Engine:
        return self._engine

    def _valid_mask(self, version: int | None = None):
        import numpy as np
        vv, _ = self._resolve(version)
        raw = self._engine[_VALID_OBJ].read(version=vv)
        m = np.unpackbits(np.frombuffer(raw, dtype=np.uint8))
        n = self._n_rows
        if version is not None:
            e = self._manifest()[version]
            n = e.get("rows", n)
        return m[:n].astype(bool)

    @property
    def num_rows(self) -> int:
        """Live rows: deleted rows are tombstoned, not removed."""
        return int(self._valid_mask().sum())

    @property
    def num_rows_physical(self) -> int:
        return self._n_rows

    def __len__(self) -> int:
        return self.num_rows

    # ------------------------------------------------------------------
    # deferred transforms
    # ------------------------------------------------------------------
    def _xform_obj(self, name: str):
        key = _XFORM_PREFIX + name
        if key not in self._engine:
            return self._engine.create_binary(key, b"", avg_chunk=256)
        return self._engine[key]

    def _pending(self, name: str, version=None) -> list:
        try:
            obj = self._engine[_XFORM_PREFIX + name]
        except KeyError:
            return []
        raw = obj.read(version=version).decode("utf-8")
        return [json.loads(l) for l in raw.splitlines() if l.strip()]

    def _push_op(self, name: str, op: dict) -> int:
        obj = self._xform_obj(name)
        line = (json.dumps(op, separators=(",", ":")) + "\n").encode("utf-8")
        obj.update(len(obj), 0, line)
        return len(line)

    @staticmethod
    def _apply_ops(arr, ops):
        """Materialize pending ops onto a column. Order matters.

        Vectorized deliberately. The obvious per-value Python loop made a read
        with 16 pending ops 154x slower than a clean one, which would have
        made the whole deferral pointless: you cannot trade a 98x write win
        for a 154x read loss and call it an optimization. Numeric ops go
        through numpy; value remaps are a single Arrow dictionary pass.
        """
        if not ops:
            return arr
        pa = _pa()
        import numpy as np

        numeric = pa.types.is_integer(arr.type) or pa.types.is_floating(arr.type)
        if numeric:
            vals = arr.to_numpy(zero_copy_only=False).astype(
                np.float64 if pa.types.is_floating(arr.type) else np.int64)
            for op in ops:
                if op["k"] == _OP_RANGE_ADD:
                    lo = max(0, op["lo"]); hi = min(len(vals), op["hi"])
                    if hi > lo:
                        vals[lo:hi] += op["d"]
                elif op["k"] == _OP_MAP:
                    m = {json.loads(k): v for k, v in op["m"].items()}
                    lo = max(0, op.get("lo", 0))
                    hi = min(len(vals), op.get("hi", len(vals)))
                    seg = vals[lo:hi]
                    for old, new in m.items():
                        seg[seg == old] = new
            # Cast back to the column's own width. Deliberately NOT via
            # arr.type.to_pandas_dtype(): that pyarrow method imports pandas
            # internally, and pandas is not a dependency of this package --
            # pyproject has `dependencies = []` and the `table` extra is
            # pyarrow+numpy only. It passed on the dev box, which happened to
            # have pandas installed ambiently, and raised ModuleNotFoundError
            # on all six CI jobs the moment CI was first allowed to run.
            #
            # Asking pyarrow for an empty array of the target type and reading
            # ITS numpy dtype gets pyarrow's own mapping, with no pandas
            # anywhere in the call.
            try:
                target = pa.array([], type=arr.type).to_numpy(
                    zero_copy_only=False).dtype
            except Exception:
                target = vals.dtype
            return pa.array(vals.astype(target), type=arr.type)

        # non-numeric: only MAP applies, done as one pass over the values
        vals = arr.to_pylist()
        combined: dict = {}
        for op in ops:
            if op["k"] != _OP_MAP:
                continue
            m = {json.loads(k): v for k, v in op["m"].items()}
            # compose with what is already pending so N remaps stay one pass
            for k_old, v_new in list(combined.items()):
                if v_new in m:
                    combined[k_old] = m[v_new]
            for k_old, v_new in m.items():
                combined.setdefault(k_old, v_new)
        if combined:
            vals = [combined.get(v, v) for v in vals]
        return pa.array(vals, type=arr.type)

    def add_range(self, name: str, lo: int, hi: int, delta) -> int:
        """rows [lo,hi) += delta, as a deferred tag. Returns bytes written.

        This is the range update lazy propagation is actually about: O(1)
        bytes regardless of how many rows the range covers.
        """
        if name not in self._schema.names:
            raise KeyError(name)
        n = self._push_op(name, {"k": _OP_RANGE_ADD, "lo": int(lo),
                                 "hi": int(hi), "d": delta})
        self._steps.append(("add_range", name, lo, hi))
        self._commit()
        return n

    def relabel_lazy(self, name: str, mapping: dict) -> int:
        """Value remap over the whole column, deferred. Returns bytes written.

        The eager `relabel()` rewrites the column; this writes the *rule*.
        Not a range update -- the matching rows are scattered -- but still
        O(1) because the rule does not depend on how many rows match.
        """
        if name not in self._schema.names:
            raise KeyError(name)
        m = {json.dumps(k): v for k, v in mapping.items()}
        n = self._push_op(name, {"k": _OP_MAP, "m": m})
        self._steps.append(("relabel_lazy", name, len(mapping)))
        self._commit()
        return n

    def pending_ops(self, name: str) -> int:
        return len(self._pending(name))

    def compact_column(self, name: str) -> int:
        """Fold pending ops into the base column. Returns ops folded."""
        ops = self._pending(name)
        if not ops:
            return 0
        arr = self.column(name, live_only=False)          # ops already applied
        obj = self._engine[_COL_PREFIX + name]
        obj.update(0, len(obj), _col_to_bytes(arr))
        x = self._engine[_XFORM_PREFIX + name]
        x.update(0, len(x), b"")
        self._commit()
        return len(ops)

    def column(self, name: str, version: int | None = None, live_only=True):
        """`version` is a TABLE version; the manifest maps it to this
        column's own version, which is generally a different number."""
        if name not in self._schema.names:
            raise KeyError(f"no column {name!r}; have {self._schema.names}")
        _, cols = self._resolve(version)
        cv = None if cols is None else cols.get(name)
        raw = self._engine[_COL_PREFIX + name].read(version=cv)
        arr = _bytes_to_col(raw)
        xv = None
        if cols is not None:
            xv = cols.get(_XFORM_PREFIX + name)
        arr = self._apply_ops(arr, self._pending(name, xv))
        if live_only:
            import numpy as np
            m = self._valid_mask(version)
            if not m.all():
                pa = _pa()
                arr = arr.filter(pa.array(m))
        return arr

    def to_arrow(self, version: int | None = None, live_only=True):
        pa = _pa()
        cols = [self.column(n, version, live_only) for n in self._schema.names]
        return pa.table(cols, names=self._schema.names)

    def to_parquet(self, path, version: int | None = None) -> int:
        import pyarrow.parquet as pq
        pq.write_table(self.to_arrow(version), os.fspath(path))
        return os.path.getsize(os.fspath(path))

    # ------------------------------------------------------------------
    # column-scoped edits: only the named column's object is rewritten
    # ------------------------------------------------------------------
    def set_column(self, name: str, values) -> None:
        pa = _pa()
        if name not in self._schema.names:
            raise KeyError(name)
        arr = values if isinstance(values, (pa.Array, pa.ChunkedArray)) \
            else pa.array(values)
        if hasattr(arr, "combine_chunks"):
            arr = arr.combine_chunks()
        if len(arr) != self._n_rows:
            raise ValueError(
                f"column {name!r} needs {self._n_rows} values "
                f"(physical rows), got {len(arr)}")
        obj = self._engine[_COL_PREFIX + name]
        new = _col_to_bytes(arr)
        obj.update(0, len(obj), new)
        i = self._schema.names.index(name)
        self._schema.types[i] = str(arr.type)

    def map_column(self, name: str, fn: Callable) -> None:
        pa = _pa()
        arr = self.column(name, live_only=False)
        self.set_column(name, pa.array([fn(v) for v in arr.to_pylist()]))

    def relabel(self, name: str, mapping: dict) -> int:
        """Replace values in one column. Touches ONLY that column."""
        pa = _pa()
        arr = self.column(name, live_only=False).to_pylist()
        n = 0
        out = []
        for v in arr:
            if v in mapping:
                out.append(mapping[v]); n += 1
            else:
                out.append(v)
        self.set_column(name, pa.array(out))
        self._steps.append(("relabel", name, n))
        self._commit()
        return n

    # ------------------------------------------------------------------
    # row deletion via the validity vector
    # ------------------------------------------------------------------
    def _set_valid(self, mask) -> None:
        import numpy as np
        packed = np.packbits(mask.astype(bool)).tobytes()
        obj = self._engine[_VALID_OBJ]
        obj.update(0, len(obj), packed)

    def drop_rows(self, row_mask) -> int:
        """Tombstone rows where `row_mask` is True. O(rows/8) bytes written,
        and no data column is touched at all."""
        import numpy as np
        cur = self._valid_mask()
        drop = np.asarray(row_mask, dtype=bool)
        if len(drop) != self._n_rows:
            # a mask over live rows: expand to physical positions
            if len(drop) == int(cur.sum()):
                full = np.zeros(self._n_rows, dtype=bool)
                full[np.nonzero(cur)[0]] = drop
                drop = full
            else:
                raise ValueError("mask length matches neither live nor "
                                 "physical row count")
        new = cur & ~drop
        removed = int(cur.sum() - new.sum())
        if removed:
            self._set_valid(new)
        self._steps.append(("drop_rows", removed))
        self._commit()
        return removed

    def drop_where(self, column: str, predicate: Callable) -> int:
        arr = self.column(column, live_only=False).to_pylist()
        import numpy as np
        mask = np.array([bool(predicate(v)) for v in arr], dtype=bool)
        return self.drop_rows(mask)

    def drop_class(self, column: str, value) -> int:
        """The canonical curation op: remove every row with this label."""
        return self.drop_where(column, lambda v: v == value)

    def filter(self, column: str, predicate: Callable) -> int:
        """Keep rows where predicate(value) is True."""
        return self.drop_where(column, lambda v: not predicate(v))

    def dedup(self, on: str | Iterable[str] | None = None) -> int:
        """Drop later rows duplicating an earlier one.

        Reads only the key columns -- the rest of the table is never touched
        or even materialized.
        """
        import numpy as np
        keys = [on] if isinstance(on, str) else (
            list(on) if on else list(self._schema.names))
        cols = [self.column(k, live_only=False).to_pylist() for k in keys]
        live = self._valid_mask()
        seen: set = set()
        drop = np.zeros(self._n_rows, dtype=bool)
        for i in range(self._n_rows):
            if not live[i]:
                continue
            k = tuple(c[i] for c in cols)
            if k in seen:
                drop[i] = True
            else:
                seen.add(k)
        return self.drop_rows(drop)

    def compact(self) -> int:
        """Physically remove tombstoned rows from every column.

        Until this is called a delete costs bits; after it, the space is
        actually reclaimed. Returns rows removed.
        """
        pa = _pa()
        import numpy as np
        m = self._valid_mask()
        removed = int((~m).sum())
        if removed == 0:
            return 0
        sel = pa.array(m)
        for name in self._schema.names:
            arr = self.column(name, live_only=False).filter(sel)
            obj = self._engine[_COL_PREFIX + name]
            obj.update(0, len(obj), _col_to_bytes(arr))
        self._n_rows = int(m.sum())
        self._set_valid(np.ones(self._n_rows, dtype=bool))
        self._steps.append(("compact", removed))
        self._commit()
        return removed

    # ------------------------------------------------------------------
    # history
    # ------------------------------------------------------------------
    @property
    def versions(self) -> int:
        """Number of TABLE versions (manifest entries), not the version count
        of any single object."""
        return len(self._manifest())

    def column_versions(self, name: str):
        return self._engine[_COL_PREFIX + name].versions

    # ------------------------------------------------------------------
    # query surface
    # ------------------------------------------------------------------
    def diff(self, va: int, vb: int, key: str | None = None) -> dict:
        """Row-level diff between two versions of the validity vector.

        Reports rows added, removed and changed -- the question an auditor
        asks. `key` names a column whose values identify a row; without one,
        rows are identified by physical position.

        Cost is O(rows) for the tombstone comparison plus one materialization
        per key column. It is NOT free, and it is O(n) not O(log n): the
        cheap-structural path below (`touched_versions`) answers the narrower
        "did THIS row change" question without materializing anything.
        """
        import numpy as np
        va_m = self._valid_mask(va)
        vb_m = self._valid_mask(vb)
        removed_idx = np.nonzero(va_m & ~vb_m)[0]
        added_idx = np.nonzero(~va_m & vb_m)[0]

        changed = []
        both = np.nonzero(va_m & vb_m)[0]
        _, cols_a = self._resolve(va)
        _, cols_b = self._resolve(vb)
        for name in self._schema.names:
            # Skip a column whose OWN version did not move between these two
            # table versions -- it cannot have changed, and materializing it
            # would be the expensive part of this query.
            if cols_a and cols_b and cols_a.get(name) == cols_b.get(name):
                continue
            a = self.column(name, va, live_only=False).to_pylist()
            b = self.column(name, vb, live_only=False).to_pylist()
            for i in both:
                if i < len(a) and i < len(b) and a[i] != b[i]:
                    changed.append((int(i), name, a[i], b[i]))

        out = {
            "rows_removed": [int(i) for i in removed_idx],
            "rows_added": [int(i) for i in added_idx],
            "cells_changed": changed,
            "n_removed": len(removed_idx),
            "n_added": len(added_idx),
            "n_cells_changed": len(changed),
        }
        if key and key in self._schema.names:
            kb = self.column(key, vb, live_only=False).to_pylist()
            ka = self.column(key, va, live_only=False).to_pylist()
            out["removed_keys"] = [ka[i] for i in removed_idx if i < len(ka)]
            out["added_keys"] = [kb[i] for i in added_idx if i < len(kb)]
        return out

    def touched_versions(self, row: int, column: str | None = None) -> list[int]:
        """Which versions changed this row?

        Uses the structural-sharing shortcut: an unchanged byte range is the
        same tree node in both versions, so this walks pointers rather than
        materializing versions. Restricted to fixed-width columns, where a
        row maps to a known byte range; for variable-width columns the row's
        offset moves and there is no such range, so this returns [] rather
        than guessing.
        """
        cols = [column] if column else list(self._schema.names)
        hits: set[int] = set()
        for name in cols:
            width = self._fixed_width(name)
            if width is None:
                continue
            obj = self._engine[_COL_PREFIX + name]
            # Arrow IPC puts a header before the buffer; locate it once by
            # comparing a materialized value's position is overkill, so the
            # range is taken as the whole record batch minus nothing --
            # conservative but honest. See _fixed_width for the caveat.
            off = self._ipc_data_offset(name)
            if off is None:
                continue
            hits.update(obj.range_history(off + row * width, width))
        return sorted(hits)

    def _fixed_width(self, name: str) -> int | None:
        pa = _pa()
        t = self.column(name, live_only=False).type
        if pa.types.is_integer(t) or pa.types.is_floating(t):
            return t.bit_width // 8
        if pa.types.is_boolean(t):
            return None            # bit-packed: a row is not byte-addressable
        return None                # variable width: offsets move

    def _ipc_data_offset(self, name: str) -> int | None:
        """Byte offset of the values buffer inside the column's IPC stream.

        Found by writing the same column twice with one value changed and
        seeing where the bytes differ -- which is deterministic for a fixed
        schema and avoids hard-coding Arrow's framing.
        """
        cached = self._cache.get("ipcoff:" + name)
        if cached is not None:
            return cached if cached >= 0 else None
        pa = _pa()
        arr = self.column(name, live_only=False)
        if len(arr) == 0:
            return None
        vals = arr.to_pylist()
        probe = list(vals)
        probe[0] = (probe[0] + 1) if probe[0] is not None else 1
        a = _col_to_bytes(arr)
        b = _col_to_bytes(pa.array(probe, type=arr.type))
        off = -1
        if len(a) == len(b):
            for i in range(len(a)):
                if a[i] != b[i]:
                    off = i
                    break
        self._cache["ipcoff:" + name] = off
        return off if off >= 0 else None

    def save(self, path) -> None:
        self._engine.save(path)

    def summary(self) -> str:
        st = self._engine.stats
        live, phys = self.num_rows, self._n_rows
        lines = [
            f"rows:      {live:,} live"
            + (f" ({phys - live:,} tombstoned, run compact() to reclaim)"
               if phys != live else ""),
            f"columns:   {len(self._schema.names)}  "
            + ", ".join(f"{n}:{t}" for n, t in
                        zip(self._schema.names, self._schema.types))[:100],
            f"store:     {st.bytes_resident/1048576:.2f} MB resident, "
            f"{self._engine.chunks:,} chunks",
        ]
        for name in self._schema.names:
            lines.append(f"  {name:<16} {len(self.column_versions(name))} versions")
        return "\n".join(lines)

    def __repr__(self) -> str:
        return (f"<genna.Table {self.num_rows:,} rows x "
                f"{len(self._schema.names)} cols>")
