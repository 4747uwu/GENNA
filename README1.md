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
Re-measured on current hardware with the real tools installed. Two numbers
were **corrected downward**: `realbench.c`/`gitcmp.c` accounted a treap node at
24 bytes when it is 56, understating Genna's own writes by 2.33x and inflating
every ratio derived from them. See TEST_REPORT.md §4.1.

| vs | documented | measured now |
|---|---|---|
| MosaicML MDS 0.13 (WikiText-2, 100 edits, 100 versions each side) | 2,755x less written | **1,170x less, 953x faster** |
| git 2.55 (Redis source, 100 commits) | 60x less written/edit | **18.1x less** |
| rsync 3.4.4 (WikiText-2) | 10x over the wire | **10.0x** |
| ffmpeg 9.0 (real H.264, structural cuts) | ~110,000x | **~125,000-143,000x** |

All four remain byte-exact. The MDS comparison is reproducible from this repo:
`python benchmarks/mds_vs_genna.py corpora/wikitext-2-raw/wiki.train.raw`.
Losses (honest): raw compression ratio (xz wins), byte-patching (bsdiff wins).
The git old-version-read claim (91x) is **not reproducible as stated** — the
measurement is dominated by process-spawn cost, so it is reported as unusable
rather than as a 1,979x win.

### Persistence (src/genna_persist.c, include/genna_persist.h)
On-disk format + write-ahead log. `gn_save()` writes the chunk store, the
dictionary and every object's full version DAG to one file; `gn_open()` brings
it back ready to use. After a save, every edit is logged and fsynced before it
is applied, so a killed session reopens without losing committed work.
The version graph is serialized *as a graph*, so structural sharing survives to
disk: 24,484 versions of a 2.4 MB dataset occupy 4.3 MB and reopen in 18 ms.
See **PERSISTENCE.md** for the format, the guarantees, and what is not handled.

    save <path> / open <path>          in genna-curate
    ./genna-curate --open <store.gn>   resume a saved session

### Python bindings (python/)
The engine as a pip-installable package, because a C library ML teams can't
`import` is invisible to them.

    pip install genna                 # core, zero ML dependencies
    pip install "genna[semantic]"     # + semantic near-dedup

```python
import genna
ds = genna.Dataset.from_jsonl("train.jsonl")
ds.dedup(); ds.near_dedup(0.45); ds.filter_length(min_bytes=20)
ds.save("train.genna")               # dataset AND full history, one file
genna.open("train.genna").rollback(0)  # byte-exact, after a restart
```

Plain C loaded with ctypes, not a CPython extension, so one build works across
interpreters and there is no Python-ABI matching to get wrong. 58 binding-level
checks including 400 differential splices against a Python shadow buffer.
See `python/README.md`.

### Semantic near-dedup (the filled seam)
`genna/embed.py` runs the real `all-MiniLM-L6-v2` through **onnxruntime — no
torch**. The old lexical embedding could not separate paraphrases from
unrelated text at any threshold (worst duplicate 0.001 vs best non-duplicate
0.085). The model separates them with a **0.445 margin**. Head-to-head:
`python/tests/test_semantic.py`.

### Reproducing the MDS number (benchmarks/)
`benchmarks/mds_vs_genna.py` is the MosaicML MDS head-to-head, in the repo so
the flagship number can actually be re-run. It reports both the per-edit case
(101 recoverable versions each) and MDS's best case (batched, 1 new version),
because quoting only one would be dishonest.

## Honest state
Core engine: proven, byte-exact. genna-curate: working for exact/lexical/rule ops.
Persistence: built and tested (byte-exact round trip in a fresh process,
SIGKILL recovery, clean under ASan+UBSan). Not handled yet: concurrent writers,
incremental checkpoints, and persisting the video dictionary module — see
PERSISTENCE.md §5.
NOT yet built: true semantic dedup (needs the real embedding model at the seam),
streaming ingest, Python bindings. These are the next build targets.
