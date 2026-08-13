#!/usr/bin/env python3
"""Do training checkpoints actually share bytes? A spike, not a product.

The pitch is that checkpoint storage is an expensive unsolved pain -- dozens
of ~14 GB checkpoints per run, 99% identical between consecutive steps -- and
that structural sharing is exactly the right shape for it.

The 3D work already found the closed form that decides this. Chunk dedup needs
an ENTIRE chunk byte-identical; if a fraction p of the values in a chunk
change, a chunk of C values survives with probability (1-p)^C. Full-precision
SGD moves *every* weight it touches, so p ~ 1 and sharing collapses to zero.

But two things differ from noisy 3D scans, and they are the whole question:

  * Parameters that receive NO gradient do not move at all. Frozen layers,
    unused embedding rows, and LoRA base weights are bit-identical forever.
  * Reduced precision quantizes small updates to zero -- a bf16 weight whose
    update is below its ULP is unchanged.

So this measures REAL torch training, not a simulation, across four regimes,
and reports what each actually shares. The interesting answer is allowed to be
"almost nothing" for the headline case.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "python"))


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
def build_model(torch, nn, d_model, n_layers, vocab):
    layers = []
    for _ in range(n_layers):
        layers += [nn.Linear(d_model, d_model * 2), nn.GELU(),
                   nn.Linear(d_model * 2, d_model), nn.LayerNorm(d_model)]
    return nn.Sequential(nn.Embedding(vocab, d_model), *layers,
                         nn.Linear(d_model, vocab))


def state_bytes(sd, torch):
    """Serialize a state_dict to raw contiguous bytes, tensor by tensor.

    Deliberately NOT torch.save: pickle framing and dict ordering would add
    noise that has nothing to do with whether the weights changed.
    """
    out = bytearray()
    for k in sorted(sd.keys()):
        t = sd[k].detach().to(torch.float32).contiguous().cpu()
        out += t.numpy().tobytes()
    return bytes(out)


def raw_state_bytes(sd, torch):
    """Same, but preserving each tensor's own dtype (so bf16 stays 2 bytes)."""
    out = bytearray()
    for k in sorted(sd.keys()):
        t = sd[k].detach().contiguous().cpu()
        if t.dtype == torch.bfloat16:
            t = t.view(torch.int16)
        out += t.numpy().tobytes()
    return bytes(out)


def unchanged_fraction(a: bytes, b: bytes, word: int) -> float:
    """Fraction of `word`-sized values byte-identical between two states."""
    import numpy as np
    if len(a) != len(b):
        return 0.0
    x = np.frombuffer(a, dtype=np.uint8).reshape(-1, word)
    y = np.frombuffer(b, dtype=np.uint8).reshape(-1, word)
    return float((x == y).all(axis=1).mean())


