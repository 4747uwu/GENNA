# Genna — complete verified test report

Every number here was measured on **one build, one machine, one run**
(`test-logs/`, generated 2026-08-13T11:33:29Z). Nothing is carried over from an
earlier report; where an earlier report was wrong, the correction is stated
with the reason.

```
host      MSYS_NT-10.0-26200 x86_64
compiler  gcc 16.2.0 (MSYS2)          sanitized builds: clang 22.1.8
tools     git 2.55.0 · rsync 3.4.4 · ffmpeg 9.0 · mosaicml-streaming 0.13.0
          Draco (DracoPy) · Pixar USD (usd-core) · zstd 1.5 · onnxruntime 1.27
reproduce bash tools/run_all.sh          (writes test-logs/)
```

**Gate result: `0 script(s) reported failure`** across 15 stages, with the
Python suite reporting **`7 run, 0 failed, 0 SKIPPED`**.

That last clause is load-bearing. An earlier run of this same gate reported
`0 failures` while three Python suites had silently skipped (see §5, error
−1). The runner now names the interpreter it chose, lists which extras it
found, and **counts a skip as a failure** — so "green" means the tests ran.

The same failure mode was found again in the C suite while this round of work
was underway, and is worth stating next to the green result rather than buried
in §5: the **corrupt-store fuzzer had been vacuous in every build that had
zlib or zstd** — 1,200 mutants, all rejected at the checksum, no structural
validator ever reached, `0 problems` printed every time. Un-vacuuming it
immediately surfaced two real loader bugs (§6, A). Both fuzzers now **fail if
nothing loads**, because a campaign that cannot get past the CRC is testing
CRC32 and nothing else.

---

## 1. Correctness — all green

| suite | what it covers | result |
|---|---|---|
| `test_genna` | 11 pre-existing correctness assertions + demo numbers | **PASS** |
| `edge` | 19 pre-existing boundary cases | **PASS** |
| `fuzz_test` | splice / search / gc-refcount / persistence / corrupt-store | **PASS** |
| `persist_test` | byte-exact round trip across a process boundary | **PASS** 8/8 |
| `crash_test` | SIGKILL mid-edit + WAL forensics | **PASS** |
| `leak_test` | allocator accounting via linker `--wrap` | **CLEAN** |
| **ASan + UBSan**, all five | rebuilt under both sanitizers | **PASS 5/5, zero reports** |
| fuzz campaign | 12 seeds × 2,000 splices vs a shadow buffer | **12/12** |
| corrupt-store campaign | 12 seeds × 100 mutants of a **normal** store + 100 of a **mappable** one, CRC-repaired, under ASan | **0 problems**, and non-vacuous: 66 normal / 229 mappable mutants actually loaded and were read |
| `verify_walfix` | proves the WAL fixes are load-bearing | **PASS** — pre-fix build fails 4 checks *as it must*; shipped build passes all |
| A/B vs pre-persistence engine | did any engine number move? | **only the deliberate node-size fix** — see below |
| Python bindings | 58 checks incl. 400 differential splices | **58/58** |
| Python threads | 8 threads editing, 4 racing readers | **PASS** |
| Python semantic | real embedding model vs lexical | **PASS** |
| genna-curate end-to-end | save → exit → open → rollback | **PASS 5/5** |
| binary path (CDC) | insert/delete/move/jitter A/B on real scans | **PASS** |
| Python columnar tables | column-per-object, deletion vectors, manifest | **22/22** |
| Python query surface | diff + range_history vs a brute-force oracle | **11/11** |
| Python suite total | 7 suites, correct interpreter, skips counted as failures | **7 run, 0 failed, 0 SKIPPED** |
| `concurrent_test` | 8 processes racing, 5 rounds, optimistic commit | **PASS** |
| `oocore_test` | mapped store: byte-exact reads, resident memory, on-demand verify | **PASS** |
| `agg_test` | node aggregates vs brute force, incl. save/open round trip | **PASS** |
| `merge_test` | three-way merge: byte-exact clean merge, refusal on overlap | **PASS** |

Zero compiler warnings and zero sanitizer reports in both the plain and
sanitized builds.

**On the A/B against the original pre-persistence tree.** It now reports
differences, and every one is the node-size correction (§5.1), confirmed by
inspecting the diffs rather than assuming:

```
gitcmp     +0.036 MB tree nodes  ->  +0.083 MB     (ratio 2.31 ~ 56/24 = 2.33)
realbench   0.036 MB written     ->   0.084 MB     (same ratio)
rsynccmp   one word in a printed string
```

Every chunk count, store size (4.01 MB), payload size and byte-exactness
verdict is **identical**. Nothing else moved.

**On `verify_walfix` showing "3 failures" in an earlier index.** That was arm A
— the deliberately-broken build — being scraped by a summary that looked for
the last "N failures" line. The script now prints an explicit verdict, because
a test whose success condition is *inverted* must say so or it will be misread
as a regression.

---

## 2. Persistence guarantees

