"""HyperLogLog, sized so it can live in a version manifest.

The point of putting a sketch beside a version is that "how many distinct
values did this column have at version 1,847?" becomes a lookup instead of a
scan. The sketch is fixed-size (1 KB at p=10) and **mergeable**, which is what
makes it a legitimate aggregate: the union of two sketches is the register-wise
maximum, so it composes the same way a subtree aggregate would.

Honest about the trade: this moves cost from read to write. Computing the
sketch is O(n) when a column changes; the saving is that you pay it once per
version instead of once per question. If you never ask, you have lost.

Accuracy is the standard HLL bound, ~1.04/sqrt(m) relative error -- about 3.25%
at p=10. Verified against exact counts in tests/test_sketch.py rather than
quoted from the paper.
"""
from __future__ import annotations

import base64
import hashlib
import struct

__all__ = ["HLL"]

_P = 10                      # 2^10 = 1024 registers = 1 KB
_M = 1 << _P
_ALPHA = 0.7213 / (1.0 + 1.079 / _M)


def _hash64(v) -> int:
    if isinstance(v, bytes):
        b = v
    elif isinstance(v, str):
        b = v.encode("utf-8")
    elif v is None:
        b = b"\x00__none__"
    elif isinstance(v, bool):
        b = b"\x01" if v else b"\x00"
    elif isinstance(v, int):
        b = b"i" + struct.pack("<q", v)
    elif isinstance(v, float):
        b = b"f" + struct.pack("<d", v)
    else:
        b = ("o" + repr(v)).encode("utf-8", "replace")
    return int.from_bytes(hashlib.blake2b(b, digest_size=8).digest(), "little")


class HLL:
    """Fixed-size distinct-value estimator. 1 KB, mergeable, serializable."""

    __slots__ = ("reg",)

    def __init__(self, reg: bytearray | None = None):
        self.reg = reg if reg is not None else bytearray(_M)

    def add(self, v) -> None:
        h = _hash64(v)
        idx = h & (_M - 1)
        w = h >> _P
        # rank = position of the first set bit, 1-based; 0 means "none in 54"
        rank = 1
        while w and not (w & 1):
            w >>= 1
            rank += 1
        if not w:
            rank = 54
        if rank > self.reg[idx]:
            self.reg[idx] = rank

    def add_many(self, values) -> "HLL":
        for v in values:
            self.add(v)
        return self

    def merge(self, other: "HLL") -> "HLL":
        """Register-wise max. This is what makes it a monoid: merging is
        associative and commutative, so sketches of two halves combine into
        the sketch of the whole without seeing the data again."""
        for i in range(_M):
            if other.reg[i] > self.reg[i]:
                self.reg[i] = other.reg[i]
        return self

    def estimate(self) -> int:
        z = 0.0
        zeros = 0
        for r in self.reg:
            z += 1.0 / (1 << r)
            if r == 0:
                zeros += 1
        e = _ALPHA * _M * _M / z
        if e <= 2.5 * _M and zeros:
            import math
            e = _M * math.log(_M / zeros)          # small-range correction
        return int(round(e))

    # -- serialization: goes into a JSON manifest, so base64 ---------------
    def dumps(self) -> str:
        import zlib
        return base64.b64encode(zlib.compress(bytes(self.reg), 6)).decode("ascii")

    @staticmethod
    def loads(s: str) -> "HLL":
        import zlib
        return HLL(bytearray(zlib.decompress(base64.b64decode(s))))

    def __len__(self) -> int:
        return self.estimate()

    def __repr__(self) -> str:
        return f"<HLL ~{self.estimate():,} distinct, {_M} registers>"