# --------------------------------------------------------------------------
def run_regime(name, torch, nn, steps, d_model, n_layers, vocab, seq,
               dtype, mode, chunk):
    """mode: 'full' | 'frozen' | 'lora'"""
    import numpy as np
    torch.manual_seed(0)
    model = build_model(torch, nn, d_model, n_layers, vocab)

    trainable = list(model.parameters())
    if mode == "frozen":
        # freeze everything but the last block, as partial fine-tuning does
        for p in list(model.parameters())[:-4]:
            p.requires_grad_(False)
        trainable = [p for p in model.parameters() if p.requires_grad]
    elif mode == "lora":
        # a genuine low-rank adapter: base frozen, only A/B train
        for p in model.parameters():
            p.requires_grad_(False)
        r = 8
        lora_a = nn.Parameter(torch.randn(d_model, r) * 0.01)
        lora_b = nn.Parameter(torch.zeros(r, vocab))
        trainable = [lora_a, lora_b]
        model.register_parameter("lora_a", lora_a)
        model.register_parameter("lora_b", lora_b)

    if dtype == "bf16":
        model = model.to(torch.bfloat16)

    opt = torch.optim.AdamW(trainable, lr=1e-3) if trainable else None
    states = []
    for step in range(steps + 1):
        sd = {k: v.clone() for k, v in model.state_dict().items()}
        states.append(raw_state_bytes(sd, torch))
        if step == steps or opt is None:
            break
        x = torch.randint(0, vocab, (8, seq))
        y = torch.randint(0, vocab, (8, seq))
        logits = model(x)
        if mode == "lora":
            h = model[0](x).to(lora_a.dtype)
            logits = logits.to(lora_a.dtype) + (h @ lora_a) @ lora_b
        loss = nn.functional.cross_entropy(
            logits.reshape(-1, vocab).float(), y.reshape(-1))
        opt.zero_grad(); loss.backward(); opt.step()

    word = 2 if dtype == "bf16" else 4
    unchanged = [unchanged_fraction(states[i - 1], states[i], word)
                 for i in range(1, len(states))]

    # ---- what Genna actually stores -----------------------------------
    import genna
    from genna._native import lib
    sdir = tempfile.mkdtemp(prefix="gn_ckpt_")
    try:
        eng = genna.Engine()
        t0 = time.perf_counter()
        for i, s in enumerate(states):
            eng.create_binary(f"ck{i}", s, avg_chunk=chunk)
        dt = time.perf_counter() - t0
        p = os.path.join(sdir, "ck.gn")
        eng.save(p)
        gn_total = os.path.getsize(p)
        chunks, dedup = eng.chunks, eng.stats.chunks_deduped
        eng.close(); lib.gn_arena_release()

        eng0 = genna.Engine()
        eng0.create_binary("ck0", states[0], avg_chunk=chunk)
        p0 = os.path.join(sdir, "ck0.gn")
        eng0.save(p0)
        gn_base = os.path.getsize(p0)
        eng0.close(); lib.gn_arena_release()
    finally:
        shutil.rmtree(sdir, ignore_errors=True)

    raw_total = sum(len(s) for s in states)
    return {
        "name": name, "states": len(states), "raw_total": raw_total,
        "per_ckpt": len(states[0]),
        "unchanged_mean": float(np.mean(unchanged)) if unchanged else 0.0,
        "gn_total": gn_total, "gn_base": gn_base,
        "gn_incremental": gn_total - gn_base,
        "chunks": chunks, "dedup": dedup, "seconds": dt,
    }


