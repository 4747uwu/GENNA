"""The on-disk format is a promise. This is the thing that keeps it.

A storage engine that cannot read last month's files is disqualifying, and
format drift is silent by nature: nothing fails at the moment you change the
layout, only later, on someone else's machine, holding data you cannot
recover.

So a store written by an older build is committed as a fixture, and this
opens it and compares every byte against a manifest recorded when it was
made. If the format changes, this fails immediately, on the machine that
changed it, and the choices are visible: write a converter, or bump the
version deliberately and regenerate the fixture with a commit message saying
why.

Regenerate with:  python tests/test_format_stability.py --write-fixture
which is deliberately manual. A fixture that regenerates itself would pass
forever and pin nothing -- the exact failure mode this repository keeps
finding.
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import genna

HERE = Path(__file__).resolve().parent
FIXTURE_DIR = HERE / "fixtures"
FIXTURE = FIXTURE_DIR / "format_v3.genna"
MANIFEST = FIXTURE_DIR / "format_v3.json"

FAILS, COUNT = [], 0

# The exact content the fixture was built from. Changing any of this without
# regenerating the fixture is itself a test failure, which is the point.
DOCS = {
    "readme": [
        b"Genna stores data as content-addressed chunks in a persistent tree.\n",
        b"An edit copies the path to the change and shares everything else.\n",
        b"So a new version costs the size of the change, not of the data.\n",
    ],
    "unicode": [
        "ascii, then CJK 漢字仮名, then RTL العربية, then emoji \U0001f9ea\n".encode("utf-8"),
        "second version with more 漢字 and \U0001f680\n".encode("utf-8"),
    ],
    "binary": [bytes(range(256)) * 4, bytes(range(255, -1, -1)) * 4],
}


def check(cond, msg):
    global COUNT
    COUNT += 1
    print(f"  {'ok  ' if cond else 'FAIL'}  {msg}")
    if not cond:
        FAILS.append(msg)
    return bool(cond)


def build_store(path: Path) -> dict:
    """Write the fixture store and return the manifest describing it."""
    eng = genna.Engine()
    man: dict = {"format": genna.format_version(), "objects": {}}
    for name, versions in DOCS.items():
        obj = eng.create(name, versions[0])
        for later in versions[1:]:
            obj.update(0, len(obj), later)
        man["objects"][name] = [
            {"index": i,
             "len": len(v.bytes()),
             "sha256": hashlib.sha256(v.bytes()).hexdigest()}
            for i, v in enumerate(obj.versions)
        ]
    eng.save(path)
    eng.close()
    return man


def write_fixture() -> int:
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    man = build_store(FIXTURE)
    MANIFEST.write_text(json.dumps(man, indent=1, sort_keys=True),
                        encoding="utf-8")
    size = FIXTURE.stat().st_size
    print(f"wrote {FIXTURE} ({size} B), format v{man['format']}")
    print(f"wrote {MANIFEST}")
    print("Commit both. Do not regenerate them to make a test pass.")
    return 0


def main() -> int:
    print("=== on-disk format stability ===\n")

    mine = genna.format_version()
    print(f"this build writes and reads format v{mine}\n")

    print("-- the version is reported, not guessed --")
    check(isinstance(mine, int) and mine >= 1,
          f"format_version() returns a real version ({mine})")

    # ---- the fixture ----------------------------------------------------
    print("\n-- a store written by an earlier build still opens --")
    if not FIXTURE.exists() or not MANIFEST.exists():
        # Missing fixture is a FAILURE, not a skip. A pinning test with
        # nothing pinned is the vacuous-pass bug wearing a new hat.
        check(False,
              f"fixture is missing ({FIXTURE.name}). Generate it once with "
              f"`python tests/test_format_stability.py --write-fixture` and "
              f"commit it. A missing fixture pins nothing.")
    else:
        man = json.loads(MANIFEST.read_text(encoding="utf-8"))
        check(genna.store_format(FIXTURE) == man["format"],
              f"fixture reports format v{genna.store_format(FIXTURE)}, "
              f"manifest says v{man['format']}")

        # Copy it: opening arms a WAL and would touch a committed file.
        tmp = Path(tempfile.mkdtemp(prefix="genna-fmt-"))
        work = tmp / FIXTURE.name
        shutil.copy2(FIXTURE, work)

        eng = genna.open_store(work)
        check(len(eng) == len(man["objects"]),
              f"object count survives ({len(eng)} of {len(man['objects'])})")

        compared = 0
        bad = []
        for name, versions in man["objects"].items():
            if name not in eng:
                bad.append(f"{name}: object missing")
                continue
            obj = eng[name]
            if len(obj.versions) != len(versions):
                bad.append(f"{name}: {len(obj.versions)} versions, "
                           f"expected {len(versions)}")
                continue
            for want in versions:
                got = obj.versions[want["index"]].bytes()
                compared += 1
                if hashlib.sha256(got).hexdigest() != want["sha256"]:
                    bad.append(f"{name} v{want['index']}: bytes differ")
                elif len(got) != want["len"]:
                    bad.append(f"{name} v{want['index']}: length differs")
        eng.close()
        shutil.rmtree(tmp, ignore_errors=True)

        check(compared > 0,
              f"actually compared {compared} versions - zero would mean this "
              f"test verified nothing")
        check(not bad,
              f"every version is byte-identical to when the fixture was "
              f"written ({len(bad)} differ{': ' + bad[0] if bad else ''})")

    # ---- refusing the future --------------------------------------------
    print("\n-- a newer format must be refused by name, not misparsed --")
    tmp = Path(tempfile.mkdtemp(prefix="genna-fmt2-"))
    try:
        good = tmp / "good.genna"
        eng = genna.Engine()
        eng.create("d", b"hello format")
        eng.save(good)
        eng.close()
        check(genna.store_format(good) == mine,
              f"a store we just wrote reports v{genna.store_format(good)}")

        # Forge a future version by bumping the header field and repairing
        # the header CRC, so the file is well-formed and ONLY the version is
        # unacceptable. Without the CRC repair this would test corruption
        # detection instead, and pass for the wrong reason.
        raw = bytearray(good.read_bytes())
        future = mine + 7
        raw[8:12] = future.to_bytes(4, "little")
        import zlib
        raw[56:60] = (zlib.crc32(bytes(raw[:56])) & 0xFFFFFFFF).to_bytes(4, "little")
        ahead = tmp / "ahead.genna"
        ahead.write_bytes(bytes(raw))

        check(genna.store_format(ahead) == future,
              f"the forged store reports v{genna.store_format(ahead)} "
              f"(expected v{future}) - the forgery is well-formed")

        msg = ""
        try:
            genna.open_store(ahead)
            opened = True
        except genna.GennaError as e:
            opened, msg = False, str(e)
        check(not opened, "opening a newer-format store fails rather than "
                          "misparsing it")
        check(str(future) in msg and str(mine) in msg,
              f"and the message names both versions: {msg[:110]}...")
        check("newer" in msg.lower(),
              "and says which direction the mismatch runs")

        # not-a-store must be distinguishable from corrupt
        junk = tmp / "junk.bin"
        junk.write_bytes(b"this is not a genna store at all, not even close")
        check(genna.store_format(junk) == 0,
              "a non-Genna file reports 0, not a version and not an error")
        msg2 = ""
        try:
            genna.open_store(junk)
        except genna.GennaError as e:
            msg2 = str(e)
        check("not a Genna store" in msg2,
              f"and opening it says exactly that: {msg2[:80]}")

        # A file that IS ours but truncated is a different answer again:
        # damaged, not foreign. Three outcomes, three messages.
        stub = tmp / "truncated.genna"
        stub.write_bytes(good.read_bytes()[:40])
        check(genna.store_format(stub) == -1,
              f"a truncated Genna store reports -1 (damaged), not 0 "
              f"(got {genna.store_format(stub)})")
        msg3 = ""
        try:
            genna.open_store(stub)
        except genna.GennaError as e:
            msg3 = str(e)
        check("truncated" in msg3 or "checksum" in msg3,
              f"and opening it blames the header, not the format: "
              f"{msg3[:90]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{'FORMAT: FAILURES' if FAILS else 'FORMAT: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks)")
    for f in FAILS:
        print(f"  - {f}")
    print(f"VERDICT: {'FAIL' if FAILS else 'PASS'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    if "--write-fixture" in sys.argv:
        sys.exit(write_fixture())
    sys.exit(main())