**Byte-exact across a process boundary.** 31 versions, **74.5 MB `memcmp`'d**
in a fresh process. `save → open → save` reproduces a byte-identical payload
over **739,810 bytes**, which is only possible if structural sharing survived
reload.

**Crash recovery.** 6 × `TerminateProcess` (the Windows SIGKILL: uncatchable,
no cleanup, no buffer flush) mid-edit. Every recovered version matched a
crash-free reference engine — up to **2,326 versions, 5.6 GB compared**, zero
mismatches. Recovery always landed in `[witnessed, witnessed+1]`, never fewer.

WAL forensics (negative controls, so the mechanism is proven not assumed):
deleting the log drops the store to its last checkpoint; a torn tail is
discarded, not misread; edits made after recovering from a torn/missing/stale
log survive the next open.

**genna-curate.** 24,484 versions of a 2.4 MB dataset → **4.3 MB on disk,
reopened in 18 ms**, `rollback` byte-exact after a restart.

---

## 3. Benchmarks vs real tools

| claim | documented | measured now | verdict |
|---|---|---|---|
| vs rsync 3.4.4, over the wire | 10× | **10.0×** (342,051 B vs 34,246 B) | **holds** |
| vs ffmpeg 9.0, structural cut | ~110,000× | **142,112×** (113.7 ms vs 0.0008 ms) | **holds** |
| vs MDS 0.13, bytes (like-for-like) | 2,755× | **1,170×** | **overstated — §5** |
| vs MDS 0.13, speed | 119× | **689×** | holds |
| vs git 2.55, bytes/edit | 60× | **18.1×** | **overstated — §5** |
| vs git, old-version read | 91× | 2,275× | **not comparable — see below** |
| byte-exactness, all of them | byte-exact | byte-exact every run | **holds** |

**MDS, in full** (WikiText-2, 36,718 samples, 100 edits, 101 versions both
sides). Harness now lives in the repo (`benchmarks/mds_vs_genna.py`) and runs
against real `mosaicml-streaming`:

```
GENNA        88,312 B in 2.2 ms, 101 versions, v0 byte-exact
MDS  shard rewrite   103,358,170 B  (the fair model)  ->  1,170x less, 689x faster
     whole rewrite  1,118,959,862 B  (naive)          -> 12,671x less
     batched           11,177,865 B  (MDS best case)  ->    127x less
```

The shard-rewrite row is the one to quote: shard boundaries come from the
`index.json` MDS itself wrote, both sides end with the same number of
recoverable versions, and MDS is used competently. Quoting 12,671× would be a
strawman; quoting 127× would compare 1 version against 101.

**The git old-version-read number is unusable, not a win.** It times
`git show`, which pays ~39 ms of process spawn per call on Windows. The ratio
mostly measures Windows process creation.

---

## 4. 3D / spatial

Real Stanford scans (bunny 35,947 v; armadillo 49,990 v; happy buddha 49,251 v).

**Geometry compression — Genna loses to Draco, decisively.**

| method | B/vertex | max error (nearest-neighbour) |
|---|---|---|
| **Draco qb11** | **2.07** | 6.5e-05 |
| **Draco qb14** | **3.19** | 8.1e-06 |
| zstd -19 (lossless) | 10.06 | 0 |
| Genna `GRID` | 12.00 | 2.5e-04 |
| Genna `EXACT` | 24.00 | 0 (bit-exact) |

Draco qb14 is 3.8× smaller *and* 30× more accurate than Genna's codec. Even
plain zstd beats Genna's exact mode losslessly. **Do not use the quantization
codec built here** — the right shape is Draco-compressed assets stored *in*
Genna for versioning.

**Instancing is not novel.** USD stores 20 instances in 436,060 B using
`SetInstanceable`; Pixar has shipped this for years. What Genna adds is that
the instance table is itself versioned.

**Versioned transform edits.** vs git + baked geometry: **971×** (packed).
vs git + a USD scene layer — the pipeline a competent studio already runs —
**1.3×, i.e. no storage win**, but **8,364× faster** (0.3 ms vs 2,880 ms).

**Content-defined chunking (added).** Same data, same edits, only chunking
differs:

| workload | fixed cuts | content-defined |
|---|---|---|
| move 1% region | 98.1% | 97.5% |
| **insert 1% verts** | 48.6% | **95.1%** |
| **delete 1% verts** | 49.5% | **98.7%** |
| jitter every vertex | 0.0% | 0.0% |

Insert/delete is what splat training does every step. Bypassing the shared
dictionary independently fixed localized edits (67% → 98%): the append-only
dictionary had been retokenizing identical bytes differently in later objects.

**Quantization against global jitter does not work, and cannot.** A coordinate
sits at a random position in its cell, so jitter of amplitude `a` crosses a
boundary with probability `≈a/2` (6% of all coordinates at step/8, measured).
Chunk dedup needs an *entire* chunk clean, so a chunk of `C` coordinates
survives with probability `(1−a/2)^C` — zero for any noticeable jitter.
Sharing only appears below ~step/1000, and a floor of ~0.08% (coordinates
sitting exactly on boundaries) caps it at ~41%.