def run_git(states, workdir):
    d = os.path.join(workdir, "git")
    os.makedirs(d, exist_ok=True)
    subprocess.run(["git", "init", "-q", "."], cwd=d, capture_output=True)
    for k, v in (("user.email", "b@b"), ("user.name", "b"),
                 ("commit.gpgsign", "false"), ("gc.auto", "0")):
        subprocess.run(["git", "config", k, v], cwd=d, capture_output=True)
    f = os.path.join(d, "ck.bin")
    for i, s in enumerate(states):
        with open(f, "wb") as fh:
            fh.write(s)
        subprocess.run(["git", "add", "-A"], cwd=d, capture_output=True)
        subprocess.run(["git", "commit", "-qm", f"s{i}"], cwd=d, capture_output=True)
    subprocess.run(["git", "gc", "-q", "--aggressive"], cwd=d, capture_output=True)
    return dir_bytes(os.path.join(d, ".git"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--steps", type=int, default=8)
    ap.add_argument("--d-model", type=int, default=256)
    ap.add_argument("--layers", type=int, default=4)
    ap.add_argument("--vocab", type=int, default=2048)
    ap.add_argument("--chunk", type=int, default=1024)
    a = ap.parse_args()

    try:
        import torch
        import torch.nn as nn
    except ImportError:
        print("SKIP: torch not installed")
        return 0

    print("=== do training checkpoints share bytes? ===")
    print(f"model: d={a.d_model}, {a.layers} blocks, vocab {a.vocab}; "
          f"{a.steps} steps -> {a.steps+1} checkpoints\n")

    regimes = [
        ("fp32 full",    "fp32", "full"),
        ("bf16 full",    "bf16", "full"),
        ("fp32 frozen",  "fp32", "frozen"),
        ("fp32 LoRA",    "fp32", "lora"),
    ]
    rows = []
    for label, dt, mode in regimes:
        r = run_regime(label, torch, nn, a.steps, a.d_model, a.layers,
                       a.vocab, 32, dt, mode, a.chunk)
        rows.append(r)

    print(f"{'regime':<14} {'ckpt MB':>8} {'weights':>9} {'genna tot':>11} "
          f"{'per-ckpt':>10} {'vs naive':>9}")
    print(f"{'':14} {'':8} {'unchanged':>9}")
    for r in rows:
        per = r["gn_incremental"] / max(1, r["states"] - 1)
        print(f"{r['name']:<14} {r['per_ckpt']/1048576:8.2f} "
              f"{r['unchanged_mean']*100:8.2f}% {r['gn_total']:11,} "
              f"{per:10,.0f} {r['raw_total']/max(1,r['gn_total']):8.1f}x")

    # git head-to-head on the headline case
    print("\n-- vs git, fp32 full (the headline case) --")
    torch.manual_seed(0)
    m = build_model(torch, nn, a.d_model, a.layers, a.vocab)
    opt = torch.optim.AdamW(m.parameters(), lr=1e-3)
    states = []
    for step in range(a.steps + 1):
        states.append(raw_state_bytes(
            {k: v.clone() for k, v in m.state_dict().items()}, torch))
        if step == a.steps:
            break
        x = torch.randint(0, a.vocab, (8, 32))
        y = torch.randint(0, a.vocab, (8, 32))
        loss = nn.functional.cross_entropy(
            m(x).reshape(-1, a.vocab), y.reshape(-1))
        opt.zero_grad(); loss.backward(); opt.step()
    wd = tempfile.mkdtemp(prefix="gn_ckpt_git_")
    try:
        g = run_git(states, wd)
    finally:
        shutil.rmtree(wd, ignore_errors=True)
    fp32 = rows[0]
    print(f"   naive (every checkpoint whole): {fp32['raw_total']:,} B")
    print(f"   genna                         : {fp32['gn_total']:,} B")
    print(f"   git (total .git, packed)      : {g:,} B")

    # ---- the steelman: "roll back to step k, fork, try a different LR" ----
    # This is the use case the pitch actually names, and it is the one case
    # where fp32 full training can still win: every checkpoint BEFORE the
    # fork point is bit-identical between the two branches, so the shared
    # prefix costs nothing regardless of how much the weights move after it.
    print("\n-- forking a run at step k (the case the pitch names) --")
    import genna
    from genna._native import lib

    def train_branch(seed_state, lr, n):
        torch.manual_seed(0)
        m = build_model(torch, nn, a.d_model, a.layers, a.vocab)
        if seed_state is not None:
            m.load_state_dict(seed_state)
        o = torch.optim.AdamW(m.parameters(), lr=lr)
        outs = []
        for _ in range(n):
            x = torch.randint(0, a.vocab, (8, 32))
            y = torch.randint(0, a.vocab, (8, 32))
            loss = nn.functional.cross_entropy(
                m(x).reshape(-1, a.vocab), y.reshape(-1))
            o.zero_grad(); loss.backward(); o.step()
            outs.append(raw_state_bytes(
                {k: v.clone() for k, v in m.state_dict().items()}, torch))
        return outs, {k: v.clone() for k, v in m.state_dict().items()}

    K = max(1, a.steps // 2)
    pre, forked = train_branch(None, 1e-3, K)
    br_a, _ = train_branch(forked, 1e-3, K)
    br_b, _ = train_branch(forked, 3e-4, K)     # different LR schedule
    all_states = pre + br_a + pre + br_b        # as two independently-kept runs

    sdir = tempfile.mkdtemp(prefix="gn_fork_")
    try:
        eng = genna.Engine()
        for i, s in enumerate(all_states):
            eng.create_binary(f"f{i}", s, avg_chunk=a.chunk)
        pth = os.path.join(sdir, "fork.gn")
        eng.save(pth)
        fork_total = os.path.getsize(pth)
        eng.close(); lib.gn_arena_release()
    finally:
        shutil.rmtree(sdir, ignore_errors=True)
    naive = sum(len(s) for s in all_states)
    print(f"   two branches sharing a {K}-checkpoint prefix "
          f"({len(all_states)} checkpoints total)")
    print(f"   naive: {naive:,} B    genna: {fork_total:,} B    "
          f"-> {naive/max(1,fork_total):.2f}x")
    print(f"   the shared prefix is free; the diverged tails are not.")

    print("\n== verdict ==")
    for r in rows:
        share = r["unchanged_mean"]
        print(f"   {r['name']:<14} {share*100:6.2f}% of weights unchanged "
              f"per step -> {r['raw_total']/max(1,r['gn_total']):.1f}x vs naive")
    print("\n   The closed form from the 3D work applies unchanged: a chunk of C")
    print("   values survives with probability (1-p)^C where p is the fraction")
    print("   of values that moved. Read the 'unchanged' column as p's")
    print("   complement -- it predicts the sharing column entirely.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
