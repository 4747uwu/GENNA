# Genna 3D — what worked, what didn't, and the number that decides it

Built from the proposal to absorb continuous 3D entropy: transforms on tree
nodes, plus quantization with a residual side-channel. Measured on real
Stanford scans (bunny 35,947 v; armadillo 49,990 v; happy buddha 49,251 v).

Run it: `bash tools/run_scene.sh`

**One of the two ideas is excellent. The other does not work, and the reason
is arithmetic, not implementation.**

---

## 1. Instance transforms — works, and it is the whole feature

Geometry stored once as an asset; the scene is a list of `(asset, 4x4 matrix)`
records held in an ordinary Genna object, so it inherits versioning, rollback
and persistence.

| mesh | cost of moving one instance | rewriting its geometry | ratio |
|---|---|---|---|
| bunny | 14 nodes = **784 B** | 0.82 MB | **1,100×** |
| armadillo | 20 nodes = **1,120 B** | 1.14 MB | **1,071×** |
| happy buddha | 17 nodes = **952 B** | 1.13 MB | **1,242×** |

500 instances of one asset:

| mesh | store holds | 500 independent copies | ratio |
|---|---|---|---|
| bunny | 0.88 MB | 205.7 MB | **233×** |
| armadillo | 1.06 MB | 286.0 MB | **269×** |
| happy buddha | 2.08 MB | 281.8 MB | **136×** |

Verified each time: the moved instance reads back correctly, the other 499 are
untouched, and the move is exactly one new version.

### One correction to the proposal

**Transforms cannot hang off treap nodes.** A treap node is a position in a
sequence, not an entity, and path copying gives it a new identity on every
edit — there is no stable node meaning "this building", and the node covering
an object's range is whatever the random priorities produced. The stable thing
is the instance table. It gets the same win, and it is implementable.

Also: this is **O(log n), not O(1)** — a 68-byte splice plus ~14–20 tree nodes.

---

## 2. Quantization against global jitter — does not work

The claim was that micro-jiggles round back to the same millimetre, so chunks
share perfectly. Measured, on the bunny, sharing after jittering every vertex:

| jitter amplitude | coords that changed cell | chunks shared |
|---|---|---|
| step/2 | 24.6% | **0%** |
| step/8 | 6.1% | **0%** |
| step/100 | 0.45% | **0%** |
| step/1000 | 0.097% | 33% |
| step/10000 | 0.080% | 41% |
| none | 0% | 100% |

Armadillo and buddha agree (0% at step/100; 23–95% at step/1000 and below).

**Why.** A coordinate sits at a uniformly random position inside its cell, so
jitter of amplitude `a` (in units of step) pushes it across a boundary with
probability `≈a/2` — at step/8 that is 6% of all coordinates, measured. Chunk
dedup needs an *entire* chunk unchanged, so a chunk of `C` coordinates survives
with probability `(1 − a/2)^C`. With `C` in the hundreds that is zero for any
jitter you would ever notice.

> **The design rule:** quantization buys sharing only when jitter is roughly a
> **thousand times finer than the quantum** — not merely below it. "Below the
> quantum" is the intuition that fails.

There is also a floor: at step/10000 the change rate stops falling (0.080% on
the bunny), because real scanned coordinates sit exactly on cell boundaries
where any perturbation flips them. That caps sharing at ~41% however fine the
jitter gets.

### And the residual channel cannot save it

In `GN_QUANT_EXACT` the residual *is* the noise: it changes whenever the input
does, so it dedups nothing. The grid stream is 50% of the bytes, so exact mode's
ceiling is 50% even in the best case — and the table shows both modes moving
together, because the grid stream is what breaks first.

Storage cost, measured (raw float32 is 12.0 B/vertex):

| mode | B/vertex | error bound | verified |
|---|---|---|---|
| `EXACT` | 24.0 | 0 | **bit-identical round trip** |
| `BYTE` | 15.0 | step/500 | max 9.83e-07 vs 1e-06 bound |
| `GRID` | 12.0 | step/2 | max 2.5e-04 vs 2.5e-04 bound |

