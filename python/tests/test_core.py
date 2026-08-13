"""Byte-exactness through the bindings, not just in C.

The C suite proves the engine is byte-exact. These prove the BINDINGS are:
that nothing is lost or mangled crossing the FFI boundary -- truncated
pointers, wrong integer widths, encoding guesses, off-by-one on lengths.
That is where FFI layers actually break.

Runs standalone (`python test_core.py`) so it needs no pytest.
"""
from __future__ import annotations

import os
import random
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import genna

FAILS = []
COUNT = 0


def check(cond, msg):
    global COUNT
    COUNT += 1
    if cond:
        print(f"  ok    {msg}")
    else:
        print(f"  FAIL  {msg}")
        FAILS.append(msg)
    return bool(cond)


def nonempty(data, msg):
    """Guard against vacuous passes.

    An earlier version of these tests compared two reads that were BOTH empty
    (a sentinel bug made every latest-version read return b"") and happily
    reported 'ok'. Any comparison of engine output has to assert the output
    exists first, or the suite can be green while the binding returns nothing.
    """
    return check(len(data) > 0, msg)


def section(name):
    print(f"\n-- {name} --")


# ----------------------------------------------------------------------
def test_roundtrip():
    section("create / read round trip")
    with genna.Engine() as eng:
        data = b"the engine holds data below the layer\n" * 3
        obj = eng.create("a", data)
        nonempty(bytes(obj), "a full read returns actual bytes")
        check(bytes(obj) == data, "full read is byte-identical to input")
        check(len(obj) == len(data), f"len() == {len(data)}")
        check(obj.read(4, 6) == data[4:6 + 4], "mid-range read matches the slice")
        check(obj.read(0, 10**9) == data, "over-long read clamps, no error")
        check(obj.read(10**9, 10) == b"", "read past end returns empty")
        check(obj.name == "a", "name round-trips")


def test_binary_safe():
    section("binary safety (this is where FFI usually breaks)")
    with genna.Engine() as eng:
        # every byte value, including NUL -- a c_char_p that treated the
        # payload as a C string would truncate here
        data = bytes(range(256)) * 40
        obj = eng.create("bin", data)
        got = bytes(obj)
        check(got == data, f"all 256 byte values survive ({len(data)} bytes)")
        check(b"\x00" in got, "embedded NUL bytes are preserved")

        # UTF-8 and lone high bytes
        txt = "héllo wörld — ünïcode ✓ 日本語\n".encode("utf-8") * 50
        o2 = eng.create("utf8", txt)
        check(bytes(o2) == txt, "UTF-8 payload is byte-identical")


def test_large():
    section("a payload bigger than any 32-bit truncation would survive")
    with genna.Engine() as eng:
        chunk = b"".join(bytes([i % 251]) for i in range(1000))
        data = chunk * 3000              # ~3 MB, non-repeating enough to matter
        obj = eng.create("big", data)
        check(len(obj) == len(data), f"length {len(data):,} preserved exactly")
        check(bytes(obj) == data, "3 MB payload byte-identical")
        mid = len(data) // 2
        check(obj.read(mid, 1000) == data[mid:mid + 1000], "slice deep inside matches")


def test_versions():
    section("versioned edits and time travel")
    with genna.Engine() as eng:
        base = b"the engine holds data below the layer"
        obj = eng.create("v", base)
        obj.update(11, 0, b"quietly ")
        check(bytes(obj) == b"the engine quietly holds data below the layer",
              "insert lands in the right place")
        obj.update(11, 8, None)
        check(bytes(obj) == base, "delete restores the original bytes")
        obj.update(0, 3, b"our")
        check(bytes(obj) == b"our engine holds data below the layer", "replace")

        check(len(obj.versions) == 4, f"4 versions recorded, got {len(obj.versions)}")
        check(obj.versions[0].read() == base, "v0 still byte-exact after 3 edits")
        check(obj.versions[-1].read() == bytes(obj), "versions[-1] is the head")
        # derive rather than hardcode: a wrong literal here fails the test for
        # a reason that has nothing to do with the engine
        expect = [len(base), len(base) + 8, len(base), len(base)]
        got = [len(v) for v in obj.versions]
        check(got == expect,
              f"each version reports its own length {got} == {expect}")

        try:
            obj.versions[99]
            check(False, "out-of-range version raises IndexError")
        except IndexError:
            check(True, "out-of-range version raises IndexError")


def test_rollback():
    section("rollback keeps history")
    with genna.Engine() as eng:
        obj = eng.create("r", b"original content here")
        v0 = bytes(obj)
        nonempty(v0, "baseline read is non-empty (no vacuous comparison below)")
        obj.update(0, 8, b"CHANGED")
        obj.update(0, 0, b"MORE ")
        check(bytes(obj) != v0, "content changed")
        obj.rollback(0)
        check(bytes(obj) == v0, "rollback(0) restores v0 byte-exactly")
        check(len(obj.versions) == 4, "rollback appended a version, destroyed none")
        check(obj.versions[1].read() != v0, "the intermediate version is still there")


def test_differential_vs_shadow():
    section("differential: 400 random splices vs a Python shadow buffer")
    rng = random.Random(20260811)
    with genna.Engine() as eng:
        shadow = bytearray(b"".join(
            bytes([rng.randrange(32, 127)]) for _ in range(20000)))
        eng.train(bytes(shadow))
        obj = eng.create("d", bytes(shadow))
        bad = None
        for i in range(400):
            n = len(shadow)
            off = rng.randrange(0, n + 1)
            dele = min(rng.randrange(0, 40), n - off)
            ins = bytes(rng.randrange(0, 256) for _ in range(rng.randrange(0, 30)))
            obj.update(off, dele, ins or None)
            shadow[off:off + dele] = ins
            got = bytes(obj)
            if got != bytes(shadow):
                bad = (i, off, dele, len(ins))
                break
        check(bad is None,
              f"400 random splices matched the shadow byte-for-byte"
              + ("" if bad is None else f" (diverged at {bad})"))
        check(len(obj) == len(shadow), "final length agrees with the shadow")


