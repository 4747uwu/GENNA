"""The check every built wheel must pass, on the platform it was built for.

Deliberately more than `import genna`. A wheel whose shared library loads but
cannot round-trip a store through disk is not a working wheel, and an import
check would call it green -- the same vacuous-pass failure this project has
been bitten by before. So this edits, reads back, saves, reopens in the same
process, and compares bytes.

Run by cibuildwheel inside the target environment, and standalone by CI.
"""
import os
import sys
import tempfile

import genna


def main() -> int:
    e = genna.Engine()
    o = e.create("d", b"hello world")

    # a splice, since O(log n) editing is the whole point
    o.update(0, 5, b"HELLO")
    got = o.read()
    assert got == b"HELLO world", got

    # history is retained, not overwritten
    assert len(o.versions) == 2, len(o.versions)
    assert o.versions[0].bytes() == b"hello world", o.versions[0].bytes()

    # and it survives a real trip through the filesystem
    d = tempfile.mkdtemp()
    p = os.path.join(d, "smoke.gn")
    e.save(p)
    e.close()

    e2 = genna.open_store(p)
    o2 = e2["d"]
    assert o2.read() == b"HELLO world", o2.read()
    assert o2.versions[0].bytes() == b"hello world", "history lost on reload"
    e2.close()

    # ---- which compression made it into THIS wheel? ---------------------
    # cibuildwheel's before-all installs zlib and tries for zstd, and for a
    # long time it ended in `|| true` -- so whether compression was actually
    # in the wheel was a comment in pyproject.toml, not a fact anyone checked.
    # It matters beyond size: a store is only readable by a build that has the
    # codec it was written with, so two wheels of the same release that
    # disagree here cannot open each other's files.
    #
    # Detected rather than asked, because the engine has no capability
    # accessor: save something compressible and read the codec out of the
    # header.
    from genna.core import _payload_codec

    d2 = tempfile.mkdtemp()
    p2 = os.path.join(d2, "codec.gn")
    e3 = genna.Engine()
    e3.create("c", b"the quick brown fox jumps over the lazy dog. " * 2000)
    e3.save(p2)
    e3.close()
    codec = _payload_codec(p2)

    print("wheel ok: %s on %s %s | format v%d | compression: %s" % (
        getattr(genna, "__version__", "?"), sys.platform,
        "%d.%d" % sys.version_info[:2], genna.format_version(),
        codec or "NONE (raw)"))

    if codec is None:
        print("FAIL: this wheel has no compression at all. A store it writes "
              "is readable anywhere, but it forfeits the whole-payload pass, "
              "and it means the build found neither zlib nor zstd -- on an "
              "image where zlib is supposed to be a base package.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
