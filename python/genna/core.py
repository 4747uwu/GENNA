"""Engine, Object and Version — the thin, honest layer over the C core.

Design rules followed here:
  * Nothing silently succeeds. Every C call that can fail is checked and
    raises, because a curation pipeline that quietly no-ops is worse than one
    that stops.
  * Bytes in, bytes out. No encoding guesses. `str` is accepted and encoded
    as UTF-8 only where it is unambiguous, and offsets are always BYTE
    offsets, which is what the engine works in.
  * Every mutating call holds GENNA_LOCK (see _native for why).
"""
from __future__ import annotations

import ctypes
import os
from typing import Iterator, Sequence

from ._native import GENNA_LOCK, LATEST, BinOpts, lib

__all__ = ["Engine", "Object", "Version", "Stats", "GennaError", "open_store"]


class GennaError(RuntimeError):
    """Raised when the engine reports failure."""


def _as_bytes(data, argname: str = "data") -> bytes:
    if data is None:
        return b""
    if isinstance(data, str):
        return data.encode("utf-8")
    if isinstance(data, (bytes, bytearray, memoryview)):
        return bytes(data)
    raise TypeError(f"{argname} must be bytes or str, not {type(data).__name__}")


class Stats:
    """A snapshot of the engine's work counters."""

    __slots__ = ("chunks_created", "chunks_deduped", "tokens_scanned",
                 "tokens_detokenized", "bytes_in", "bytes_resident")

    def __init__(self, values: Sequence[int]):
        (self.chunks_created, self.chunks_deduped, self.tokens_scanned,
         self.tokens_detokenized, self.bytes_in, self.bytes_resident) = values

    def __repr__(self) -> str:
        return (f"Stats(bytes_in={self.bytes_in}, "
                f"bytes_resident={self.bytes_resident}, "
                f"chunks_created={self.chunks_created}, "
                f"chunks_deduped={self.chunks_deduped})")


class Version:
    """One immutable version of an object. Reading it is O(bytes requested)."""

    __slots__ = ("_obj", "_index")

    def __init__(self, obj: "Object", index: int):
        self._obj = obj
        self._index = index

    @property
    def index(self) -> int:
        return self._index

    def __len__(self) -> int:
        return int(lib.gn_object_bytes(self._obj._ptr, self._index))

    @property
    def tokens(self) -> int:
        return int(lib.gn_object_tokens(self._obj._ptr, self._index))

    def read(self, offset: int = 0, length: int | None = None) -> bytes:
        """Materialize a byte range of THIS version."""
        return self._obj.read(offset, length, version=self._index)

    def bytes(self) -> bytes:
        return self.read()

    def __repr__(self) -> str:
        return f"<Version {self._index} of {self._obj.name!r}: {len(self)} bytes>"


class _Versions(Sequence):
    """`obj.versions` — indexable, iterable, len()-able history."""

    __slots__ = ("_obj",)

    def __init__(self, obj: "Object"):
        self._obj = obj

    def __len__(self) -> int:
        return int(lib.gn_object_versions(self._obj._ptr))

    def __getitem__(self, i):
        n = len(self)
        if isinstance(i, slice):
            return [Version(self._obj, k) for k in range(*i.indices(n))]
        if i < 0:
            i += n
        if not 0 <= i < n:
            raise IndexError(f"version {i} out of range (have 0..{n - 1})")
        return Version(self._obj, i)

    def __iter__(self) -> Iterator[Version]:
        for i in range(len(self)):
            yield Version(self._obj, i)

    def __repr__(self) -> str:
        return f"<{len(self)} versions>"