**A sculpting session, 40 strokes** (`benchmarks/sculpt.py`), per-stroke cost —
the number an undo stack actually pays:

| strategy | B/stroke |
|---|---|
| whole (naive) | 431,364 |
| cdc | 7,027 |
| cdc + morton | 5,642 |
| **git** | **5,139** |
| **displacement** | **2,275** |
| **base + strokes** | **32** |

Morton ordering is real but modest (1.25×) — the Stanford scans are already in
near-spatial scan order. Displacement encoding beats git by 2.2×. Stroke
sourcing beats it by 159×, but charges **O(k) replay** (22 ms to reach state
20 vs 0.4 ms to read it from a store) and requires a bit-deterministic brush
(verified: replay reproduced the final state exactly).

---

## 4.5 Columnar tables, query surface, multi-writer

### Columnar layout — measured against the row-major path

Each column is its own Genna object; row deletion uses a **deletion vector**
(a validity bitmap) rather than rewriting columns. 20,000 rows × 5 columns.

| operation | columnar | row-major (JSONL splices) |
|---|---|---|
| relabel one column (10,000 values) | **+31,368 B** | **+785,670 B** |
| | | **25.0× more** |

After the relabel, versions per column were
`{id: 1, label: 2, text: 1, score: 1, source: 1}` — the other four columns
were not merely deduplicated, they were **not rewritten at all**.

| operation | cost |
|---|---|
| drop a label class (5,000 rows) | **+40 bytes**, zero data columns touched |
| dedup on one column | reads only that column |
| compact() | materializes the deletions when you want the space back |

22/22 checks, including every cell identical after save → reopen.

### Query surface

`diff(va, vb)` reports rows added / removed / cells changed, resolved through
a manifest (below). Verified: 167 rows removed are exactly the 'cat' rows;
`diff(v, v)` reports no change.

`range_history(offset, len)` answers "which versions touched this record?"
using structural sharing — if two versions reach the **same node** for a byte
range, the bytes underneath are the same memory, so the query walks pointers
instead of materializing versions.

```
range_history      :   3.3 us
materialize + diff : 110.4 us      -> 33.8x faster
```

**It is a conservative filter, not an oracle**, and the test asserts the right
property: **no false negatives** across 58 sampled records (every real change
reported), while producing 42 extra candidates for 6 true changes at this
chunk size. A record sharing a chunk with an edited record reports changed.
Use it to skip work, then confirm the candidates by reading them.

### Multi-writer optimistic concurrency

8 real processes racing on one store, 5 rounds:

```
round 0: base gen 1 -> 2 | won 1, conflict 7, locked 0, error 0
...
totals: 5 committed, 35 conflicted, 0 lock-timeout, 0 errors
ok  the store contains exactly the 5 winning edits - no lost update, no phantom
ok  all 8 writers landed via retry
```

Exactly one writer commits per generation; the rest are **told** they lost
rather than silently overwritten. There is deliberately **no merge** —
reconciling divergent histories needs semantics this layer does not have.

---

## 4.6 Deferred transforms (lazy tags)

Rewriting a column to change its values costs the whole column. Writing the
*rule* costs a few dozen bytes and composes. `benchmarks`/`test_lazy.py`,
20,000 rows:

| | bytes written |
|---|---|
| eager `relabel` (10,000 values) | **+30,600 B** |
| lazy `relabel_lazy` (same result) | **+313 B** (the op itself is **56 B**) |
| | **97.8× less** |

Write cost is **flat in rows affected** — 1,000 / 10,000 / 20,000 rows all cost
within 311 B of each other. Correctness is asserted first: the lazy path
produces values byte-identical to the eager rewrite.

**What it charges instead**, measured rather than waved away:

| read of a 20,000-row column | time |
|---|---|
| 0 pending ops | 0.68 ms |
| 16 pending ops | **4.06 ms (6.0×)** |
| after `compact_column()` | 0.64 ms |

The first implementation was **154× slower** on reads (a per-value Python
loop), which would have made the whole thing pointless — you cannot trade a
98× write win for a 154× read loss and call it an optimization. Vectorizing
brought it to 6.0×.

**Two kinds, and they are not the same thing.** `add_range(rows lo..hi, +d)`
is the range update lazy propagation is actually about. `SET label=2 WHERE
label=1` is a *predicate over values* — the matching rows are scattered, so
it is not a range update at all. It is still O(1) here, but as a value remap,
not as a range tag.

**Where this does NOT live: inside `gn_enode`.** Tags in the tree node would
mean pushing them down on every descent (the hot read path) and changing the
on-disk node format — the thing whose byte-exactness the rest of this report
is built on proving. The overlay gets the same complexity at the layer where
values are typed; the tree stores bytes and has no idea what "+1" means.

## 4.7 Model checkpoints — the spike, and it mostly fails

Real torch training, 9 checkpoints of a 2M-parameter model, four regimes.
`benchmarks/checkpoints.py`.

