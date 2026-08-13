"""Does the real model actually beat the lexical one?

The old seam was a hashed bag of word/char n-grams. It was honest about being
lexical. The claim being tested here is specific: on PARAPHRASES -- pairs that
mean the same thing and share almost no wording -- the embedding model finds
them and the lexical method does not.

This test would be worthless if it only used near-identical strings, because
lexical matching gets those right too. So it uses three groups:

  paraphrases  same meaning, different words       -> semantic should catch
  surface      typos/case/spacing of one sentence  -> BOTH should catch
  distinct     unrelated sentences                 -> NEITHER may catch

A method is only good if it separates all three. Catching everything is not a
pass -- that is just a low threshold, and the test checks for it.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import genna
from genna import embed

FAILS = []


def check(cond, msg):
    print(f"  {'ok  ' if cond else 'FAIL'}  {msg}")
    if not cond:
        FAILS.append(msg)
    return bool(cond)


# ---------------------------------------------------------------------------
# The lexical baseline: exactly what gn_embed() did in genna_curate.c --
# hashed unigrams + bigrams + 4-char shingles into 256 dims, L2 normalized.
# Reimplemented here so the comparison is against the real prior art.
# ---------------------------------------------------------------------------
EMB_DIM = 256


def _fnv(b: bytes) -> int:
    h = 1469598103934665603
    for c in b:
        h ^= c
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def lexical_embed(text: str):
    import numpy as np
    v = np.zeros(EMB_DIM, dtype=np.float32)

    def add(h, w):
        v[h % EMB_DIM] += (1.0 if (h >> 40) & 1 else -1.0) * w

    t = " ".join(text.lower().split())
    words = t.split(" ")
    for i, w in enumerate(words):
        add(_fnv(w.encode()), 1.0)
        if i + 1 < len(words):
            add((_fnv(w.encode()) * 1099511628211) ^ _fnv(words[i + 1].encode()), 0.5)
    for k in range(len(t) - 3):
        if t[k] == " ":
            continue
        add(_fnv(t[k:k + 4].encode()), 0.35)
    n = float((v * v).sum()) ** 0.5
    return v / n if n > 0 else v


PARAPHRASES = [
    ("The cat sat on the mat.",
     "A feline was resting upon the rug."),
    ("How do I reset my password?",
     "What is the procedure for changing my login credentials?"),
    ("The film was terrible and I want my money back.",
     "That movie was awful; I would like a refund."),
    ("Python is a programming language used for machine learning.",
     "ML engineers often write their code in Python."),
]

SURFACE = [
    ("The quick brown fox jumps over the lazy dog.",
     "the  quick brown fox jumps over the lazy dog"),
    ("Deep learning models require large datasets.",
     "Deep learning modles require large datasets."),
]

DISTINCT = [
    ("The cat sat on the mat.",
     "Quarterly revenue increased by twelve percent."),
    ("How do I reset my password?",
     "The mitochondrion is the powerhouse of the cell."),
    ("Python is a programming language used for machine learning.",
     "Please deliver the package to the back entrance."),
]


def main():
    print("=== semantic vs lexical near-dedup ===\n")

    if not embed.available():
        print("SKIP: semantic extras not installed "
              '(pip install "genna[semantic]")')
        return 0

    import numpy as np

    print("loading model (first run downloads ~90 MB)...")
    try:
        emb = embed.Embedder()
    except Exception as e:
        print(f"SKIP: could not load the embedding model: {e}")
        return 0
    print(f"model: {emb.name}\n")

    def lex_sim(a, b):
        va, vb = lexical_embed(a), lexical_embed(b)
        return float(va @ vb)

    print("-- paraphrases (same meaning, different words) --")
    print(f"  {'semantic':>9} {'lexical':>9}   pair")
    sem_para, lex_para = [], []
    for a, b in PARAPHRASES:
        s, l = emb.similarity(a, b), lex_sim(a, b)
        sem_para.append(s); lex_para.append(l)
        print(f"  {s:9.3f} {l:9.3f}   {a[:44]!r}")

    print("\n-- surface variants (typos/spacing: both should catch) --")
    sem_surf, lex_surf = [], []
    for a, b in SURFACE:
        s, l = emb.similarity(a, b), lex_sim(a, b)
        sem_surf.append(s); lex_surf.append(l)
        print(f"  {s:9.3f} {l:9.3f}   {a[:44]!r}")

    print("\n-- distinct (neither may catch) --")
    sem_dist, lex_dist = [], []
    for a, b in DISTINCT:
        s, l = emb.similarity(a, b), lex_sim(a, b)
        sem_dist.append(s); lex_dist.append(l)
        print(f"  {s:9.3f} {l:9.3f}   {a[:44]!r}")

    # ------------------------------------------------------------------
    # Threshold-free verdict.
    #
    # Asking "how many does it catch at 0.6" only measures the threshold I
    # picked. The method-level question is: does ANY threshold separate the
    # duplicates from the non-duplicates? That is min(duplicate similarity) >
    # max(non-duplicate similarity), and it cannot be tuned into existence.
    # ------------------------------------------------------------------
    print("\n-- separability (does ANY threshold work?) --")
    sem_dup = sem_para + sem_surf
    lex_dup = lex_para + lex_surf

    sem_lo, sem_hi = min(sem_dup), max(sem_dist)
    lex_lo, lex_hi = min(lex_dup), max(lex_dist)
    print(f"  semantic: worst duplicate {sem_lo:.3f} vs best non-duplicate "
          f"{sem_hi:.3f}  -> margin {sem_lo - sem_hi:+.3f}")
    print(f"  lexical:  worst duplicate {lex_lo:.3f} vs best non-duplicate "
          f"{lex_hi:.3f}  -> margin {lex_lo - lex_hi:+.3f}")

    check(sem_lo > sem_hi,
          f"semantic IS separable: every duplicate scores above every "
          f"non-duplicate (margin {sem_lo - sem_hi:.3f})")
    check(not (lex_lo > lex_hi),
          f"lexical is NOT separable at any threshold "
          f"(margin {lex_lo - lex_hi:+.3f}) - so this is a real improvement, "
          f"not a tuning artifact")

    # A threshold anywhere in the semantic gap works; take the midpoint.
    TH = round((sem_lo + sem_hi) / 2, 2)
    print(f"  any threshold in ({sem_hi:.3f}, {sem_lo:.3f}) separates them; "
          f"using {TH} below")
    check(sum(1 for s in sem_para if s >= TH) == len(PARAPHRASES),
          f"at {TH}, semantic catches all {len(PARAPHRASES)} paraphrases")
    check(sum(1 for s in sem_dist if s >= TH) == 0,
          f"at {TH}, semantic flags none of the unrelated pairs")

    # ---------------------------------------------------------------
    print("\n-- end to end: Dataset.near_dedup on a paraphrase corpus --")
    records = []
    for a, b in PARAPHRASES:
        records.append({"text": a})
        records.append({"text": b})
    for a, b in DISTINCT:
        records.append({"text": b})           # unrelated filler
    ds = genna.Dataset.from_records(records, name="para")
    n_before = len(ds)

    ds.dedup()
    check(len(ds) == n_before,
          f"exact dedup removes nothing here (all records differ): {len(ds)}")

    step = ds.near_dedup(threshold=TH, embedder=emb)
    print(f"  {step}")
    check(step.removed == len(PARAPHRASES),
          f"near_dedup removed exactly the {len(PARAPHRASES)} paraphrase "
          f"partners (removed {step.removed})")
    check(len(ds) == n_before - len(PARAPHRASES),
          f"{len(ds)} records left, unrelated ones untouched")

    # and it is still a versioned, reversible edit
    original = ds.version_bytes(0)
    ds.rollback(0)
    check(ds.bytes() == original, "near_dedup is a reversible versioned edit")
    ds.engine.close()

    print(f"\n{'SEMANTIC: FAILURES' if FAILS else 'SEMANTIC: ALL PASS'} "
          f"({len(FAILS)} failures)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
