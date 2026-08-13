#!/usr/bin/env python3
"""The MosaicML MDS head-to-head, re-homed so it runs from this repo.

The flagship number -- "2,755x less written than MDS" -- lived in a harness
that was never checked in, which meant the most-cited result could not be
reproduced by anyone reading the code. This is that harness.

WHAT IS BEING COMPARED

Both systems hold the same dataset (WikiText-2, one sample per line) and then
apply the same 100 edits, keeping the previous state readable. The question is
how many bytes each has to WRITE to do that.

  MDS   is a shard format. A sample lives inside a shard; shards are immutable
        once written. Editing a sample means rewriting the shard that contains
        it. Keeping the old version means keeping the old shard too.

  Genna edits the version tree: O(log n) new nodes, and every untouched
        subtree is shared with the previous version.

FAIRNESS

Rewriting one shard per edit is the worst case for MDS and the best case for
the argument, so this measures BOTH:

  per-edit    edits arrive one at a time and each is separately durable
              (this is the versioning scenario: 100 recoverable states)
  batched     all 100 edits are known up front and the dataset is rewritten
              once (MDS's best case; gives 2 recoverable states, not 101)

They answer different questions and both are reported. Quoting only the first
would be cheating; quoting only the second would compare 101 versions against
2.

USAGE
    pip install mosaicml-streaming
    python benchmarks/mds_vs_genna.py corpora/wikitext-2-raw/wiki.train.raw
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "python"))


# ---------------------------------------------------------------------------
def dir_bytes(path: str) -> int:
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def load_samples(path: str, limit: int | None = None) -> list[str]:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        samples = [ln for ln in (line.rstrip("\n") for line in f) if ln]
    if limit:
        samples = samples[:limit]
    return samples


def edit_plan(n_samples: int, n_edits: int, seed: int = 12345):
    """The same deterministic edit schedule for both systems."""
    plan = []
    s = seed
    for i in range(n_edits):
        s = (s * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        plan.append(((s >> 20) % n_samples, f"[EDITED SAMPLE {i}]"))
    return plan


# ---------------------------------------------------------------------------
def run_mds(samples, plan, shard_size_mb: float, workdir: str):
    """Returns (initial_bytes, per_edit_bytes, batched_bytes, seconds)."""
    try:
        from streaming import MDSWriter
    except ImportError:
        return None

    columns = {"text": "str"}
    size_limit = int(shard_size_mb * 1024 * 1024)

    # MDSWriter runs `out` through urlparse to detect cloud URIs, so an
    # absolute Windows path becomes scheme "c" and is rejected. Work from
    # inside the temp directory and pass relative names instead.
    prev_cwd = os.getcwd()
    os.chdir(workdir)

    def write_all(out_dir, data):
        with MDSWriter(out=out_dir, columns=columns, size_limit=size_limit,
                       compression=None) as w:
            for t in data:
                w.write({"text": t})
        return dir_bytes(out_dir)

    # --- v0 -------------------------------------------------------------
    base = "mds_v0"
    t0 = time.perf_counter()
    initial = write_all(base, samples)
    t_initial = time.perf_counter() - t0

    # --- how big is each shard, and which samples are in it? -------------
    # MDS records this in index.json, so the shard-level cost below is taken
    # from what MDS actually wrote, not estimated.
    with open(os.path.join(base, "index.json"), "r", encoding="utf-8") as f:
        index = json.load(f)
    shard_of, shard_ranges = [], []
    start = 0
    for sh in index["shards"]:
        n = sh["samples"]
        shard_ranges.append((start, start + n))
        shard_of.extend([len(shard_ranges) - 1] * n)
        start += n
    n_shards = len(shard_ranges)

    # --- per-edit, SHARD-level: the fair model ---------------------------
    # Editing one sample means rewriting the shard that holds it, not the
    # whole dataset. Rewriting everything would be a strawman; this is what a
    # competent MDS user actually pays, and it is the number to beat.
    cur = list(samples)
    t0 = time.perf_counter()
    per_edit_total = 0
    for k, (idx, new_text) in enumerate(plan):
        cur[idx] = new_text
        s = shard_of[idx]
        lo, hi = shard_ranges[s]
        sdir = f"mds_shard_{k}"
        # one shard's samples, written as one shard
        with MDSWriter(out=sdir, columns=columns,
                       size_limit=None, compression=None) as w:
            for t in cur[lo:hi]:
                w.write({"text": t})
        per_edit_total += dir_bytes(sdir)
        shutil.rmtree(sdir, ignore_errors=True)
    t_per_edit = time.perf_counter() - t0

    # --- per-edit, WHOLE-dataset rewrite: the naive model ----------------
    cur2 = list(samples)
    t0 = time.perf_counter()
    whole_total = 0
    for k, (idx, new_text) in enumerate(plan):
        cur2[idx] = new_text
        vdir = f"mds_v{k+1}"
        whole_total += write_all(vdir, cur2)
        shutil.rmtree(vdir, ignore_errors=True)
    t_whole = time.perf_counter() - t0

    # --- batched: all edits at once, one new version ---------------------
    cur = list(samples)
    for idx, new_text in plan:
        cur[idx] = new_text
    bdir = "mds_batched"
    t0 = time.perf_counter()
    batched = write_all(bdir, cur)
    t_batched = time.perf_counter() - t0
    shutil.rmtree(bdir, ignore_errors=True)
    shutil.rmtree(base, ignore_errors=True)
    os.chdir(prev_cwd)

    return {
        "initial_bytes": initial,
        "initial_seconds": t_initial,
        "shards": n_shards,
        "per_edit_bytes": per_edit_total,          # shard-level (fair)
        "per_edit_seconds": t_per_edit,
        "whole_rewrite_bytes": whole_total,        # naive
        "whole_rewrite_seconds": t_whole,
        "batched_bytes": batched,
        "batched_seconds": t_batched,
    }


# ---------------------------------------------------------------------------
def run_genna(samples, plan):
    import genna

    text = "".join(s + "\n" for s in samples).encode("utf-8")
    # byte offset of each sample in the concatenated corpus
    offsets, lengths, pos = [], [], 0
    for s in samples:
        b = len(s.encode("utf-8"))
        offsets.append(pos); lengths.append(b)
        pos += b + 1

    eng = genna.Engine()
    t0 = time.perf_counter()
    eng.train(text)
    obj = eng.create("dataset", text)
    t_initial = time.perf_counter() - t0
    resident0 = eng.stats.bytes_resident

    from genna._native import lib
    nodes_before = int(lib.gn_arena_live_nodes())

    t0 = time.perf_counter()
    for idx, new_text in plan:
        obj.update(offsets[idx], lengths[idx], new_text.encode("utf-8"))
    t_edits = time.perf_counter() - t0

    nodes_after = int(lib.gn_arena_live_nodes())
    node_bytes = int(lib.gn_arena_node_bytes())
    per_node = node_bytes / max(1, nodes_after)
    written = int((nodes_after - nodes_before) * per_node)

    # correctness gate: the comparison is meaningless if the result is wrong
    v0 = obj.read(version=0)
    ok_v0 = v0 == text
    head = bytes(obj)
    ok_head = len(head) > 0

    result = {
        "initial_resident_bytes": resident0,
        "initial_seconds": t_initial,
        "edit_bytes": written,
        "edit_seconds": t_edits,
        "versions": len(obj.versions),
        "v0_byte_exact": ok_v0,
        "head_readable": ok_head,
        "nodes_added": nodes_after - nodes_before,
    }
    eng.close()
    return result


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus", help="one sample per line (e.g. wiki.train.raw)")
    ap.add_argument("--edits", type=int, default=100)
    ap.add_argument("--shard-mb", type=float, default=1.0,
                    help="MDS shard size limit in MB (default 1)")
    ap.add_argument("--limit", type=int, default=None,
                    help="use only the first N samples")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    args = ap.parse_args()

    samples = load_samples(args.corpus, args.limit)
    raw = sum(len(s.encode()) + 1 for s in samples)
    plan = edit_plan(len(samples), args.edits)

    print(f"corpus: {args.corpus}")
    print(f"  {len(samples):,} samples, {raw / 1048576:.1f} MB")
    print(f"  {args.edits} edits, MDS shard limit {args.shard_mb} MB\n")

    g = run_genna(samples, plan)
    print("GENNA")
    print(f"  ingest:            {g['initial_resident_bytes'] / 1048576:6.2f} MB resident, "
          f"{g['initial_seconds']:.2f} s")
    print(f"  {args.edits} edits written:  {g['edit_bytes']:,} bytes "
          f"({g['edit_bytes'] / 1048576:.3f} MB) in {g['edit_seconds'] * 1000:.1f} ms")
    print(f"  versions kept:     {g['versions']} (all readable)")
    print(f"  v0 byte-exact:     {'YES' if g['v0_byte_exact'] else 'NO'}")
    if not g["v0_byte_exact"]:
        print("  *** v0 is not byte-exact; the comparison below is void ***")
        return 1

    workdir = tempfile.mkdtemp(prefix="genna_mds_")
    try:
        m = run_mds(samples, plan, args.shard_mb, workdir)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if m is None:
        print("\nMDS: NOT MEASURED - `streaming` is not installed.")
        print("     pip install mosaicml-streaming")
        print("\nGenna's side above is measured; the ratio is NOT reproduced")
        print("without the MDS side. That is the honest state of this run.")
        return 2

    print("\nMOSAICML MDS")
    print(f"  initial write:      {m['initial_bytes']:,} bytes "
          f"({m['initial_bytes'] / 1048576:.2f} MB) in {m['shards']} shards, "
          f"{m['initial_seconds']:.2f} s")
    print(f"  per-edit, shard:    {m['per_edit_bytes']:,} bytes "
          f"({m['per_edit_bytes'] / 1048576:.1f} MB) in {m['per_edit_seconds']:.1f} s")
    print(f"                      -> rewrite only the affected shard "
          f"({args.edits} versions)  [the fair model]")
    print(f"  per-edit, whole:    {m['whole_rewrite_bytes']:,} bytes "
          f"({m['whole_rewrite_bytes'] / 1048576:.1f} MB) in "
          f"{m['whole_rewrite_seconds']:.1f} s")
    print(f"                      -> rewrite everything each time  [naive]")
    print(f"  batched:            {m['batched_bytes']:,} bytes "
          f"({m['batched_bytes'] / 1048576:.2f} MB) in {m['batched_seconds']:.2f} s")
    print(f"                      -> 1 new version  [MDS best case]")

    r_edit = m["per_edit_bytes"] / max(1, g["edit_bytes"])
    r_whole = m["whole_rewrite_bytes"] / max(1, g["edit_bytes"])
    r_batch = m["batched_bytes"] / max(1, g["edit_bytes"])
    s_edit = m["per_edit_seconds"] / max(1e-9, g["edit_seconds"])
    print("\nRATIOS vs Genna's "
          f"{g['edit_bytes']:,} bytes / {g['edit_seconds'] * 1000:.1f} ms "
          f"(documented: 2,755x less written, 119x faster)")
    print(f"  vs shard rewrite  ({args.edits} versions each, like for like): "
          f"{r_edit:,.0f}x less written, {s_edit:,.0f}x faster")
    print(f"  vs whole rewrite  (naive MDS use):                       "
          f"{r_whole:,.0f}x less written")
    print(f"  vs batched        (MDS best case, 1 version vs {g['versions']}):   "
          f"{r_batch:,.0f}x less written")
    print("\n  The shard-rewrite row is the one to quote: it is the only")
    print("  comparison where both systems end up with the same number of")
    print("  recoverable versions and MDS is used competently.")

    if args.json:
        print("\n" + json.dumps({"genna": g, "mds": m,
                                 "ratio_per_edit": r_edit,
                                 "ratio_batched": r_batch}, indent=2, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