| regime | weights unchanged per step | vs storing every checkpoint whole |
|---|---|---|
| **fp32 full** | **0.00%** | **0.9× — worse than naive** |
| bf16 full | 29.61% | 1.3× |
| fp32 frozen (partial FT) | 74.96% | 2.8× |
| **fp32 LoRA** | **99.13%** | **7.6×** |

**The headline case does not work.** Under full-precision training with Adam,
*literally zero* weights are byte-identical between consecutive steps —
momentum moves every parameter that receives any gradient at all. Genna comes
out at 0.9×, i.e. slightly *worse* than storing each checkpoint whole, and git
(70,044,415 B) beats both Genna (80,370,010 B) and naive (75,755,520 B).

This is the same closed form as the 3D jitter wall: a chunk of `C` values
survives with probability `(1−p)^C`, and full-precision SGD has `p ≈ 1`.

**It works exactly where parameters receive no gradient**: LoRA (7.6×) and
frozen layers (2.8×). Note the honest caveat even there — if you are running
LoRA you would normally save only the adapter, in which case the naive
baseline is already small and Genna's advantage is convenience, not bytes.

**Forking a run** — the case the pitch specifically names — gives **1.25×**
on two branches sharing a 4-checkpoint prefix. That is just the shared-prefix
arithmetic (8 of 16 checkpoints identical); nothing about the engine makes the
diverged tails cheaper.

Verdict: **not the biggest thing here.** Worth knowing before anyone builds a
product on "checkpoint dedup".

## 4.8 Versioned sketches — built, but off by default

A HyperLogLog (1 KB, 1024 registers) stored beside each table version, so
"how many distinct values did this column have at version 1,847?" is a lookup
rather than a scan of a reconstructed version.

| | measured |
|---|---|
| accuracy vs exact | 0.06 – 4.0% relative error (100 → 100,000 distinct) |
| merge is a union | 7,432 vs true 7,500 (0.9%) — mergeable, so it composes |
| lookup vs rescanning the historical version | **48× cheaper** (0.100 ms vs 4.769 ms) |
| **cost at write time** | **511× more per commit** (27.17 ms vs 0.05 ms) |

**Defaulted OFF on that last row.** A table doing thousands of small edits
would be crippled computing cardinalities nobody asked for. `t.sketches(True)`
opts in. The write cost is dominated by BLAKE2b per value; Python's built-in
`hash()` is far faster but randomized per process, which would make persisted
sketches disagree between runs — so it is not an option.

## 4.9 Automaton aggregate — declined, with the reason

The proposal was to store a DFA transition function as the subtree aggregate,
making regex matching O(log n) after an edit and fixing the one publicly weak
number (search 128.9 ms vs raw `memmem` 62.0 ms).

**It would not fix that number.** The aggregate makes search cheap for a
**pre-registered** pattern — the DFA has to be known when the tree is built or
maintained, because the aggregate *is* that DFA's transition function. The
benchmark measures an **ad-hoc** needle supplied at query time, and no
precomputed aggregate helps there. Fixing the published number would need a
different mechanism (a suffix structure, or accepting the scan).

It is also the most invasive of the four: aggregates live in `gn_enode`, which
means the on-disk node format and the push-down path on every descent — the
two things this report's byte-exactness evidence rests on.

Worth building *if* the use case is "watch a fixed set of patterns over an
evolving corpus" (content filters, PII scanners, alert rules). That is a real
use case, and the aggregate is O(|states|) per node with composition as the
monoid. It is not what the search benchmark measures, so it is not a fix for
that row, and I have not built it.

---

## 4.10 Out-of-core: mapping the chunk store

`gn_save_ex(e, path, GN_SAVE_MAPPABLE)` writes a store whose chunk token
arrays are 4-byte aligned and uncompressed, so `gn_open` points each chunk's
`gn_tok*` straight into a memory mapping and copies nothing. Full detail and
the on-disk layout: **PERSISTENCE.md §2.5**.

Same data, same machine, compressed store vs mapped store
(`tools/run_oocore.sh`, incompressible random payload):

| payload | store on disk | compressed open | mapped open | lighter | open time |
|---|---|---|---|---|---|
| 8 MB | 32.1 MB | 33.3 MB | **1.0 MB** | **32×** | 169 ms → **1 ms** |
| 32 MB | 128.5 MB | 129.6 MB | **3.3 MB** | **39.8×** | 676 ms → **3 ms** |
| 96 MB | 385.0 MB | 388.2 MB | **5.2 MB** | **73.9×** | 2,060 ms → **13 ms** |
| 192 MB | 770.0 MB | 777.3 MB | **9.8 MB** | **79.3×** | 4,066 ms → **14 ms** |

The ratio grows with the store because a mapped open keeps only metadata
resident, which is near-constant, while a copied store tracks its own size.

Correctness at every size: a 1 MB read and 40 scattered 64 KB reads through
the mapping are `memcmp`-identical to the same reads from the heap store; a
mapped store is still writable (new chunks heap-owned, untouched ones still
borrowed); untouched data is byte-exact after editing.