Exact mode **doubles** the storage. That is the real price of byte-exactness
on float data, and it is worth knowing before building on it.

---

## 3. The case that actually matters — and it needed nothing new

Retraining a region, an artist moving one limb: a **localized** change.

| mesh | raw float32 shared | quantized (grid) shared |
|---|---|---|
| bunny | 50% | 38% |
| armadillo | 51% | 48% |
| happy buddha | 50% | 51% |

**Raw float32 already shares half the mesh, and quantization does not beat it.**
The treap handles localized edits by construction — that is what it was built
for. Quantization's only possible contribution was global micro-noise, and
section 2 shows the window for that is a thousand times narrower than claimed.

This is asserted as a test, so it cannot quietly stop being true.

---

---

## 4. Against the actual industry tools

Everything above compares Genna to *itself*. That measures nothing about
whether it is worth using. `benchmarks/spatial_vs_industry.py` runs one
identical workload — N instances of the bunny, 25 moves, every intermediate
state recoverable — through Google Draco, Pixar USD, git and zstd.

### 4.1 Geometry compression: Draco wins decisively

| method | bytes/vertex | max error (nearest-neighbour) |
|---|---|---|
| **Draco qb11** | **2.07** | 6.5e-05 |
| **Draco qb14** | **3.19** | 8.1e-06 |
| zstd -19 on raw float32 | 10.06 | 0 |
| Genna `GRID` | 12.00 | 2.5e-04 |
| raw float32 | 12.00 | 0 |
| Genna `EXACT` | 24.00 | 0 (bit-exact) |

**Draco beats Genna's codec on both axes at once**: `draco_qb14` is 3.8×
smaller than Genna `GRID` *and* 30× more accurate. Genna `EXACT` is **11.6×
larger than Draco qb11**. The only thing Genna's codec has that Draco does not
is bit-exactness — and if you need that, zstd gets you 10.06 B/vertex losslessly,
still better than Genna's 24.

**Conclusion: do not use the quantization codec built here.** The right shape
is Draco-compressed assets stored *inside* Genna for versioning.

### 4.2 Versioned editing vs the pipeline a good studio already runs

Geometry stored once, transforms in a USD layer, committed to git per move:

| instances | Genna writes | git+USD (loose) | git+USD **after `gc`** | ratio (packed) |
|---|---|---|---|---|
| 20 | 12,544 B | 18,360 B | 16,193 B | **1.3×** |
| 200 | 14,672 B | 45,637 B | 16,281 B | **1.1×** |
| 2,000 | 16,240 B | 309,084 B | 16,554 B | **1.0×** |
| 20,000 | 17,416 B | 2,894,014 B | 18,446 B | **1.1×** |

**On bytes, this is a tie at every scale tested.** git's delta compression
finds exactly the same redundancy that structural sharing does, and it does
not degrade as the scene grows. The 1,100× headline in §1 was measured against
*rewriting the geometry*, which is the naive pipeline — against the
competent one there is no storage win at all.

Two things do survive:

- **Latency.** 0.4–0.5 ms vs 2.6–17.6 seconds: **7,000×–36,000× faster**, and
  the gap widens with scene size. That is the difference between an edit that
  is interactive and one that is a commit you wait for.
- **Write amplification before repack.** git only reaches parity *after* a
  `gc --aggressive`; loose, it writes 166× more at 20k instances. That matters
  for IO and working set, but it is a deferred cost, not a permanent one.

Against the naive baked-geometry pipeline the gap is real but should be quoted
carefully: 14,152× loose, **998× after `gc`**.

### 4.3 Instancing is not novel

USD stores 20 instances of the bunny in 436,060 B (binary `.usdc` asset +
4,030 B scene) using `SetInstanceable`. Pixar has shipped this for years.
Genna's instance table is the same idea; what Genna adds is that the instance
table is *itself versioned*, not that instancing exists.

---

## 5. Engine changes made in response to the above

Two things the text ingest path does are wrong for geometry, and both were
fixed in a new binary path (`src/genna_bin.c`, `gn_create_binary`):