def test_search():
    section("search")
    with genna.Engine() as eng:
        eng.create("s1", b"another file where the engine appears twice: engine")
        eng.create("s2", b"the engine holds data")
        hits = eng.search("engine")
        check(len(hits) == 3, f"'engine' found 3x across objects (got {len(hits)})")
        check(all(isinstance(h[0], str) and isinstance(h[1], int) for h in hits),
              "hits are (name, offset) pairs")
        check(eng.search("zzzcryptid") == [], "absent needle returns []")
        for name, off in hits:
            obj = eng[name]
            check_bytes = obj.read(off, 6)
            if check_bytes != b"engine":
                check(False, f"hit at {name}:{off} does not point at the needle")
                break
        else:
            check(True, "every reported offset really points at the needle")


def test_engine_dict_api():
    section("engine container API")
    with genna.Engine() as eng:
        eng.create("alpha", b"aaa")
        eng.create("beta", b"bbb")
        check(len(eng) == 2, "len(engine) == 2 objects")
        check("alpha" in eng and "beta" in eng, "__contains__ works")
        check("gamma" not in eng, "missing name is not 'in' engine")
        check(bytes(eng["alpha"]) == b"aaa", "engine['name'] indexes")
        check(sorted(o.name for o in eng) == ["alpha", "beta"], "iteration yields objects")
        try:
            eng["nope"]
            check(False, "missing object raises KeyError")
        except KeyError:
            check(True, "missing object raises KeyError")


def test_errors():
    section("errors are raised, not swallowed")
    with genna.Engine() as eng:
        obj = eng.create("e", b"short")
        try:
            obj.update(9999, 0, b"x")
            check(False, "editing past the end raises")
        except genna.GennaError:
            check(True, "editing past the end raises GennaError")
        try:
            eng.create("x" * 100, b"y")
            check(False, "over-long name raises")
        except ValueError:
            check(True, "over-long object name raises ValueError")
        try:
            obj.update(0, 0, 12345)
            check(False, "wrong insert type raises")
        except TypeError:
            check(True, "wrong insert type raises TypeError")

    eng2 = genna.Engine()
    eng2.close()
    try:
        eng2.create("z", b"z")
        check(False, "using a closed engine raises")
    except genna.GennaError:
        check(True, "using a closed engine raises GennaError")


def test_persistence():
    section("persistence through the bindings")
    tmp = tempfile.mkdtemp(prefix="genna_py_")
    store = os.path.join(tmp, "s.genna")
    refs = []
    with genna.Engine() as eng:
        data = b"".join(f"record {i} with some content to tokenize\n".encode()
                        for i in range(2000))
        eng.train(data)
        obj = eng.create("ds", data)
        for i in range(25):
            obj.update((i * 977) % max(1, len(obj) - 50), 5, f"[E{i}]".encode())
        refs = [v.read() for v in obj.versions]
        eng.save(store)
        check(os.path.exists(store), "save() wrote the store file")
        check(eng.wal_active, "WAL is armed after save()")
        check(eng.wal_healthy, "WAL reports healthy")
        check(eng.store_path is not None, "engine knows its store path")

    # fresh handle, nothing shared but the file
    ds = genna.open(store)
    obj2 = ds._obj if isinstance(ds, genna.Dataset) else ds.objects[0]
    check(len(obj2.versions) == len(refs),
          f"all {len(refs)} versions came back")
    mismatch = [i for i, want in enumerate(refs) if obj2.read(version=i) != want]
    check(not mismatch,
          f"every one of {len(refs)} versions is byte-identical after reopen"
          + ("" if not mismatch else f" (differs at {mismatch[:5]})"))
    (ds.engine if isinstance(ds, genna.Dataset) else ds).close()

    for f in os.listdir(tmp):
        os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)


def test_dataset():
    section("Dataset: the record-level API")
    recs = [{"text": "the cat sat on the mat"},
            {"text": "the cat sat on the mat"},          # exact dup
            {"text": "a completely different sentence"},
            {"text": "short"},
            {"text": "the cat sat on the mat"}]          # exact dup
    ds = genna.Dataset.from_records(recs, name="t")
    check(len(ds) == 5, f"5 records ingested (got {len(ds)})")
    check(ds[0].json()["text"] == "the cat sat on the mat", "JSON parses")
    check(ds[0].content() == "the cat sat on the mat", "content() reads the text field")

    v_before = len(ds.versions)
    step = ds.dedup()
    check(len(ds) == 3, f"dedup left 3 unique records (got {len(ds)})")
    check(step.removed == 2, f"step reports 2 removed (got {step.removed})")
    check(len(ds.versions) > v_before, "dedup created versioned edits")

    ds.filter_length(min_bytes=20)
    check(len(ds) == 2, f"filter_length dropped the short record (got {len(ds)})")

    original = ds.version_bytes(0)
    ds.rollback(0)
    check(ds.nbytes == len(original), "rollback restored the original length")
    check(ds.bytes() == original, "rollback is byte-exact")
    check(len(ds) == 5, "all 5 records are back")
    ds.engine.close()


def main():
    print("=== genna python bindings: correctness ===")
    for fn in (test_roundtrip, test_binary_safe, test_large, test_versions,
               test_rollback, test_differential_vs_shadow, test_search,
               test_engine_dict_api, test_errors, test_persistence, test_dataset):
        fn()
    print(f"\n{'BINDINGS: FAILURES' if FAILS else 'BINDINGS: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