**The first working version showed no win at all** — 33.1 MB resident against
33.3 MB. The cause was the integrity check: verifying a CRC over the whole
payload reads every byte, faulting in the entire mapping. The header now
carries `crc_len`, so `gn_open` verifies only the metadata prefix (everything
that drives pointer arithmetic) and `gn_verify_chunks()` checks the bulk on
demand, streaming it in 1 MB blocks. Format version 2 → **3**.

Costs, stated in the report rather than only in the header:

- A mappable store is **3.29× bigger on disk** (385 MB for a 96 MB payload) —
  it forfeits the payload zstd pass *and* carries one `u32` token per input
  byte.
- A mapped store with a **corrupt bulk still opens**. That is the deferred
  check, and `oocore_test` asserts it, so the gap cannot quietly change size.
  `gn_verify_chunks` catches a single flipped bit (tested both directions).
- **Creating** a large store still needs RAM for it; only *opening* does not.
  The measurements stop at 192 MB for that reason (15.3 GB RAM, 7.2 GB free
  disk). What is demonstrated is the scaling law, not an unmeasured open of a
  store larger than RAM.

## 4.11 Delta-encoded chunks — measured, and deliberately not built

Content addressing is exact-match, so a one-line edit produces a chunk 99.9%
identical to one already stored that shares nothing with it. git deltas
objects against similar objects in packfiles. That was the argument for
building delta encoding, and it does not survive measurement.

Identical corpus (8.18 MB of Redis source), identical 100 single-line
insertions at identical byte offsets — `benchmarks/git_delta_cmp.py` against
`tools/run_deltabench.sh`:

| | bytes/commit |
|---|---|
| git, loose objects (`gc.auto=0`) | 2,551,907 |
| git, after `git gc --aggressive` — **delta compression ON** | **872** |
| **Genna, saved-store growth** | **110** |
| Genna, WAL append per commit | **49** (0.45 ms/commit, survives SIGKILL) |

Genna is **7.9× better than git with delta compression fully enabled**.
Quoting git's loose-object number instead would have claimed ~23,000× and
meant nothing — an unrepacked repo is not a comparison anyone should make.

The reason is that the whole-payload zstd pass **is** the delta encoder:
content addressing collapses the exact duplicates, the compressor codes the
near-duplicates against each other.

That left one plausible limit — zstd's window at level 19 is 8 MB, so
redundancy further apart is invisible. Enabling long-distance matching with a
128 MB window is ten lines. Measured at 2,000 versions:

| | store bytes | save time |
|---|---|---|
| level 19, default window | **1,999,660** | 3,388 ms |
| level 19 + LDM, windowLog 27 | 1,999,724 | 3,613 ms |

64 bytes **larger** and 7% slower. The window was never the binding
constraint, so the change was reverted rather than kept. Scope: this covers
text with a learned dictionary and insert-shaped edits; a **mappable** store
forfeits the compressor entirely, and that is the one configuration where
per-chunk delta would still have something to do. See PERSISTENCE.md §2.6.

## 4.12 Aggregates in the node

The treap already carried subtree token and byte counts — the sum monoid over
leaf sizes — which is why range reads can prune. `-DGN_NODE_AGG` generalises
the slot: register an associative operation and any range aggregate costs
O(log n). Detail: **AGGREGATES.md**.

| query, 8 MB binary object | time |
|---|---|
| MAX over 4 KB | 0.0021 ms |
| MAX over the whole 8 MB | **0.0000 ms** |
| (for scale) `gn_read` of 4 MB | 17.0 ms |

Correct before fast: 200 random MAX ranges, 100 MIN, 100 SUM all match a
brute-force scan exactly; planted extremes are found; old versions keep their
own aggregates across an edit.

**Cost 1 — node width: 56 → 64 bytes (+14%)**, reported by
`gn_ext_node_size()`.

**Cost 2 — it fights out-of-core, which is the conflict flagged when this work
was scoped.** Registering a monoid makes every leaf compute its aggregate at
construction, which reads that leaf's chunk tokens; on a mapped store, reading
is faulting. Same store, one fresh process each:

| store | no monoid | `GN_AGG_MAX` attached | |
|---|---|---|---|
| 32 MB payload | 2.9 MB, 3 ms | **130.0 MB, 69 ms** | 44.9× resident, 23× slower |
| 192 MB payload | 10.0 MB, 11 ms | **777.1 MB, 367 ms** | **77.9×** resident, 33.4× slower |

777 MB is essentially the entire 768 MB mapping pulled in. The two features
are usable together only if you wanted the store resident anyway. Note the
cost is the *leaf computation*, not the node width: +8 bytes costs 14% of the
tree, computing leaf aggregates costs the whole payload.

## 4.13 Merge (confluent persistence)

`gn_commit` could detect divergence but never resolve it. `gn_merge(e, o,
base, other, &info)` performs a three-way merge into the object's latest
version, applied as **one splice on top of head** so structural sharing
survives. Detail: **MERGE.md**.

On a 200,000-byte document (`tools/run_merge.sh`):