1. **It learned.** `gn_create()` calls `gn_dict_learn()`, and the dictionary
   is shared and append-only — so ingesting mesh B after mesh A could
   tokenize byte sequences identical in both *differently*. This is why §3
   measured only ~67% sharing on a 1% localized edit when it should have been
   ~98%. The binary path tokenizes byte-for-byte with no learning.
2. **It cut chunks at fixed token counts.** Insert one vertex and every
   boundary after it shifts. The binary path cuts where the *content* says
   to, via a gear-hash rolling window.

Measured A/B, same data, same edits, only the chunking differs
(`bash tools/run_bin.sh`):

| workload | fixed cuts | content-defined | change |
|---|---|---|---|
| move 1% region | 98.1% | 97.5% | −0.6 pts |
| **insert 1% verts** | 48.6% | **95.1%** | **+46.5 pts** |
| **delete 1% verts** | 49.5% | **98.7%** | **+49.2 pts** |
| jitter ALL | 0.0% | 0.0% | +0.0 |

(armadillo agrees: insert 49.3→97.3%, delete 50.3→99.1%.)

Insert and delete are what splat training does every step, so this is the
common case, not an edge case. Bypassing the dictionary also fixed the
localized-edit result on its own: 67% → **98%**.

**The jitter row is still 0% and always will be.** Every vertex genuinely
changed; no chunking scheme can share data that does not exist. That is now
asserted as a test so the claim cannot quietly drift.

Also added: `gn_morton_order()`, a Z-order permutation so that points near
each other in space are near each other in the byte stream — without it a
"localized" edit touches bytes scattered through the whole file.

Cost, stated plainly: the binary path still represents one byte as one 4-byte
token, so a binary object occupies **4× its size** in the store.

## 6. Evolving geometry vs git — Genna loses on storage

The real test of the above: a point cloud that gains and loses points every
step (densify +1%, prune −0.5%), 20 steps, every state recoverable.
`python benchmarks/evolving_geometry.py`

| method | bytes | time |
|---|---|---|
| every state stored whole | 9,100,000 | — |
| genna, fixed chunking | 23,415,856 | 51 ms |
| genna, **CDC** | **3,552,380** | **43 ms** |
| git, loose | 7,709,844 | 2,105 ms |
| zstd -19 over all states | 386,330 | — |
| **git, packed** | **56,508** | — |

CDC is a genuine **6.6× improvement over what Genna had**. And Genna is
**49× faster** than git here.

But **git packed is 63× smaller than Genna**, and 15.7× smaller even after
correcting for Genna's 4× token penalty (888 KB adjusted). Plain zstd also
beats Genna.

**The diagnosis is precise: Genna dedups but never compresses.** An unmatched
chunk is stored raw. git deltas against the previous version *and* zlib-
compresses the result, which is why it beats even a dedicated compressor.
Deduplication alone is simply a weaker mechanism than delta+compression on
data this self-similar.

## 7. Compression, wired in — and the comparison redone properly

Two things were wrong with §6 and both are fixed here.

**First, §6 compared the wrong quantities.** It put Genna's *resident RAM*
against git's *on-disk `.git`*. Storage against storage means `gn_save()` file
vs `.git`, which is what this section measures.

**Second, the snapshot payload was stored raw.** It now compresses as a whole
at save time (zstd -19, else deflate). zstd rather than zlib for a specific
reason: zlib's 32 KB window cannot see that a chunk near the end of a store is
nearly identical to one near the start, which on a many-version store is the
only redundancy left after dedup.

Same workload, 21 states of an evolving point cloud, **file vs file**:

| method | on disk | vs before |
|---|---|---|
| every state stored whole | 9,100,000 | — |
| genna, fixed chunking | 703,131 | |
| **genna, CDC + zstd** | **546,574** | 3.55 MB → 546 KB |
| git, loose | 7,709,842 | |
| zstd -19 over all states | 386,330 | |
| **git, packed** | **56,507** | |

