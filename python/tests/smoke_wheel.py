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

    print("wheel ok: %s on %s %s" % (
        getattr(genna, "__version__", "?"), sys.platform,
        "%d.%d" % sys.version_info[:2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