| check | result |
|---|---|
| disjoint edits merge — head span [1000,1020), other [150000,150030) | **PASS** |
| merged document vs applying both edits by hand | **199,977 bytes, `memcmp` exact** |
| head / other / common ancestor all unchanged afterwards | **PASS** |
| overlapping edits **REFUSED** (`rc = -2`), no version appended | **PASS** |
| merging into an unchanged head fast-forwards | 199,993 bytes exact |
| merging head into itself — recognised as convergence, idempotent | **PASS** |
| merged store saves, reopens, byte-exact | **199,929 bytes** |

The expected document is spliced from the base array inside the test,
independent of Genna, and compared with `memcmp` — not re-derived through the
engine that produced it.

It **refuses rather than guesses**: overlapping changes have no correct merge
without knowing what the bytes mean. Each side's change is reduced to one
contiguous span (longest common prefix/suffix against the base), so a side
that made several scattered edits presents as one span — conservative in the
safe direction: it never merges what it should refuse, but refuses some things
a finer differ would take.

---

## 5. Errors found in my own measurements

Listed because every one of them changed a headline number, and all were in
Genna's favour before correction.

| # | error | effect |
|---|---|---|
| −2 | **CI had never run. Not once, on any commit.** `.github/workflows/ci.yml` triggered on `on.push.branches: [main]`; this repository's default branch is `master`. Every push matched nothing, GitHub reported no failures because it ran no jobs, and the repository presented as continuously tested for its entire public life. Found on 15 August 2026 by querying the Actions API for the run history instead of reading the workflow file. The first real run went **red on 7 of 8 jobs**, and the failures were genuine and had been latent from the beginning: a link probe that could not fail, `-Wl,-Bstatic` silently unsupported by Apple's linker so macOS fell back to dynamically linking Homebrew's libzstd, the Homebrew prefix absent from the include path, and `to_pandas_dtype()` pulling in pandas — which is not a dependency and was only ever satisfied by this machine having pandas installed ambiently. | **The largest error in this table.** Every cross-platform claim ever made about this project — including a "verified on three platforms" line that went into a deck — rested on reading a workflow file and never checking whether it had run. Fixed: trigger corrected, and `python/tests/test_ci_wiring.py` now asserts that every branch named in a push trigger actually exists, that every test file is named in CI or exempted with a stated reason, and that no C test is silently unrun. It was proven to fail against the original `[main]` before being trusted. |
| −1 | **The gate silently skipped a third of the Python suite and called it a pass.** The MSYS2 login shell the gate runs from does not expose the interpreter where `pyarrow`/`onnxruntime` are installed, so `test_table`, the table half of `test_query`, and `test_semantic` printed `SKIP`, **exited 0**, and were counted as passing. The index read `ALL PASS`. Found by diffing the gate's `python.log` against the same tests run directly — 22 table checks and 11 query checks had never executed in the gate. | a green gate that tested nothing in three suites. Fixed: the runner now selects an interpreter that can import the extras, prints which one and what it has, and **counts a skip as a failure** |
| 0 | **Conflated table versions with column versions.** Every object versions independently, so "table version 7" is not version 7 of an untouched column. `diff(v0, v1)` raised `IndexError` on a column with one version — and would have silently read the *wrong* version of a column that happened to have enough. Fixed with a manifest recording per-object versions per table version. | correctness bug in the query surface, found by its own test |
| 1 | **Node size hardcoded at 24 bytes**; a treap node is **56** (measured). | Understated Genna's own writes 2.33×, inflating every ratio derived from it. vs git: 41.7× → **18.1×**. vs MDS: 2,755× → **1,170×**. rsync/ffmpeg unaffected (they measure payload and time). |
| 2 | **Compared Genna's resident RAM to git's on-disk `.git`.** | Not a like-for-like comparison at all. |
| 3 | **Compared git's *incremental* growth to Genna's *whole* store.** git's figure excluded its base object, which for float geometry is most of the file. | Reported "git 9.7× smaller"; corrected: **total vs total, git 1.16× smaller**; incremental, git 2.18×. |
| 4 | **Draco error measured elementwise**, but Draco reorders vertices. Sorting does not fix it either (near-tied points swap). | Reported a 1.5e-01 error on a model 1.5e-01 across — i.e. "Draco is useless". Correct metric (nearest-neighbour): **6.5e-05**. |
| 5 | **USD measured as text `.usda`.** No studio ships 36k points as text. | 1.18 MB → **436 KB** with binary `.usdc`. |
| 6 | **`test_genna`'s rewrite baseline was dead-code-eliminated** by gcc 16 (`copy` written then freed unread), timing at 0.0 ms. | Headline ratio silently read `0x`; with a volatile sink, **604×**. |
| 7 | **The corrupt-store fuzzer has been vacuous in every compression-enabled build since compression was added.** It repairs the payload CRC after mutating, but computes it over the bytes *on disk* — which are zstd-compressed — while the loader checks the CRC of the *decompressed* payload. The two can never match, so all 1,200 mutants were rejected at the checksum and no structural validator ever ran. The campaign printed `0 problems` for months. Only the sanitized build, which happens to compile without zlib/zstd, was doing real work. | The headline "12 seeds × 100 CRC-repaired mutants" tested that CRC32 works. Fixed with a `GN_SAVE_RAW` save flag; the compressed build now reports **237 rejected, 13 loaded** and the campaign asserts `opened > 0` so it can never go vacuous silently again. |
| 8 | **I ranked delta-encoded chunks as the #2 highest-leverage change**, citing a 2.18× incremental advantage for git. Measured properly — same corpus, same edits, git allowed to repack — **Genna is 7.9× ahead of git with delta compression fully on** (110 vs 872 bytes/commit). | A feature I had committed to building, and argued for, turned out to be unnecessary. Not built; the reasoning and both numbers are recorded in PERSISTENCE.md §2.6 rather than quietly dropped. |
| 9 | **The first mapped-store measurement showed a 1.0× win and I nearly reported the feature as working**, because every correctness check passed. Resident was 33.1 MB against 33.3 MB. | The whole point of out-of-core is lower resident memory; the feature was worthless in exactly the dimension it existed for. Cause: the payload CRC read every byte. Fixed by splitting the CRC → **32×**, then **79×** at 192 MB. |
| 10 | **Measured the aggregate/out-of-core conflict in-process and got a physically impossible answer** — the monoid arm looked *lighter* (0.4 MB vs 2.9 MB). After several open/free cycles the working set stays high, so a later open appears free. | Would have reported "aggregates cost nothing out-of-core", the opposite of the truth. Re-measured with one fresh process per arm: **44.9× more resident at 32 MB, 78.0× at 192 MB**. |
| 11 | **`agg_test` reported the engine as broken when the test's model was wrong.** It compared post-edit aggregates against `GN_BYTE_BASE + byte`, but `gn_update` tokenizes through the dictionary, so the edited region holds dictionary ids. | Nearly "fixed" correct engine code. Test now checks against the object's real tokens; the byte-vs-token semantics are documented in `genna_agg.h`. |