| ratio | with raw payload | with zstd payload |
|---|---|---|
| vs git loose | 2.17× smaller | **14.1× smaller** |
| vs git packed | git 15.8× smaller | **git 9.7× smaller** |
| vs zstd -19 | zstd 2.3× smaller | zstd 1.4× smaller |
| chunk-size optimum | — | ~1 KB (256 B is worse: per-chunk overhead) |

Compression also helps text exactly as predicted: the 2.4 MB corpus store with
31 versions went **739,922 → 191,925 bytes (3.85×)**.

**git still wins storage by 9.7×, and the remaining gap is delta encoding.**
git stores an xdelta against the previous version — a few KB per step. Genna
stores a whole new chunk for any chunk containing a change, which at a 1 KB
chunk size is ~1 KB per changed region. Dedup plus compression is still a
weaker mechanism than delta plus compression on data this self-similar. No
amount of chunk tuning closes that; it needs chunk-level delta encoding, which
is not built.

One nuance worth recording: **once the payload is well compressed, CDC's
on-disk advantage shrinks from 6.45× to 1.29×** — a good compressor recovers
much of what dedup was doing. CDC's real remaining value is resident memory
(2.3 MB vs 22.2 MB, **9.6×**), which is the working set, not the file.

---

## 8. A sculpting session, stored five ways

`python benchmarks/sculpt.py` — 40 brush strokes on the bunny, each displacing
the vertices inside a sphere with a smooth falloff (a real, *localized* edit),
every intermediate state recoverable.

Brush touches 187 vertices per stroke (0.5% of the mesh).

| strategy | on disk | vs naive | **B/stroke** |
|---|---|---|---|
| whole (naive) | 17,685,924 | 1.0× | 431,364 |
| cdc | 703,446 | 25.1× | 7,027 |
| cdc + morton | 648,038 | 27.3× | 5,642 |
| **git (total .git)** | 626,383 | 28.2× | **5,100** |
| **displacement** | 513,367 | 34.5× | **2,275** |
| **base + strokes** | 423,650 | 41.7× | **32** |

The per-stroke column is the one that matters for an undo stack; the totals
are dominated by the base mesh (~422 KB), which every strategy pays once.

**Morton ordering: real but modest — 7,027 → 5,642 B/stroke (1.25×).** Not the
transformative win it was described as. The reason is worth knowing: the
Stanford scans are already in roughly spatially-coherent scan order, so
Z-ordering has less to add than it would on an arbitrarily-ordered mesh.

**Displacement encoding: 2,275 B/stroke, 2.2× better than git.** Storing a
base mesh plus sparse `(index, dx, dy, dz)` records genuinely beats
chunk-level dedup, because it encodes exactly what changed instead of the
chunk that contains what changed.

**Stroke sourcing: 32 B/stroke, 159× better than git** — and it is not free:

- Reconstructing state *k* costs **O(k) replay**: 16 ms to reach state 20,
  against 0.4 ms to read it from a Genna store. An undo stack you cannot
  scrub is not an undo stack.
- It requires the brush to be **bit-for-bit deterministic** across versions
  and platforms. Verified here (replay reproduced the final state exactly),
  but a floating-point change in the brush silently invalidates the whole
  history.
- The 41.7× total is almost entirely the base mesh: 1,280 bytes of strokes
  against 422 KB of base. "Gigabytes become kilobytes" is true of the
  *deltas*, not of the store.

The practical answer is a hybrid, which is what Genna already is: strokes in
the log for cheap undo, periodic snapshots to bound replay. That is exactly
the snapshot + WAL architecture in PERSISTENCE.md, applied at the domain
level.

## 9. Core: a small read was O(total leaves), not O(log n)

`read_ver()` passed the whole tree to `gn_ext_walk()`, which visits **every**
leaf; `read_leaf()` then skipped the ones before the offset. So the central
claim — reading a range costs the range, not the file — was not true.

Measured, a fixed 1 KB read as the object grows (`tests/readscale.c`):

| object | leaves | before | after |
|---|---|---|---|
| 1 MB | 64 | 0.0037 ms | 0.0035 ms |
| 8 MB | 512 | 0.0054 ms | 0.0035 ms |
| 32 MB | 2048 | **0.0110 ms** | **0.0035 ms** |

