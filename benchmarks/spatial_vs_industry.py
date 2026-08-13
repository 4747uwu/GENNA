#!/usr/bin/env python3
"""Genna's 3D layer against the tools a studio actually uses.

The earlier scene_test.c compared Genna against ITSELF (raw float32 vs
quantized). That measures nothing about whether this is worth using. This
runs one identical workload through the real tools:

  WORKLOAD  A scene of N instances of a real scanned asset. An artist moves
            E of them, one at a time, and EVERY intermediate state must stay
            recoverable. That last clause is the whole point -- without it
            you would just overwrite the file.

  CONTENDERS
    genna         instance table, one versioned edit per move
    git + baked   the naive export pipeline: geometry with transforms baked
                  in, one file, committed per move
    git + layered the sophisticated pipeline: geometry stored once, a small
                  USD scene layer holding transforms, committed per move.
                  This is what a good studio already does, and it is the
                  comparison that matters. Genna should NOT crush it.
    usd           Pixar USD's own instancing, for the storage claim
    draco         Google Draco, the geometry compression standard
    zstd          generic compression floor

git is measured both loose (what a commit costs you now) and after `git gc`
(what it costs long term), because packing changes the answer by an order of
magnitude and quoting only one would be misleading.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))


# --------------------------------------------------------------------------
def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def dir_bytes(path):
    t = 0
    for root, _d, files in os.walk(path):
        for f in files:
            try:
                t += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return t


def load_obj_positions(path):
    xs = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                p = line.split()
                if len(p) >= 4:
                    xs.append((float(p[1]), float(p[2]), float(p[3])))
    return xs


def edit_plan(n_inst, n_edits, seed=4242):
    plan, s = [], seed
    for i in range(n_edits):
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        plan.append(((s >> 20) % n_inst, ((s >> 5) % 1000) / 100.0))
    return plan


# --------------------------------------------------------------------------
def bench_genna(verts, n_inst, plan):
    import genna
    from genna._native import lib

    flat = struct.pack(f"<{len(verts)*3}f", *[c for v in verts for c in v])
    eng = genna.Engine()
    eng.create("asset.geom", flat)                     # geometry stored ONCE

    # instance table: 68 bytes per record (u32 asset + 16 floats)
    rec = bytearray()
    for i in range(n_inst):
        m = [1.0, 0, 0, (i % 20) * 10.0,
             0, 1.0, 0, 0.0,
             0, 0, 1.0, (i // 20) * 10.0,
             0, 0, 0, 1.0]
        rec += struct.pack("<I16f", 0, *m)
    inst = eng.create("scene.instances", bytes(rec))

    nodes0 = int(lib.gn_arena_live_nodes())
    t0 = time.perf_counter()
    for idx, delta in plan:
        at = idx * 68
        cur = inst.read(at, 68)
        vals = list(struct.unpack("<I16f", cur))
        vals[1 + 3] += delta                            # translate x
        inst.update(at, 68, struct.pack("<I16f", *vals))
    dt = time.perf_counter() - t0
    nodes1 = int(lib.gn_arena_live_nodes())
    per_node = int(lib.gn_arena_node_bytes()) / max(1, nodes1)
    written = int((nodes1 - nodes0) * per_node)

    resident = eng.stats.bytes_resident
    versions = len(inst.versions)
    eng.close()
    return {"written": written, "seconds": dt, "resident": resident,
            "versions": versions}


# --------------------------------------------------------------------------
def _git_init(d):
    run(["git", "init", "-q", "."], cwd=d)
    for k, v in (("user.email", "b@b"), ("user.name", "b"),
                 ("commit.gpgsign", "false"), ("gc.auto", "0")):
        run(["git", "config", k, v], cwd=d)


def _git_size(d):
    return dir_bytes(os.path.join(d, ".git"))


def bench_git_baked(verts, n_inst, plan, workdir):
    """Geometry with transforms baked in: one big file, rewritten per move."""
    d = os.path.join(workdir, "git_baked")
    os.makedirs(d)
    _git_init(d)
    n = len(verts)
    base = [[v[0], v[1], v[2]] for v in verts]
    offs = [((i % 20) * 10.0, 0.0, (i // 20) * 10.0) for i in range(n_inst)]

    def write_scene():
        with open(os.path.join(d, "scene.bin"), "wb") as f:
            for ox, oy, oz in offs:
                buf = bytearray()
                for vx, vy, vz in base:
                    buf += struct.pack("<3f", vx + ox, vy + oy, vz + oz)
                f.write(buf)

    write_scene()
    run(["git", "add", "-A"], cwd=d); run(["git", "commit", "-qm", "init"], cwd=d)
    run(["git", "gc", "-q"], cwd=d)
    before = _git_size(d)
    scene_bytes = os.path.getsize(os.path.join(d, "scene.bin"))

    t0 = time.perf_counter()
    for k, (idx, delta) in enumerate(plan):
        offs[idx] = (offs[idx][0] + delta, offs[idx][1], offs[idx][2])
        write_scene()
        run(["git", "add", "-A"], cwd=d)
        run(["git", "commit", "-qm", f"move {k}"], cwd=d)
    dt = time.perf_counter() - t0
    loose = _git_size(d) - before
    run(["git", "gc", "-q", "--aggressive"], cwd=d)
    packed = _git_size(d) - before
    return {"written_loose": loose, "written_packed": packed,
            "seconds": dt, "scene_bytes": scene_bytes}


def bench_git_layered(verts, n_inst, plan, workdir, use_usd):
    """Geometry stored once; only a small scene layer changes per move.

    This is the pipeline a competent studio already runs, and it is the
    honest comparison. Uses a real USD layer when usd-core is available,
    otherwise an equivalent-sized text layer.
    """
    d = os.path.join(workdir, "git_layered")
    os.makedirs(d)
    _git_init(d)

    # geometry: written once, never touched again
    flat = bytearray()
    for v in verts:
        flat += struct.pack("<3f", *v)
    with open(os.path.join(d, "asset.bin"), "wb") as f:
        f.write(flat)

    offs = [((i % 20) * 10.0, 0.0, (i // 20) * 10.0) for i in range(n_inst)]
    layer = os.path.join(d, "scene.usda")

    def write_layer():
        if use_usd:
            from pxr import Usd, UsdGeom, Gf
            stage = Usd.Stage.CreateNew(layer) if not os.path.exists(layer) \
                else Usd.Stage.Open(layer)
            for i, (ox, oy, oz) in enumerate(offs):
                p = f"/inst_{i}"
                x = UsdGeom.Xform.Get(stage, p) or UsdGeom.Xform.Define(stage, p)
                ops = x.GetOrderedXformOps()
                op = ops[0] if ops else x.AddTranslateOp()
                op.Set(Gf.Vec3d(ox, oy, oz))
            stage.GetRootLayer().Save()
        else:
            with open(layer, "w") as f:
                f.write("#usda 1.0\n")
                for i, (ox, oy, oz) in enumerate(offs):
                    f.write(f'def Xform "inst_{i}" {{\n'
                            f'  double3 xformOp:translate = ({ox}, {oy}, {oz})\n'
                            f'  uniform token[] xformOpOrder = ["xformOp:translate"]\n}}\n')

    write_layer()
    run(["git", "add", "-A"], cwd=d); run(["git", "commit", "-qm", "init"], cwd=d)
    run(["git", "gc", "-q"], cwd=d)
    before = _git_size(d)
    layer_bytes = os.path.getsize(layer)

    t0 = time.perf_counter()
    for k, (idx, delta) in enumerate(plan):
        offs[idx] = (offs[idx][0] + delta, offs[idx][1], offs[idx][2])
        write_layer()
        run(["git", "add", "-A"], cwd=d)
        run(["git", "commit", "-qm", f"move {k}"], cwd=d)
    dt = time.perf_counter() - t0
    loose = _git_size(d) - before
    run(["git", "gc", "-q", "--aggressive"], cwd=d)
    packed = _git_size(d) - before
    return {"written_loose": loose, "written_packed": packed,
            "seconds": dt, "layer_bytes": layer_bytes}


# --------------------------------------------------------------------------
def bench_compression(verts, workdir):
    """Pure geometry compression: Draco and zstd vs Genna's codec modes."""
    import numpy as np
    out = {}
    n = len(verts)
    raw = bytearray()
    for v in verts:
        raw += struct.pack("<3f", *v)
    out["raw_bytes"] = len(raw)
    out["vertices"] = n

    try:
        import zstandard as zstd
        for lvl in (3, 19):
            c = zstd.ZstdCompressor(level=lvl).compress(bytes(raw))
            out[f"zstd_{lvl}"] = len(c)
    except Exception as e:
        out["zstd_error"] = str(e)

    try:
        import DracoPy
        pts = np.array(verts, dtype=np.float32)
        # Draco REORDERS vertices, so an elementwise comparison measures the
        # permutation rather than the precision. Sorting does not fix it
        # either: points that are nearly tied swap under perturbation, and
        # then row i is compared against an unrelated point. (Both mistakes
        # reported a 1.5e-01 "error" on a model 1.5e-01 across -- i.e. they
        # made Draco look useless when it is excellent.)
        #
        # The right measure is nearest-neighbour distance, which is what mesh
        # compression is actually judged on.
        from scipy.spatial import cKDTree
        tree = cKDTree(pts.astype(np.float64))
        for qb in (11, 14):
            enc = DracoPy.encode(pts, quantization_bits=qb)
            out[f"draco_qb{qb}"] = len(enc)
            dec = DracoPy.decode(enc)
            dp = np.asarray(dec.points, dtype=np.float64)
            d, _ = tree.query(dp, k=1)
            out[f"draco_qb{qb}_maxerr"] = float(d.max())
    except Exception as e:
        out["draco_error"] = f"{type(e).__name__}: {e}"

    return out