class Object:
    """A versioned byte sequence. Edits are O(log n) and never rewrite it."""

    __slots__ = ("_ptr", "_engine", "__weakref__")

    def __init__(self, engine: "Engine", ptr: int):
        self._engine = engine
        self._ptr = ptr

    # -- identity ---------------------------------------------------------
    @property
    def name(self) -> str:
        raw = lib.gn_object_name(self._ptr)
        return raw.decode("utf-8", "replace") if raw else ""

    @property
    def versions(self) -> _Versions:
        return _Versions(self)

    def __len__(self) -> int:
        """Byte length of the CURRENT version."""
        return int(lib.gn_object_bytes(self._ptr, LATEST))

    @property
    def tokens(self) -> int:
        return int(lib.gn_object_tokens(self._ptr, LATEST))

    # -- read -------------------------------------------------------------
    def read(self, offset: int = 0, length: int | None = None,
             version: int | None = None) -> bytes:
        """Materialize [offset, offset+length) of `version` (default: latest).

        Only the chunks covering the range are detokenized -- reading 1 KB of
        a 10 GB object costs 1 KB of work, not 10 GB.
        """
        self._engine._check_open()
        # gn_read_version takes a REAL index and rejects anything >= n_ver.
        # The LATEST sentinel is understood only by the capi accessors, so it
        # must be resolved here -- passing it through returns 0 bytes and looks
        # exactly like an empty object.
        n_ver = int(lib.gn_object_versions(self._ptr))
        if version is None:
            vidx = n_ver - 1
            if vidx < 0:
                return b""
        else:
            vidx = int(version)
            if vidx < 0:
                vidx += n_ver
            if not 0 <= vidx < n_ver:
                raise IndexError(
                    f"version {version} out of range (have 0..{n_ver - 1})")

        total = int(lib.gn_object_bytes(self._ptr, vidx))
        if offset < 0:
            raise ValueError("offset must be >= 0")
        if offset >= total:
            return b""
        if length is None:
            length = total - offset
        length = min(int(length), total - offset)
        if length <= 0:
            return b""

        buf = ctypes.create_string_buffer(length)
        with GENNA_LOCK:
            got = lib.gn_read_version(self._engine._ptr, self._ptr, vidx,
                                      offset, length, buf)
        return buf.raw[:got]

    def bytes(self) -> bytes:
        """The whole current version."""
        return self.read()

    def __bytes__(self) -> bytes:
        return self.read()

    # -- write ------------------------------------------------------------
    def update(self, offset: int, delete: int = 0, insert=None) -> int:
        """Splice: replace [offset, offset+delete) with `insert`.

        Appends a new version. Returns the new version index.
        Insert-only: delete=0. Delete-only: insert=None.
        """
        self._engine._check_open()
        data = _as_bytes(insert, "insert")
        if offset < 0 or delete < 0:
            raise ValueError("offset and delete must be >= 0")
        with GENNA_LOCK:
            rc = lib.gn_update(self._engine._ptr, self._ptr,
                               int(offset), int(delete),
                               data if data else None, len(data))
        if rc != 0:
            raise GennaError(
                f"update rejected: offset {offset} is past the end "
                f"({len(self)} bytes)")
        return len(self.versions) - 1

    def insert(self, offset: int, data) -> int:
        return self.update(offset, 0, data)

    def delete_range(self, offset: int, length: int) -> int:
        return self.update(offset, length, None)

    def append(self, data) -> int:
        return self.update(len(self), 0, data)

    def rollback(self, version: int) -> int:
        """Make `version`'s content the new head, keeping all history.

        Implemented as an edit, not a truncation: nothing is destroyed, so you
        can roll back a rollback.
        """
        v = self.versions[version]          # raises IndexError if absent
        content = v.read()
        return self.update(0, len(self), content)

    # -- version comparison, without materializing ------------------------
    def range_changed(self, version_a: int, version_b: int,
                      offset: int, length: int) -> bool:
        """Did bytes [offset, offset+length) differ between two versions?

        Answered from the tree, not the bytes: structural sharing means an
        unchanged region is literally the same node in both versions, so this
        is a pointer comparison down O(log n) levels.

        **Conservative in one direction.** False positives are possible and
        expected: the unit of sharing is a chunk, so a range sharing a chunk
        with a genuinely edited range reports True even though its own bytes
        are unchanged. False negatives are not possible -- if this returns
        False the bytes are the same memory. So it is a sound *filter*:
        use it to skip work, then confirm candidates by reading.
        """
        self._engine._check_open()
        return bool(lib.gn_range_changed(self._ptr, int(version_a),
                                         int(version_b), int(offset),
                                         int(length)))

    def range_history(self, offset: int, length: int,
                      limit: int | None = None) -> list[int]:
        """Candidate versions at which this byte range changed.

        The "which versions touched record N?" query. Returns a SUPERSET of
        the true answer -- see range_changed for why -- so treat it as a
        filter: it never misses a real change, and the candidates it does
        return can be confirmed by reading just those versions.

        Cost is O(versions x log n) pointer walks rather than
        O(versions x bytes).
        """
        self._engine._check_open()
        # Size the buffer to the version count. Allocating a fixed 1M-entry
        # array here cost 4 MB per call and made this query slower than the
        # brute force it was supposed to beat.
        n_ver = int(lib.gn_object_versions(self._ptr))
        cap = n_ver if limit is None else min(int(limit), n_ver)
        if cap <= 0:
            return []
        buf = (ctypes.c_uint32 * cap)()
        n = int(lib.gn_range_history(self._ptr, int(offset), int(length),
                                     buf, cap))
        return [int(buf[i]) for i in range(n)]

    def trim_history(self, keep: int) -> int:
        """Drop all but the newest `keep` versions. Returns how many went."""
        self._engine._check_open()
        with GENNA_LOCK:
            return int(lib.gn_trim_history(self._engine._ptr, self._ptr, int(keep)))

    def __repr__(self) -> str:
        return (f"<genna.Object {self.name!r}: {len(self)} bytes, "
                f"{len(self.versions)} versions>")