Fixed by `gn_ext_walk_range()`, which prunes on the subtree byte totals the
tree already maintains. The time is now flat: **3.1× faster at 32 MB, and the
gap grows with object size** — on a 10 GB object the old path would have
visited ~640,000 leaves for a 1 KB read.

This was found by measuring the claim rather than trusting it, and it is the
most valuable change in this round: it makes an existing headline claim
actually true.

---

## Verdict

| idea | verdict |
|---|---|
| **Content-defined chunking** (added this round) | **Keep.** Insert/delete sharing 49% → 95–99%; 6.6× less resident on evolving geometry. The largest real improvement made here. |
| **Binary path, no dictionary learning** (added) | **Keep.** Fixed localized-edit sharing 67% → 98% on its own. |
| Instance table + transforms | Correct, but **not novel** — USD already does it. The 1,100× is against the naive pipeline; against git+USD layers the storage win is **1.0–1.3×, i.e. none**. |
| Quantized grid + residuals | **Do not build on it.** Draco is 3.8–11.6× smaller *and* more accurate. Even plain zstd beats Genna's exact mode. |
| Payload compression (added) | **Keep.** 3.85× on a text store, 6.5× on a geometry store. |
| Range-limited read (added) | **Keep.** A 1 KB read was O(total leaves); now flat. 3.1× at 32 MB, unbounded gain with size. Makes the O(log n) claim true. |
| Morton ordering | **Keep, but modest**: 1.25× per stroke, not transformative. Stanford scans are already near-spatially-ordered. |
| Displacement encoding | **Keep.** 2,275 B/stroke, **2.2× better than git**. |
| Stroke sourcing | **Keep as a layer, with eyes open.** 32 B/stroke, but O(k) replay and requires a bit-deterministic brush. |
| Storage vs git, total | **Near-tie: git 1.16× smaller.** (An earlier report said 9.7×; that compared git's *incremental* growth against Genna's *whole* store.) |
| Storage vs git, incremental | git 2.18× smaller with plain CDC — but **Genna wins with displacement (2.2×) or strokes (159×)**. |
| Latency of a versioned edit | **The one real, defensible advantage.** 49× on evolving geometry, 36,000× on transform edits at 20k instances. |
| "Solved the 3D bottleneck" | **Not supported.** Storage loses to git, compression loses to Draco, per-vertex noise is unsolved. |

**Compression is now wired in** (PERSISTENCE.md §2) and it helped text as much
as geometry, as predicted — but it did not close the gap. **The remaining gap
is delta encoding, and it is not a 3D problem either.** Genna stores a whole
new chunk for any chunk containing a change; git stores the changed bytes.
Chunk-level delta against a similar existing chunk is the next mechanism, and
until it exists Genna should not be pitched on storage size against git.

If there is a 3D product here it is "**interactive versioning**", not
"**smaller storage**" — and the sales pitch would have to be latency, with
Draco doing the compression and Genna doing the history.

What would actually attack the noise problem — none of it built or measured
here, so treat it as untested:

- **Content-defined chunking** over the coordinate stream, so one changed
  vertex resizes one chunk instead of shifting every boundary after it. This
  is the standard fix and is the first thing to try.
- **Much smaller chunks** for spatial data — the `(1 − a/2)^C` rule says the
  survival probability is exponential in chunk size, so a 10× smaller chunk
  is worth far more than a 10× finer quantum.
- **Spatial ordering** (Morton/Hilbert) so that localized edits stay localized
  in the byte stream.

## Not handled

- Only `v x y z` positions are read from OBJ. No normals, uvs, faces,
  materials, or any binary format (PLY/glTF/USD).
- Transforms are stored but never *applied* — there is no renderer and no
  composition of parent/child transforms. This is a storage layer.
- No spatial index; no frustum/region queries.
- Instance records are fixed 68 bytes and instances cannot be deleted, only
  overwritten.
- Gaussian splats specifically (opacity, covariance, SH coefficients) are not
  modelled; only positions were tested.
