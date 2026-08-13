#!/usr/bin/env python3
"""Versioning geometry that CHANGES SHAPE, against git.

The instance-table benchmark tied with git+USD because transforms are tiny --
both systems were versioning a few kilobytes. This measures the case that is
actually hard, and the one content-defined chunking was built for:

    a point cloud that gains and loses points every step,
    as Gaussian-splat training does continuously (densify / prune),
    with every step recoverable.

Here the thing being versioned is megabytes of geometry, not a small transform
list, so the storage question is real. Contenders:

    genna CDC     content-defined chunking (the new binary path)
    genna fixed   fixed-size cuts, to isolate what CDC is worth
    git           the status quo for versioned assets, loose and after gc

git is given every advantage: raw binary blobs (no text bloat), and its
packed size is reported, which is where its delta compression does its work.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def dir_bytes(p):
    t = 0
    for root, _d, fs in os.walk(p):
        for f in fs:
            try:
                t += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return t


def load_obj(path):
    v = []
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                v.append((float(p[1]), float(p[2]), float(p[3])))
    return v


class Rng:
    def __init__(self, s=20260812):
        self.s = s
    def next(self):
        self.s = (self.s * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
        return self.s >> 11


def evolve(pts, rng, densify_pct=1.0, prune_pct=0.5):
    """One training step: split some points, drop some others."""
    n = len(pts)
    add = max(1, int(n * densify_pct / 100))
    at = rng.next() % max(1, n - add)
    new = [(pts[at + i][0] + 1e-3, pts[at + i][1], pts[at + i][2])
           for i in range(add)]
    pts = pts[:at + add] + new + pts[at + add:]
    drop = max(1, int(len(pts) * prune_pct / 100))
    d0 = rng.next() % max(1, len(pts) - drop)
    pts = pts[:d0] + pts[d0 + drop:]
    return pts


def pack(pts):
    return struct.pack(f"<{len(pts)*3}f", *[c for p in pts for c in p])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mesh", nargs="?", default="corpora/3d/stanford-bunny.obj")
    ap.add_argument("--steps", type=int, default=20)
    ap.add_argument("--chunk", type=int, default=4096)
    a = ap.parse_args()

    base = load_obj(a.mesh)
    if not base:
        print("no vertices"); return 2

    # the identical sequence of states for every contender
    states, pts, rng = [pack(base)], list(base), Rng()
    for _ in range(a.steps):
        pts = evolve(pts, rng)
        states.append(pack(pts))
    total_raw = sum(len(s) for s in states)

    print(f"asset: {a.mesh}  ({len(base):,} points)")
    print(f"work:  {a.steps} training steps (densify +1%, prune -0.5%)")
    print(f"       {len(states)} states, {total_raw/1048576:.1f} MB if each "
          f"were stored whole\n")

    import genna
    from genna._native import lib

    # The comparison against git must be file-vs-file. An earlier version of
    # this benchmark compared Genna's RESIDENT RAM against git's on-disk
    # .git directory, which is not a like-for-like measurement at all.
    results = {}
    sdir = tempfile.mkdtemp(prefix="gn_evo_save_")
    try:
        for label, fixed, chunk in (("genna CDC", False, a.chunk),
                                    ("genna fixed", True, a.chunk),
                                    ("genna CDC 1k", False, 1024)):
            eng = genna.Engine()
            t0 = time.perf_counter()
            for i, s in enumerate(states):
                eng.create_binary(f"v{i}", s, avg_chunk=chunk, fixed=fixed)
            dt = time.perf_counter() - t0
            resident = eng.stats.bytes_resident
            chunks, dedup = eng.chunks, eng.stats.chunks_deduped
            store = os.path.join(sdir, f"{label.replace(' ', '_')}.gn")
            eng.save(store)
            on_disk = os.path.getsize(store)
            eng.close()
            lib.gn_arena_release()

            # Genna's incremental cost, measured the same way git's is:
            # store holding v0 only, vs store holding all states.
            e0 = genna.Engine()
            e0.create_binary("v0", states[0], avg_chunk=chunk, fixed=fixed)
            s0 = os.path.join(sdir, f"{label.replace(' ', '_')}_v0.gn")
            e0.save(s0)
            base_only = os.path.getsize(s0)
            e0.close()
            lib.gn_arena_release()

            results[label] = {"resident": resident, "on_disk": on_disk,
                              "base_only": base_only,
                              "incremental": on_disk - base_only,
                              "seconds": dt, "chunks": chunks, "dedup": dedup}
            print(f"   {label:<14} {on_disk:>10,} B on disk  "
                  f"({resident/1048576:5.1f} MB resident)  {dt*1000:7.1f} ms   "
                  f"{chunks:,} chunks")
    finally:
        shutil.rmtree(sdir, ignore_errors=True)

    # ---- git -----------------------------------------------------------
    d = tempfile.mkdtemp(prefix="gn_evo_")
    try:
        run(["git", "init", "-q", "."], cwd=d)
        for k, v in (("user.email", "b@b"), ("user.name", "b"),
                     ("commit.gpgsign", "false"), ("gc.auto", "0")):
            run(["git", "config", k, v], cwd=d)
        f = os.path.join(d, "points.bin")
        with open(f, "wb") as fh:
            fh.write(states[0])
        run(["git", "add", "-A"], cwd=d); run(["git", "commit", "-qm", "v0"], cwd=d)
        run(["git", "gc", "-q"], cwd=d)
        before = dir_bytes(os.path.join(d, ".git"))
        t0 = time.perf_counter()
        for i, s in enumerate(states[1:], 1):
            with open(f, "wb") as fh:
                fh.write(s)
            run(["git", "add", "-A"], cwd=d)
            run(["git", "commit", "-qm", f"v{i}"], cwd=d)
        gdt = time.perf_counter() - t0
        loose_total = dir_bytes(os.path.join(d, ".git"))
        loose = loose_total - before
        run(["git", "gc", "-q", "--aggressive"], cwd=d)
        packed_total = dir_bytes(os.path.join(d, ".git"))
        packed = packed_total - before
        # TOTAL is the number that compares to Genna's store file. The
        # incremental figures below exclude v0 entirely -- reporting them
        # against Genna's whole-store size (as an earlier version of this
        # benchmark did) understates git's cost by the size of the base
        # object, which for float geometry is most of the file.
        print(f"   {'git TOTAL .git':<14} {packed_total:>10,} B on disk  "
              f"(after gc)          {gdt*1000:7.1f} ms")
        print(f"   {'  ^ of which v0':<14} {before:>10,} B")
        print(f"   {'  ^ 20 commits':<14} {packed:>10,} B incremental "
              f"({loose:,} loose)")
    finally:
        shutil.rmtree(d, ignore_errors=True)

    # ---- what a COMPRESSING store could achieve -------------------------
    # Genna's chunk store dedups but never compresses: an unmatched chunk is
    # stored raw. git deltas AND zlib-compresses. This measures the gap that
    # compression alone accounts for, so the conclusion is not just "git wins"
    # but "git wins because of a mechanism Genna does not have".
    zbytes = None
    try:
        import zstandard as zstd
        blob = b"".join(states)
        zbytes = len(zstd.ZstdCompressor(level=19).compress(blob))
        print(f"   {'zstd -19 (all states)':<14} {zbytes:>7,} B          "
              f"(reference: pure compression, no dedup structure)")
    except Exception as e:
        print(f"   zstd reference unavailable: {e}")

    print("\n== verdict (file on disk vs file on disk) ==")
    cdc = results["genna CDC"]["on_disk"]
    fx = results["genna fixed"]["on_disk"]
    k1 = results["genna CDC 1k"]["on_disk"]
    best = min(cdc, k1)
    print(f"   CDC vs fixed chunking  : {fx/cdc:6.2f}x smaller "
          f"({fx:,} -> {cdc:,} B)")
    print(f"   1k chunks vs {a.chunk}      : {cdc/k1:6.2f}x "
          f"{'smaller' if k1 < cdc else 'larger'} ({k1:,} B)")
    print(f"   best genna vs every state whole: {total_raw/best:6.1f}x smaller")
    print(f"\n   TOTAL vs TOTAL (the like-for-like comparison):")
    if packed_total > best:
        print(f"     genna {best:,} B vs git {packed_total:,} B "
              f"-> genna {packed_total/best:5.2f}x SMALLER")
    else:
        print(f"     genna {best:,} B vs git {packed_total:,} B "
              f"-> git {best/packed_total:5.2f}x smaller")
    inc = results["genna CDC"]["incremental"]
    b0 = results["genna CDC"]["base_only"]
    print(f"\n   INCREMENTAL (the {a.steps} edits alone, both excluding v0):")
    print(f"     genna {inc:,} B  (store {results['genna CDC']['on_disk']:,} "
          f"- v0-only {b0:,})")
    print(f"     git   {packed:,} B packed  ({loose:,} B loose)")
    if inc > 0:
        print(f"     -> git {inc/packed:5.2f}x smaller incrementally"
              if packed < inc else
              f"     -> genna {packed/inc:5.2f}x smaller incrementally")
    if zbytes:
        print(f"   best genna vs zstd -19  : "
              f"{'genna ' + format(zbytes/best, '.2f') + 'x smaller' if zbytes > best else 'zstd ' + format(best/zbytes, '.2f') + 'x smaller'}")
    gt = results["genna CDC"]["seconds"]
    print(f"   speed                   : {gdt/max(1e-9,gt):6.0f}x faster than git "
          f"({gt*1000:.0f} ms vs {gdt*1000:.0f} ms)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