class Engine:
    """A Genna store: a dictionary, a chunk sea, and named versioned objects.

    Usable as a context manager, which is the only way to be sure the native
    memory is released promptly rather than at interpreter shutdown.
    """

    __slots__ = ("_ptr", "_closed", "__weakref__")

    def __init__(self, _ptr: int | None = None):
        if _ptr is None:
            _ptr = lib.gn_engine_new()
            if not _ptr:
                raise GennaError("gn_engine_new failed (out of memory)")
        self._ptr = _ptr
        self._closed = False

    # -- lifecycle --------------------------------------------------------
    def _check_open(self):
        if self._closed:
            raise GennaError("this Engine is closed")

    def close(self):
        if not self._closed and self._ptr:
            with GENNA_LOCK:
                lib.gn_engine_free(self._ptr)
            self._ptr = None
            self._closed = True

    def __enter__(self) -> "Engine":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    # -- objects ----------------------------------------------------------
    def create(self, name: str, data) -> Object:
        """Ingest bytes as version 0 of a new object."""
        self._check_open()
        payload = _as_bytes(data)
        cname = name.encode("utf-8")
        if len(cname) > 63:
            raise ValueError("object name must be <= 63 bytes when UTF-8 encoded")
        with GENNA_LOCK:
            ptr = lib.gn_create(self._ptr, cname, payload if payload else None,
                                len(payload))
        if not ptr:
            raise GennaError(f"could not create object {name!r}")
        return Object(self, ptr)

    def create_binary(self, name: str, data, avg_chunk: int = 4096,
                      fixed: bool = False) -> Object:
        """Ingest raw bytes with content-defined chunking.

        Use this for geometry, embeddings, tensors -- anything that is not
        text. It differs from create() in two ways that matter:

          * No dictionary learning. The text path learns from every ingest,
            and because the dictionary is shared and append-only, an object
            ingested later can tokenize identical bytes differently. Measured
            on a mesh, that dropped a 1% localized edit from ~98% sharing to
            ~67%.
          * Chunk boundaries come from the CONTENT (gear-hash rolling window),
            so inserting or deleting bytes perturbs one chunk instead of
            shifting every boundary after it. Measured on real scans,
            inserting 1% of vertices: 49% shared with fixed cuts, 95-97%
            with content-defined cuts.

        `fixed=True` forces fixed-size cuts, for A/B measurement.

        Cost: a byte becomes a 4-byte token in the store, so a binary object
        occupies about 4x its size in RAM.
        """
        self._check_open()
        payload = _as_bytes(data)
        cname = name.encode("utf-8")
        if len(cname) > 63:
            raise ValueError("object name must be <= 63 bytes when UTF-8 encoded")
        opts = BinOpts()
        lib.gn_bin_opts_default(ctypes.byref(opts))
        opts.avg_chunk = int(avg_chunk)
        opts.min_chunk = max(1, int(avg_chunk) // 4)
        opts.max_chunk = int(avg_chunk) * 4
        opts.fixed = 1 if fixed else 0
        with GENNA_LOCK:
            ptr = lib.gn_create_binary(self._ptr, cname,
                                       payload if payload else None,
                                       len(payload), ctypes.byref(opts))
        if not ptr:
            raise GennaError(f"could not create binary object {name!r}")
        return Object(self, ptr)

    def get(self, name: str) -> Object:
        self._check_open()
        ptr = lib.gn_object_open(self._ptr, name.encode("utf-8"))
        if not ptr:
            raise KeyError(name)
        return Object(self, ptr)

    def __getitem__(self, name: str) -> Object:
        return self.get(name)

    def __contains__(self, name: str) -> bool:
        try:
            self.get(name)
            return True
        except KeyError:
            return False

    def delete(self, name: str) -> None:
        self._check_open()
        with GENNA_LOCK:
            rc = lib.gn_delete(self._ptr, name.encode("utf-8"))
        if rc != 0:
            raise KeyError(name)

    @property
    def objects(self) -> list[Object]:
        self._check_open()
        n = int(lib.gn_engine_objects(self._ptr))
        return [Object(self, lib.gn_engine_object(self._ptr, i)) for i in range(n)]

    def __len__(self) -> int:
        return int(lib.gn_engine_objects(self._ptr)) if not self._closed else 0

    def __iter__(self) -> Iterator[Object]:
        return iter(self.objects)

    # -- language ---------------------------------------------------------
    def train(self, text, rounds: int = 6, merges: int = 200_000,
              min_count: int = 16) -> int:
        """Learn the corpus's language. Do this BEFORE create() for best
        compression: the dictionary is append-only, so later training adds
        entries but cannot retokenize what is already stored."""
        self._check_open()
        payload = _as_bytes(text)
        with GENNA_LOCK:
            return int(lib.gn_train(self._ptr, payload, len(payload),
                                    rounds, merges, min_count))

    # -- search -----------------------------------------------------------
    def search(self, needle, limit: int = 100_000) -> list[tuple[str, int]]:
        """Exact search across all objects' latest versions.

        Returns (object_name, byte_offset) pairs. A needle whose units are
        absent from the dictionary returns [] without scanning anything.
        """
        self._check_open()
        pat = _as_bytes(needle, "needle")
        if not pat:
            return []
        offs = (ctypes.c_uint64 * limit)()
        objs = (ctypes.c_uint32 * limit)()
        with GENNA_LOCK:
            n = int(lib.gn_search_flat(self._ptr, pat, len(pat), offs, objs, limit))
        names = [o.name for o in self.objects]
        return [(names[objs[i]] if objs[i] < len(names) else "", int(offs[i]))
                for i in range(n)]

    # -- persistence ------------------------------------------------------
    def save(self, path: str | os.PathLike) -> None:
        """Write the whole store -- chunks, dictionary, every version -- to
        `path`, atomically. Binds this engine to `path` and starts a WAL, so
        edits after the save are crash-safe too."""
        self._check_open()
        with GENNA_LOCK:
            rc = lib.gn_save(self._ptr, str(path).encode("utf-8"))
        if rc != 0:
            raise GennaError(f"save to {path!r} failed: {os.strerror(ctypes.get_errno() or 0)}")

    # -- introspection ----------------------------------------------------
    @property
    def stats(self) -> Stats:
        arr = (ctypes.c_uint64 * 6)()
        lib.gn_stats_flat(self._ptr, arr)
        return Stats([int(x) for x in arr])

    @property
    def chunks(self) -> int:
        return int(lib.gn_store_chunk_count(self._ptr))

    @property
    def dictionary_size(self) -> int:
        return int(lib.gn_dict_entries(self._ptr))

    @property
    def store_path(self) -> str | None:
        raw = lib.gn_store_path(self._ptr)
        return raw.decode("utf-8") if raw else None

    @property
    def wal_active(self) -> bool:
        return bool(lib.gn_wal_active(self._ptr))

    @property
    def wal_healthy(self) -> bool:
        """False once a log write or fsync has failed -- durability lapsed."""
        return bool(lib.gn_wal_ok(self._ptr))

    @property
    def wal_records(self) -> int:
        return int(lib.gn_wal_records(self._ptr))

    @property
    def replayed_records(self) -> int:
        """How many un-checkpointed edits were recovered when this store was
        opened. Non-zero means the previous session did not exit cleanly."""
        return int(lib.gn_wal_replayed(self._ptr))

    def set_durability(self, fsync_every_edit: bool) -> bool:
        """True (default): every edit is fsynced before it is applied --
        survives power loss. False: survives a killed process but not a power
        cut, and is much faster for bulk curation. Returns the previous mode.
        """
        self._check_open()
        prev = lib.gn_wal_set_sync(self._ptr, 1 if fsync_every_edit else 0)
        if prev < 0:
            raise GennaError("no write-ahead log attached; call save() first")
        return bool(prev)

    def __repr__(self) -> str:
        if self._closed:
            return "<genna.Engine (closed)>"
        return (f"<genna.Engine {len(self)} objects, {self.chunks} chunks, "
                f"{self.stats.bytes_resident / 1048576:.2f} MB resident>")


def open_store(path: str | os.PathLike) -> Engine:
    """Open a saved store, replaying any un-checkpointed edits."""
    p = str(path)
    if not os.path.exists(p):
        raise FileNotFoundError(p)
    with GENNA_LOCK:
        ptr = lib.gn_open(p.encode("utf-8"))
    if not ptr:
        raise GennaError(
            f"could not open {p!r}: not a Genna store, or it is corrupt "
            f"(the header/payload checksum did not verify)")
    return Engine(_ptr=ptr)
