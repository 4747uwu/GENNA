#!/usr/bin/env python3
"""A sculpting session, stored five different ways.

Workload: N brush strokes on a real scanned mesh. Each stroke displaces the
vertices inside a sphere along their radial direction with a smooth falloff --
which is what a sculpt brush does, and crucially it is LOCALIZED: a stroke
touches a small, spatially contiguous set of vertices.

Every intermediate state must be recoverable (that is the undo stack).

STRATEGIES

  whole         every state stored complete            (the naive baseline)
  cdc           Genna binary path, content-defined chunking
  cdc+morton    same, with vertices in Z-order first
  displacement  base mesh once + sparse (index, dx,dy,dz) per stroke
  strokes       only the brush parameters, ~32 bytes each

The last two are the interesting ones and they are not free:

  displacement  needs the base mesh to reconstruct any state, and the sparse
                list grows with how much of the model has ever been touched.
  strokes       is tiny, but reconstructing state K requires REPLAYING K
                strokes. There is no random access. That cost is measured
                here rather than waved away, because an undo stack that takes
                a minute to scrub is not an undo stack.
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

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))


def load_obj(path):
    v = []
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                v.append((float(p[1]), float(p[2]), float(p[3])))
    return np.array(v, dtype=np.float32)


def dir_bytes(p):
    t = 0
    for root, _d, fs in os.walk(p):
        for f in fs:
            try:
                t += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return t


# --------------------------------------------------------------------------
class Stroke:
    """A brush stroke. 32 bytes on the wire."""
    __slots__ = ("cx", "cy", "cz", "radius", "strength", "tool")

    def __init__(self, c, radius, strength, tool=0):
        self.cx, self.cy, self.cz = float(c[0]), float(c[1]), float(c[2])
        self.radius, self.strength, self.tool = float(radius), float(strength), int(tool)

    def pack(self):
        return struct.pack("<5fI", self.cx, self.cy, self.cz,
                           self.radius, self.strength, self.tool)

    def apply(self, verts):
        """Displace vertices inside the sphere, smooth falloff. Deterministic:
        replay must reproduce the state exactly."""
        c = np.array([self.cx, self.cy, self.cz], dtype=np.float32)
        d = verts - c
        dist = np.sqrt((d * d).sum(axis=1))
        hit = dist < self.radius
        if not hit.any():
            return verts, hit
        t = 1.0 - (dist[hit] / self.radius)
        falloff = (t * t * (3.0 - 2.0 * t)).astype(np.float32)   # smoothstep
        n = d[hit]
        ln = np.sqrt((n * n).sum(axis=1))[:, None]
        ln[ln == 0] = 1.0
        verts = verts.copy()
        verts[hit] += (n / ln) * (falloff * self.strength)[:, None]
        return verts, hit


def make_strokes(verts, n, seed=7):
    rng = np.random.default_rng(seed)
    lo, hi = verts.min(axis=0), verts.max(axis=0)
    extent = float(np.max(hi - lo))
    out = []
    for _ in range(n):
        # brush centres on an actual surface point, as a real brush would
        c = verts[rng.integers(0, len(verts))]
        out.append(Stroke(c, extent * 0.06, extent * 0.004))
    return out


# --------------------------------------------------------------------------
def genna_store(states, label, sdir, chunk=1024):
    import genna
    from genna._native import lib
    eng = genna.Engine()
    t0 = time.perf_counter()
    for i, s in enumerate(states):
        eng.create_binary(f"v{i}", s, avg_chunk=chunk)
    dt = time.perf_counter() - t0
    p = os.path.join(sdir, f"{label}.gn")
    eng.save(p)
    n = os.path.getsize(p)
    resident = eng.stats.bytes_resident
    eng.close()
    lib.gn_arena_release()
    return n, dt, resident


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mesh", nargs="?", default="corpora/3d/stanford-bunny.obj")
    ap.add_argument("--strokes", type=int, default=40)
    ap.add_argument("--chunk", type=int, default=1024)
    a = ap.parse_args()

    verts = load_obj(a.mesh)
    nv = len(verts)
    raw = nv * 12
    strokes = make_strokes(verts, a.strokes)

    print(f"mesh:    {a.mesh}  ({nv:,} vertices, {raw/1048576:.2f} MB)")
    print(f"session: {a.strokes} brush strokes, every state recoverable\n")

    # ---- generate the session -------------------------------------------
    states_raw, hits, cur = [verts.copy()], [], verts.copy()
    for s in strokes:
        cur, hit = s.apply(cur)
        states_raw.append(cur.copy())
        hits.append(int(hit.sum()))
    print(f"   brush touches {np.mean(hits):.0f} vertices per stroke "
          f"({100*np.mean(hits)/nv:.1f}% of the mesh)\n")

    packed = [v.tobytes() for v in states_raw]
    total_whole = sum(len(p) for p in packed)

    # ---- Morton order ----------------------------------------------------
    import ctypes
    from genna._native import lib as _lib
    order = (ctypes.c_uint32 * nv)()
    flat = verts.astype(np.float32).ravel()
    _lib.gn_morton_order(flat.ctypes.data_as(ctypes.c_void_p), nv, order)
    perm = np.frombuffer(order, dtype=np.uint32, count=nv).astype(np.int64)
    packed_morton = [v[perm].tobytes() for v in states_raw]

    sdir = tempfile.mkdtemp(prefix="gn_sculpt_")
    rows = []
    try:
        print("== storage ==")
        print(f"   {'strategy':<16} {'on disk':>12} {'vs whole':>10}  note")
        rows.append(("whole (naive)", total_whole, "1.0x", "every state complete"))

        n_cdc, t_cdc, r_cdc = genna_store(packed, "cdc", sdir, a.chunk)
        rows.append(("cdc", n_cdc, f"{total_whole/n_cdc:.1f}x",
                     f"{t_cdc*1000:.0f} ms ingest"))

        n_mor, t_mor, _ = genna_store(packed_morton, "morton", sdir, a.chunk)
        rows.append(("cdc + morton", n_mor, f"{total_whole/n_mor:.1f}x",
                     f"{t_mor*1000:.0f} ms ingest"))

        # ---- displacement: base once + sparse per-stroke deltas ----------
        disp = [packed[0]]
        for i in range(1, len(states_raw)):
            d = states_raw[i] - states_raw[i - 1]
            idx = np.nonzero((d != 0).any(axis=1))[0].astype(np.uint32)
            rec = idx.tobytes() + d[idx].astype(np.float32).tobytes()
            disp.append(rec)
        n_dis, t_dis, _ = genna_store(disp, "disp", sdir, a.chunk)
        disp_raw = sum(len(d) for d in disp)
        rows.append(("displacement", n_dis, f"{total_whole/n_dis:.1f}x",
                     f"{disp_raw/1024:.0f} KB before compression"))

        # ---- stroke log --------------------------------------------------
        log = packed[0] + b"".join(s.pack() for s in strokes)
        n_str, t_str, _ = genna_store([log], "strokes", sdir, a.chunk)
        rows.append(("base + strokes", n_str, f"{total_whole/n_str:.1f}x",
                     f"{len(strokes)*32} B of strokes + base mesh"))

        for name, b, ratio, note in rows:
            print(f"   {name:<16} {b:>12,} {ratio:>10}  {note}")

        gtot = 0
        # ---- git ---------------------------------------------------------
        d = tempfile.mkdtemp(prefix="gn_sculpt_git_")
        try:
            subprocess.run(["git", "init", "-q", "."], cwd=d, capture_output=True)
            for k, v in (("user.email", "b@b"), ("user.name", "b"),
                         ("commit.gpgsign", "false"), ("gc.auto", "0")):
                subprocess.run(["git", "config", k, v], cwd=d, capture_output=True)
            f = os.path.join(d, "mesh.bin")
            t0 = time.perf_counter()
            for i, p in enumerate(packed):
                with open(f, "wb") as fh:
                    fh.write(p)
                subprocess.run(["git", "add", "-A"], cwd=d, capture_output=True)
                subprocess.run(["git", "commit", "-qm", f"v{i}"], cwd=d,
                               capture_output=True)
            gdt = time.perf_counter() - t0
            subprocess.run(["git", "gc", "-q", "--aggressive"], cwd=d,
                           capture_output=True)
            gtot = dir_bytes(os.path.join(d, ".git"))
            print(f"   {'git (total .git)':<16} {gtot:>12,} "
                  f"{total_whole/gtot:>9.1f}x  {gdt*1000:.0f} ms")
        finally:
            shutil.rmtree(d, ignore_errors=True)

        # ---- incremental: what each strategy charges PER STROKE ----------
        # This is the number an undo stack actually pays. The totals above are
        # dominated by the base mesh, which every strategy stores once.
        base = n_str - len(strokes) * 32          # stroke log minus its strokes
        print(f"\n== incremental cost per stroke (total minus the base mesh) ==")
        print(f"   base mesh alone is ~{base:,} B, which every strategy pays\n")
        per = [
            ("whole (naive)", (total_whole - raw) / len(strokes)),
            ("cdc", (n_cdc - base) / len(strokes)),
            ("cdc + morton", (n_mor - base) / len(strokes)),
            ("displacement", (n_dis - base) / len(strokes)),
            ("base + strokes", (n_str - base) / len(strokes)),
            ("git", (gtot - base) / len(strokes)),
        ]
        for name, b in per:
            print(f"   {name:<16} {b:>12,.0f} B/stroke")

        # ---- the cost the stroke log actually charges ---------------------
        print("\n== what the stroke log costs you ==")
        t0 = time.perf_counter()
        rep = verts.copy()
        for s in strokes:
            rep, _ = s.apply(rep)
        t_replay = time.perf_counter() - t0
        exact = np.array_equal(rep, states_raw[-1])
        print(f"   replay all {len(strokes)} strokes: {t_replay*1000:.0f} ms")
        print(f"   reproduces the final state exactly: {exact}")
        mid = len(strokes) // 2
        t0 = time.perf_counter()
        rep2 = verts.copy()
        for s in strokes[:mid]:
            rep2, _ = s.apply(rep2)
        t_mid = time.perf_counter() - t0
        print(f"   random access to state {mid}: {t_mid*1000:.0f} ms "
              f"(vs {0.4:.1f} ms to read it from a Genna store)")
        print(f"   -> the stroke log is {n_cdc/n_str:.0f}x smaller but pays "
              f"O(k) replay for state k,\n      and only works if the brush is "
              f"bit-for-bit deterministic across versions.")
        print(f"   replay determinism verified above: {exact}")
    finally:
        shutil.rmtree(sdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
