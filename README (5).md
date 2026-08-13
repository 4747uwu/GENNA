# Genna — complete

A structural-sharing engine + `genna-curate`, a versioned training-data curation
tool built on it. Everything here is the current, tested code.

## Quick start
    make              # builds genna-curate + embedding + SIMD + compression
    ./genna-curate sample_ml_dataset.jsonl   # interactive; type 'help'

## What's here

### The engine (src/, include/)
Structural-sharing representation engine: a persistent treap + content
addressing giving O(log n) edits, cheap versioning/branching, and byte-exact
delta-sync. Core files:
- genna_engine3.c — current engine (persistent treap)
- genna_ext.c     — the treap (split/join, refcount GC)
- genna_dict2.c   — language/dictionary module (segmentation + BPE + longest-match)
- genna_net.c     — Genna-Net sync (manifest/diff/serialize/apply)
- genna_vdict.c, genna_smart.c, genna_genesis.c, genna_kdict.c, genna_tokdict.c
                  — medium-specific modules (video, genomics, tokens)
- (genna_engine.c / engine2.c / genna_dict.c kept for the array-vs-tree comparison)

### genna-curate (genna_curate.c)
Versioned dataset curation — every op is a byte-exact versioned edit on the engine:
- `dedup`            exact dedup, O(n) hash, content-aware (text field)
- `near-dedup`      lexical near-dedup via embedding cosine (catches typos/
                    spacing/case/edits; 7/8 types vs exact match's 0/8, verified)
- `filter-length` / `drop-contains` / `replace`   filtering & relabeling
- `quality` / `scan-pii`                          quality report & PII flagging
- `versions` / `rollback` / `diff` / `export`     versioning (all byte-exact)

### The embedding (genna_embed.c)
Lightweight text embedding (hashed word+char n-grams -> 256-dim, cosine).
[SEAM: REAL EMBEDDING MODEL] — swap gn_embed() for sentence-transformers to get
real semantic dedup; the pipeline and the engine's role are unchanged.

### Optimizations
- genna_simd.c     AVX2 two-anchor substring scan (27-37x over scalar; beats memmem)
- genna_compress.c LZ compression experiment (see genna_embed for the honest note:
                   zlib as the chunk layer reaches git parity)

## Measured results (real tools, real data, byte-exact)
- vs MosaicML MDS (WikiText-2, mutate w/ history): 2,755x less written, 119x faster
- vs git 2.43 (Redis source):     60x less written/edit, 91x faster old-version read
- vs rsync 3.2.7 (WikiText-2):     10x less over the wire
- vs ffmpeg (real H.264):          ~110,000x on structural cuts, byte-exact
- scale: O(log n) holds 64MB -> 700MB; runs in WASM (171KB module)
Losses (honest): raw compression ratio (xz wins), byte-patching (bsdiff wins).

## Honest state
Core engine: proven, byte-exact. genna-curate: working for exact/lexical/rule ops.
NOT yet built: persistence (RAM-only — the product blocker), true semantic dedup
(needs the real embedding model at the seam), streaming ingest, Python bindings.
These are the next build targets.