---

## 6. Bugs found and fixed (each proven by a test)

| # | bug | found by |
|---|---|---|
| 1 | `gn_delete` freed the object without releasing its version trees — the whole node graph leaked | node-count assertion |
| 2 | `ver_from_root` left `ext`/`n_ext` uninitialized; `o->ver` is `realloc`'d so each version inherited stale bytes | writing a serializer that walks every version |
| 3 | **Three silent WAL data-loss bugs**: a missing, stale-generation, or torn-tail log was appended to anyway, so everything written afterwards was discarded at the next open | re-reading the recovery path; pinned by `verify_walfix.sh` |
| 4 | **heap-buffer-overflow** in `read_leaf` reachable from a corrupt store (extent past its chunk) | corrupt-store fuzzer under ASan |
| 5 | name-length `memcpy` into `char[64]` unbounded (up to 255); plus a leak on a loader error path | review of my own new code |
| 6 | `wal_append` returned silently on failed write/fsync — a full disk voided durability with no signal | review; now latches broken, `gn_wal_ok()` reports |
| 7 | **`Object.read()` passed a `LATEST` sentinel `gn_read_version` does not understand** — every latest-version read returned `b""`, and several checks *passed vacuously* comparing empty to empty | Python binding tests; suite now has a `nonempty()` guard |
| 8 | **A 1 KB read was O(total leaves), not O(log n)** — `read_ver` walked the whole tree | measuring the claim (`tests/readscale.c`) |

Bug 3, proven load-bearing by rebuilding the pre-fix behaviour behind
`-DGN_WALFIX_OFF`:

```
fix DISABLED: FAIL  6 edits after a torn tail    (178 -> 178)
              FAIL  5 edits after log rebuilt    (175 -> 175)
              FAIL  4 edits after a stale log    (175 -> 175)
fix ENABLED:  ok    6 / 5 / 4 edits all survive
```

Bug 8, measured before and after — a fixed 1 KB read as the object grows:

| object | leaves | before | after |
|---|---|---|---|
| 1 MB | 64 | 0.0037 ms | 0.0035 ms |
| 8 MB | 512 | 0.0054 ms | 0.0035 ms |
| 32 MB | 2048 | **0.0110 ms** | **0.0035 ms** |

Now flat. 3.1× at 32 MB and unbounded with size — a 10 GB object would have
visited ~640,000 leaves per 1 KB read. This one made an existing headline
claim actually true.

Found while building the four features above, each proven by a test that now
guards it:

