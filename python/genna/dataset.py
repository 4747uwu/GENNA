"""Dataset — a line-delimited corpus you can curate without rewriting it.

Every operation here is a real, byte-exact, versioned edit on the engine. None
of them copy the dataset. `rollback` works after `save` and a restart because
the version DAG itself is what goes to disk.

A note on what `versions` means. Dropping 24,000 duplicate records is 24,000
splices, so it is 24,000 versions -- each costing O(log n) nodes, which is the
whole point of the architecture. That is a truthful but unhelpful way to show a
history to a person, so `steps` records the LOGICAL operations on top of it.
Both are reported; neither is hidden.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Callable, Iterator, Sequence

from .core import Engine, GennaError, Object, open_store

__all__ = ["Dataset", "Record", "Step"]


@dataclass(frozen=True)
class Record:
    """One line of the dataset, plus where it lives right now."""
    index: int
    offset: int
    raw: bytes

    def __len__(self) -> int:
        return len(self.raw)

    @property
    def text(self) -> str:
        return self.raw.decode("utf-8", "replace")

    def json(self):
        """Parse as JSON, or raise ValueError with the record shown."""
        try:
            return json.loads(self.raw)
        except Exception as e:
            raise ValueError(f"record {self.index} is not valid JSON: {e}") from None

    def content(self, field_name: str = "text") -> str:
        """The field curation should look at.

        Falls back to the whole line when the record is not JSON or lacks the
        field -- which is what makes this work on plain text corpora too.
        """
        try:
            obj = json.loads(self.raw)
        except Exception:
            return self.text
        if isinstance(obj, dict):
            v = obj.get(field_name)
            if isinstance(v, str):
                return v
            if v is not None:
                return json.dumps(v, sort_keys=True)
        return self.text

    def __repr__(self) -> str:
        head = self.raw[:60].decode("utf-8", "replace")
        return f"<Record {self.index}: {head!r}{'...' if len(self.raw) > 60 else ''}>"


@dataclass
class Step:
    """One logical curation operation, and the versions it spans."""
    name: str
    version_before: int
    version_after: int
    records_before: int
    records_after: int
    detail: str = ""

    @property
    def removed(self) -> int:
        return self.records_before - self.records_after

    @property
    def edits(self) -> int:
        return self.version_after - self.version_before

    def __repr__(self) -> str:
        return (f"<Step {self.name}: {self.records_before}->{self.records_after} "
                f"records, v{self.version_before}->v{self.version_after} "
                f"({self.edits} edits)>")


class Dataset:
    """A curatable, versioned, line-delimited dataset."""

    def __init__(self, _engine: Engine, _object: Object,
                 steps: list[Step] | None = None):
        self._engine = _engine
        self._obj = _object
        self._steps: list[Step] = steps or []
        self._buf: bytes = b""
        self._offsets: list[int] = []
        self._lengths: list[int] = []
        self._refresh()

    # ------------------------------------------------------------------
    # construction
    # ------------------------------------------------------------------
    @classmethod
    def from_bytes(cls, data: bytes, name: str = "dataset",
                   train: bool = True) -> "Dataset":
        eng = Engine()
        if train:
            # Learn the corpus language first: the dictionary is append-only,
            # so training after ingest cannot retokenize what is stored.
            eng.train(data)
        obj = eng.create(name, data)
        return cls(_engine=eng, _object=obj)

    @classmethod
    def from_jsonl(cls, path: str | os.PathLike, name: str | None = None,
                   train: bool = True) -> "Dataset":
        p = os.fspath(path)
        with open(p, "rb") as f:
            data = f.read()
        return cls.from_bytes(data, name or os.path.basename(p)[:63], train=train)

    from_text = from_jsonl  # same thing: one record per line

    @classmethod
    def from_records(cls, records, name: str = "dataset",
                     train: bool = True) -> "Dataset":
        """Build from dicts (written as JSONL) or strings/bytes (one per line)."""
        out = bytearray()
        for r in records:
            if isinstance(r, (dict, list)):
                out += json.dumps(r, ensure_ascii=False).encode("utf-8")
            elif isinstance(r, str):
                out += r.encode("utf-8")
            elif isinstance(r, (bytes, bytearray)):
                out += bytes(r)
            else:
                raise TypeError(f"cannot write record of type {type(r).__name__}")
            out += b"\n"
        return cls.from_bytes(bytes(out), name, train=train)

    @classmethod
    def open(cls, path: str | os.PathLike) -> "Dataset":
        eng = open_store(path)
        if len(eng) == 0:
            raise GennaError(f"{path!r} contains no objects")
        return cls(_engine=eng, _object=eng.objects[0])

    # ------------------------------------------------------------------
    # index over the current version
    # ------------------------------------------------------------------
    def _refresh(self) -> None:
        self._buf = self._obj.read()
        offs, lens = [], []
        start = 0
        buf = self._buf
        n = len(buf)
        while start < n:
            nl = buf.find(b"\n", start)
            if nl < 0:
                if n > start:
                    offs.append(start); lens.append(n - start)
                break
            if nl > start:
                offs.append(start); lens.append(nl - start)
            start = nl + 1
        self._offsets, self._lengths = offs, lens

    # ------------------------------------------------------------------
    # reading
    # ------------------------------------------------------------------
    def __len__(self) -> int:
        return len(self._offsets)

    @property
    def nbytes(self) -> int:
        return len(self._buf)

    def __getitem__(self, i):
        if isinstance(i, slice):
            return [self[k] for k in range(*i.indices(len(self)))]
        n = len(self)
        if i < 0:
            i += n
        if not 0 <= i < n:
            raise IndexError(f"record {i} out of range (have {n})")
        off, ln = self._offsets[i], self._lengths[i]
        return Record(index=i, offset=off, raw=self._buf[off:off + ln])

    def __iter__(self) -> Iterator[Record]:
        for i in range(len(self)):
            yield self[i]

    def head(self, n: int = 5) -> list[Record]:
        return self[:n]

    def bytes(self) -> bytes:
        return self._buf

    # ------------------------------------------------------------------
    # history
    # ------------------------------------------------------------------
    @property
    def versions(self):
        return self._obj.versions

    @property
    def steps(self) -> list[Step]:
        return list(self._steps)

    @property
    def engine(self) -> Engine:
        return self._engine

    def touched_versions(self, index: int, limit: int | None = None) -> list[int]:
        """Which versions changed record `index`?

        Answered from the version tree's structure, without materializing any
        version -- which is what makes it usable on a long history at all.

        The result is a SUPERSET of the true answer: it never misses a version
        that really changed the record, but it can include a few that did not,
        because a record's byte range is compared against shared subtrees
        rather than by reading the bytes. Treat it as a filter and confirm the
        candidates by reading just those versions.

        Note that `index` is a position in the CURRENT version. Records shift
        as earlier ones are removed, so the same index means a different
        record after a curation step.
        """
        if not 0 <= index < len(self._offsets):
            raise IndexError("record %d out of range (%d records)"
                             % (index, len(self._offsets)))
        return self._obj.range_history(self._offsets[index],
                                       self._lengths[index], limit=limit)

    def _begin(self) -> tuple[int, int]:
        return len(self._obj.versions) - 1, len(self)

    def _end(self, name: str, before: tuple[int, int], detail: str = "") -> Step:
        self._refresh()
        step = Step(name=name, version_before=before[0],
                    version_after=len(self._obj.versions) - 1,
                    records_before=before[1], records_after=len(self),
                    detail=detail)
        self._steps.append(step)
        return step

    def rollback(self, to) -> Step:
        """Roll back to a version index or a Step (its `version_before`).

        Non-destructive: this appends a new version whose content equals the
        old one, so the history in between is still there.
        """
        before = self._begin()
        target = to.version_before if isinstance(to, Step) else int(to)
        self._obj.rollback(target)
        return self._end(f"rollback(v{target})", before, detail=f"to v{target}")

    def version_bytes(self, v: int) -> bytes:
        return self._obj.read(version=v)

    # ------------------------------------------------------------------
    # curation
    # ------------------------------------------------------------------
    def _delete_indices(self, doomed: Sequence[int]) -> None:
        """Delete records by index, back to front so offsets stay valid."""
        for i in sorted(doomed, reverse=True):
            at = self._offsets[i]
            ln = self._lengths[i]
            # take the trailing newline with the record, unless it is last
            dellen = ln + (1 if at + ln < len(self._buf) else 0)
            self._obj.update(at, dellen, None)

    def dedup(self, field_name: str = "text") -> Step:
        """Drop records whose content is byte-identical to an earlier one."""
        before = self._begin()
        seen: dict[str, int] = {}
        doomed = []
        for rec in self:
            key = rec.content(field_name)
            if key in seen:
                doomed.append(rec.index)
            else:
                seen[key] = rec.index
        self._delete_indices(doomed)
        return self._end("dedup", before, detail=f"exact match on {field_name!r}")

    def filter(self, predicate: Callable[[Record], bool], name: str = "filter") -> Step:
        """Keep records where predicate(record) is True."""
        before = self._begin()
        doomed = [r.index for r in self if not predicate(r)]
        self._delete_indices(doomed)
        return self._end(name, before)

    def filter_length(self, min_bytes: int = 0, max_bytes: int | None = None) -> Step:
        def keep(r: Record) -> bool:
            return len(r) >= min_bytes and (max_bytes is None or len(r) <= max_bytes)
        return self.filter(keep, name=f"filter_length({min_bytes},{max_bytes})")

    def drop_containing(self, needle) -> Step:
        pat = needle.encode("utf-8") if isinstance(needle, str) else bytes(needle)
        return self.filter(lambda r: pat not in r.raw,
                           name=f"drop_containing({needle!r})")

    def replace(self, old, new) -> Step:
        """Replace every occurrence of `old` with `new`, as versioned edits."""
        before = self._begin()
        a = old.encode("utf-8") if isinstance(old, str) else bytes(old)
        b = new.encode("utf-8") if isinstance(new, str) else bytes(new)
        if not a:
            raise ValueError("`old` must be non-empty")
        positions = []
        start = 0
        while True:
            k = self._buf.find(a, start)
            if k < 0:
                break
            positions.append(k)
            start = k + len(a)
        for pos in reversed(positions):        # back to front: offsets hold
            self._obj.update(pos, len(a), b)
        return self._end(f"replace({old!r},{new!r})", before,
                         detail=f"{len(positions)} occurrences")

    def near_dedup(self, threshold: float = 0.85, field_name: str = "text",
                   model: str | None = None, batch_size: int = 64,
                   embedder=None) -> Step:
        """Drop records that are SEMANTICALLY near-duplicates of an earlier one.

        Uses a real sentence-transformer (all-MiniLM-L6-v2) through
        onnxruntime, so paraphrases with no shared wording are caught.

        **Choosing `threshold` is a real decision and there is no safe
        universal value.** Measured on the pairs in tests/test_semantic.py
        with the default model (cosine similarity):

            0.99 / 0.84   whitespace and typo variants of one sentence
            0.56 - 0.77   genuine paraphrases (same meaning, no shared wording)
            below 0.12    unrelated sentences

        So:
          * 0.85 (the default) removes near-copies only. It will NOT remove
            paraphrases -- deliberately, because deleting training data you
            meant to keep is the expensive mistake.
          * ~0.45 removes paraphrases too. On a topically narrow corpus that
            can also remove things you wanted, so measure before trusting it.

        Use `Embedder().similarity(a, b)` on a handful of your own pairs to
        pick a number rather than inheriting mine.

        Requires the `semantic` extra:  pip install "genna[semantic]"
        """
        from .embed import Embedder                # lazy: core has no ML deps

        before = self._begin()
        emb = embedder if embedder is not None else Embedder(model)
        texts = [r.content(field_name) for r in self]
        if not texts:
            return self._end("near_dedup", before, detail="empty dataset")

        vecs = emb.encode(texts, batch_size=batch_size)
        doomed = emb.duplicate_indices(vecs, threshold)
        self._delete_indices(doomed)
        return self._end("near_dedup", before,
                         detail=f"cosine>={threshold} via {emb.name}")

    # ------------------------------------------------------------------
    # output
    # ------------------------------------------------------------------
    def save(self, path: str | os.PathLike) -> None:
        """Persist the dataset AND its whole version history to one file."""
        self._engine.save(path)

    def to_jsonl(self, path: str | os.PathLike, version: int | None = None) -> int:
        """Write one version out as a flat file, byte-exact."""
        data = self._buf if version is None else self._obj.read(version=version)
        with open(os.fspath(path), "wb") as f:
            f.write(data)
        return len(data)

    export = to_jsonl

    # ------------------------------------------------------------------
    def summary(self) -> str:
        st = self._engine.stats
        lines = [
            f"records:        {len(self):,}",
            f"bytes:          {self.nbytes:,}",
            f"versions:       {len(self.versions):,} (all rollback-able)",
            f"store resident: {st.bytes_resident / 1048576:.2f} MB "
            f"({self._engine.chunks:,} chunks, {self._engine.dictionary_size:,} dict entries)",
        ]
        if self._steps:
            lines.append("steps:")
            for s in self._steps:
                lines.append(f"  {s.name:<28} {s.records_before:>8,} -> "
                             f"{s.records_after:>8,}  ({s.edits} edits)")
        if self._engine.store_path:
            lines.append(f"store:          {self._engine.store_path}")
        return "\n".join(lines)

    def __repr__(self) -> str:
        return (f"<genna.Dataset {len(self):,} records, {self.nbytes:,} bytes, "
                f"{len(self.versions):,} versions>")