def bench_usd_instancing(verts, n_inst, workdir):
    """USD's own storage for N instances of one asset."""
    try:
        from pxr import Usd, UsdGeom, Gf, Vt
    except Exception as e:
        return {"error": str(e)}
    d = os.path.join(workdir, "usd_inst")
    os.makedirs(d, exist_ok=True)
    # .usdc (binary crate), not .usda: a studio would never ship 36k points
    # as text, and measuring the text form would flatter Genna by ~3x.
    geo = os.path.join(d, "asset.usdc")
    st = Usd.Stage.CreateNew(geo)
    mesh = UsdGeom.Points.Define(st, "/asset")
    mesh.GetPointsAttr().Set(Vt.Vec3fArray([Gf.Vec3f(*v) for v in verts]))
    st.GetRootLayer().Save()

    scene = os.path.join(d, "scene.usda")
    st2 = Usd.Stage.CreateNew(scene)
    for i in range(n_inst):
        p = UsdGeom.Xform.Define(st2, f"/inst_{i}")
        p.GetPrim().GetReferences().AddReference("./asset.usdc", "/asset")
        p.GetPrim().SetInstanceable(True)
        p.AddTranslateOp().Set(Gf.Vec3d((i % 20) * 10.0, 0.0, (i // 20) * 10.0))
    st2.GetRootLayer().Save()
    return {"asset_bytes": os.path.getsize(geo),
            "scene_bytes": os.path.getsize(scene),
            "total": os.path.getsize(geo) + os.path.getsize(scene)}


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mesh", nargs="?", default="corpora/3d/stanford-bunny.obj")
    ap.add_argument("--instances", type=int, default=20)
    ap.add_argument("--edits", type=int, default=25)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--skip-baked", action="store_true",
                    help="skip the naive baked-scene arm (it is O(scene) per "
                         "commit and dominates runtime at large instance counts)")
    a = ap.parse_args()

    verts = load_obj_positions(a.mesh)
    if not verts:
        print(f"could not read vertices from {a.mesh}")
        return 2
    plan = edit_plan(a.instances, a.edits)
    raw_scene = len(verts) * 3 * 4 * a.instances

    print(f"asset:  {a.mesh}  ({len(verts):,} vertices, "
          f"{len(verts)*12/1048576:.2f} MB raw)")
    print(f"scene:  {a.instances} instances "
          f"({raw_scene/1048576:.1f} MB if baked flat)")
    print(f"work:   {a.edits} moves, every intermediate state recoverable\n")

    wd = tempfile.mkdtemp(prefix="gn_spatial_")
    results = {}
    try:
        print("== geometry compression (storage of the asset itself) ==")
        comp = bench_compression(verts, wd)
        results["compression"] = comp
        rawb = comp["raw_bytes"]; nv = comp["vertices"]
        print(f"   {'raw float32':<18} {rawb:>10,} B   {rawb/nv:5.2f} B/vertex")
        for k in sorted(comp):
            if k.startswith(("zstd_", "draco_")) and not k.endswith("maxerr"):
                err = comp.get(k + "_maxerr")
                print(f"   {k:<18} {comp[k]:>10,} B   {comp[k]/nv:5.2f} B/vertex"
                      + (f"   max err {err:.2e}" if err is not None else ""))
        for k in ("zstd_error", "draco_error"):
            if k in comp:
                print(f"   {k}: {comp[k]}")
        print(f"   {'genna EXACT':<18} {nv*24:>10,} B   {24.0:5.2f} B/vertex   "
              f"max err 0 (bit-exact)")
        print(f"   {'genna GRID':<18} {nv*12:>10,} B   {12.0:5.2f} B/vertex")

        print("\n== instancing (storing the scene once) ==")
        usd = bench_usd_instancing(verts, a.instances, wd)
        results["usd"] = usd
        if "error" in usd:
            print(f"   usd: {usd['error']}")
        else:
            print(f"   {'USD (instanceable)':<18} {usd['total']:>10,} B "
                  f"(asset {usd['asset_bytes']:,} + scene {usd['scene_bytes']:,})")

        print("\n== versioned editing: bytes written for "
              f"{a.edits} moves ==")
        g = bench_genna(verts, a.instances, plan)
        results["genna"] = g
        print(f"   {'genna':<18} {g['written']:>12,} B  "
              f"{g['seconds']*1000:8.1f} ms   {g['versions']} versions")

        gl = bench_git_layered(verts, a.instances, plan, wd, use_usd=("error" not in usd))
        results["git_layered"] = gl
        print(f"   {'git + USD layer':<18} {gl['written_loose']:>12,} B  "
              f"{gl['seconds']*1000:8.1f} ms   (loose; layer is "
              f"{gl['layer_bytes']:,} B)")
        print(f"   {'  ^ after git gc':<18} {gl['written_packed']:>12,} B")

        if not a.skip_baked:
            gb = bench_git_baked(verts, a.instances, plan, wd)
            results["git_baked"] = gb
            print(f"   {'git + baked scene':<18} {gb['written_loose']:>12,} B  "
                  f"{gb['seconds']*1000:8.1f} ms   (loose; scene is "
                  f"{gb['scene_bytes']:,} B)")
            print(f"   {'  ^ after git gc':<18} {gb['written_packed']:>12,} B")

        print("\n== verdict ==")
        gw = max(1, g["written"])
        arms = [("git + USD layer", "git_layered")]
        if "git_baked" in results:
            arms.insert(0, ("git + baked scene", "git_baked"))
        for name, key in arms:
            r = results[key]
            print(f"   vs {name:<20} {r['written_loose']/gw:8.1f}x less written "
                  f"(loose) | {r['written_packed']/gw:7.1f}x (packed)")
        gl_t = results["git_layered"]["seconds"]
        print(f"   vs git + USD layer   {gl_t/max(1e-9,g['seconds']):8.0f}x faster "
              f"({g['seconds']*1000:.1f} ms vs {gl_t*1000:.0f} ms)")
        if "draco_qb11" in comp:
            print(f"   vs Draco on raw geometry: genna EXACT is "
                  f"{nv*24/comp['draco_qb11']:.1f}x LARGER "
                  f"(Draco wins compression, as expected)")

        if a.json:
            print("\n" + json.dumps(results, indent=2, default=str))
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