| # | bug | found by |
|---|---|---|
| A | **Unbounded allocations in `snap_load_ex` from corrupt counts.** A flipped byte in the dictionary, chunk, node, object or version count made the loader `malloc` whatever it read — observed attempting **12.9 GB**. Every count is now checked against the bytes actually remaining (`rfits`) before any allocation. | the corrupt-store fuzzer, the moment it stopped being vacuous (error 7) — it found these on the first run |
| B | **Every reopened store had uninitialized aggregates.** The snapshot loader builds leaves through `gn_ext_mk_leaf_p`, not `mk_leaf`, and that path never computed the annotation — `node_alloc` only clears `rc`, so aggregates were whatever was in the arena. Plausible numbers, silently wrong. | added a save/open round trip to `agg_test`; nothing in the suite had ever reopened a store with a monoid attached |
| C | **A monoid attached before `gn_open` resolved chunks against the freed engine's store**, so every leaf got the identity and whole-object aggregates read 0 — and it is a use-after-free if the old engine is gone. `gn_open` now calls `gn_ext_monoid_bind()` with the engine it is loading. | the same round-trip test, which returned a uniform 0 |
| D | **`gn_save_ex` failed outright at a 96 MB payload** (`ENOSPC`-adjacent: three simultaneous copies of 384 MB of token data). Writing a store bigger than RAM must not need RAM; the bulk is now streamed from the live store to the file. | running `oocore_test` at a size larger than the first one tried |
| E | **`oocore_test` segfaulted instead of reporting a failure** when a store failed to open, dereferencing a NULL engine and burying the one clear failure line above it under a crash. | the `ENOSPC` case in D |
| F | **`run_corruptfuzz.sh` silently discarded the mappable results.** Its `grep "corrupted stores:"` does not match `corrupted MAPPABLE stores:`, so the whole mapped load path was fuzzed and its result thrown away. | reading the log after adding mappable coverage and finding only one line per seed |
| G | **The gate's index reported a passing verdict for a failing script.** It scraped the *last* verdict-shaped string in a log, so a runner whose C test printed `ALL PASS` and then failed a later shell-side comparison still showed `ALL PASS`. Runners now emit an explicit `VERDICT:` line, which the scraper prefers. | writing `run_oocore.sh`, whose shell-side comparison runs after the C test's verdict |

Pre-existing build defects also fixed: `make bench` linked `genna_vdict.c`
*alongside* `genna_dict2.c` when the former **replaces** the latter (that
target could never have linked); `vbench` dereferenced a NULL `FILE*` reading
`/proc/self/status` on any platform without `/proc`; `genna-curate`'s `export`
failed **silently** on an unwritable path.

---

## 7. Optimisations added, with their measured effect

| change | effect |
|---|---|
| **Snapshot payload compression** (zstd -19, else deflate) | text store **739,922 → 191,925 B (3.85×)**; geometry store **3.55 MB → 546 KB**. zstd over zlib specifically because zlib's 32 KB window cannot see cross-version redundancy. |
| **Range-limited read** (`gn_ext_walk_range`) | small reads now O(log n). 3.1× at 32 MB, unbounded with size. |
| **Binary path + content-defined chunking** | insert/delete sharing 49% → 95–99%; localized edits 67% → 98%; 6.6× less resident on evolving geometry. |
| **Morton ordering** | 1.25× per sculpt stroke. Real, modest. |
| **Node-size accounting fixed** | corrected two headline numbers downward (see §5). |

---

## 8. What is NOT verified

- **git old-version read** — measurement is process-spawn dominated (§3).
- **Leaks under ASan** — LeakSanitizer does not exist on Windows (ASan there
  answers "detect_leaks is not supported on this platform"). Covered by
  `leak_test`'s own `--wrap` accounting instead.
- **Concurrent writers** — detection (`gn_commit`) and now resolution
  (`gn_merge`) are tested; what is *not* tested is merge combined with the
  optimistic-commit loop across real processes. `merge_test` merges within one
  process.
- **A store larger than RAM, actually opened.** The mapping demonstrably keeps
  resident memory near-constant (§4.10), but *creating* a store still needs
  RAM for it, so the largest measured is a 192 MB payload / 770 MB store on a
  15.3 GB machine. The scaling law is measured; the headline case is
  extrapolated from it.
- **Bulk integrity of a mapped store at open.** Deliberately deferred — it is
  what buys the RAM win — and asserted as a gap in `oocore_test`.
  `gn_verify_chunks()` covers it on demand, so a caller who never calls it
  never checks it.
- **Aggregates are not persisted**, so every open recomputes them, which is
  what makes a monoid so expensive on a mapped store (§4.12). Storing them
  needs a monoid identity in the header so a store annotated under MAX cannot
  be read as SUM.
- **Merge precision.** Each side reduces to one contiguous span, so two sides
  that each made scattered edits can be refused where a finer differ would
  merge. Conservative in the safe direction, but the refusals are real and
  unmeasured in frequency.
- **Power-loss durability** — `fsync` is issued and the crash tests kill the
  process, but no test pulls actual power. That claim rests on `fsync`
  semantics.
- **Video-dictionary persistence** — `gn_save` returns `ENOTSUP` rather than
  writing a store that would reload wrong.
- **Delta encoding** — measured and deliberately not built (§4.11). Genna is
  7.9× ahead of repacked git on the text workload, so the case for it
  collapsed. It remains unmeasured on **mappable** stores, which forfeit the
  payload compressor and are therefore the one place it would still help.
- **Anything on a real user's data.** Every result here is a benchmark. No ML
  team has run this on their own corpus, which remains the single largest
  unknown and is not something more engineering can resolve.
