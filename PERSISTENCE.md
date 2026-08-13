# Genna persistence — on-disk format and guarantees

Genna was RAM-only: every version, chunk and edit died with the process. This
document describes the layer that fixes that, exactly as implemented in
`src/genna_persist.c`, and is honest about what it does not do.

```c
int        gn_save(gn_engine *e, const char *path);   /* whole engine -> disk */
gn_engine *gn_open(const char *path);                 /* disk -> ready to use */
void       gn_close(gn_engine *e);
```

A store is **two files**:

| file | what it holds |
|---|---|
| `<path>` | the snapshot: chunk table, dictionary, and the full version DAG |
| `<path>.wal` | every edit made since that snapshot, in order |

---

## 1. The design decision that matters: the DAG stays a DAG

In RAM, N versions of an object cost `O(N log n)` treap nodes, not `O(N·n)`,
because path copying leaves every untouched subtree **shared** between
versions. That sharing is the entire reason versioning is cheap.

A naive serializer would walk each version and write out its extents. That
destroys the sharing on disk: 24,484 versions of a 2.4 MB dataset would write
24,484 full extent lists.

So the node graph is serialized **as a graph**. A post-order walk over all
version roots emits each distinct node exactly once, with children referenced
by index; reload rebuilds the same graph with the same sharing.

Measured on a real curation session (`tools/run_curate.sh`):

```
24,484 versions of a 2.40 MB dataset  ->  4,388,100 bytes on disk
reopened in a new process             ->  18 ms
```

The test that proves the sharing survived is not a size check — it is that
**save → open → save reproduces a byte-identical payload** (739,810 content
bytes in `persist_test`). Had reload expanded any shared subtree into copies,
the second file would be larger.

Chunks are content-addressed and immutable, so the chunk table is written once
per distinct chunk, keyed by content hash. Dedup on disk is inherited from the
engine, not re-implemented.

---

## 2. Snapshot format

All integers are **little-endian, fixed-width**, so a store written by a
32-bit build reads on a 64-bit one.

### Header — 64 bytes

| offset | size | field |
|---|---|---|
| 0  | 8 | magic `"GENNAsnp"` |
| 8  | 4 | format version (`3`) |
| 12 | 4 | flags: bit0 = deflate, bit1 = zstd, bit2 = mappable |
| 16 | 8 | **generation** — bumped on every save; ties a WAL to its snapshot |
| 24 | 8 | payload length **as stored** (compressed) |
| 32 | 4 | CRC32 of uncompressed payload bytes `[0, crc_len)` |
| 36 | 8 | uncompressed payload length |
| 44 | 8 | `crc_len` — how much of the payload the CRC above covers |
| 52 | 4 | CRC32 of payload bytes `[crc_len, raw_len)` (the bulk; `0` if none) |
| 56 | 4 | CRC32 of header bytes `0..55` |
| 60 | 4 | reserved, zero |

For an ordinary store `crc_len == raw_len`: the CRC covers everything, exactly
as in version 2. The split exists only for mappable stores — see §2.5.

### Payload compression

The payload is compressed as a whole at save time — zstd level 19 when
available, else deflate, else raw. The read path, chunk store and version DAG
are untouched; only the encoding on disk changes, and the CRC is taken over
the *uncompressed* bytes so integrity is checked on the real data.

This matters more than it sounds. The store keeps chunks as `gn_tok` (u32), so
a binary byte becomes `GN_BYTE_BASE + b` — three bytes in four are `0xFF`.
Leaving that raw was costing ~4× for nothing.

zstd is preferred over deflate for one specific reason: zlib's window is 32 KB,
so it cannot see that a chunk near the end of the payload is nearly identical
to one near the start. On a store holding many versions of the same data that
is exactly the redundancy worth finding.

Measured:

| store | before | deflate | zstd |
|---|---|---|---|
| 2.4 MB text corpus, 31 versions | 739,922 | 218,404 | **191,925** (3.85×) |
| evolving point cloud, 21 states | 3.55 MB resident | 894,535 | **546,574** |

Format version 3 is not backward compatible with 2, which was not compatible
with 1; an older store is rejected rather than misread.

### Payload — five sections, in order

**1. Dictionary**
```
u32  n_entries
u64  texts_len
u8   texts[texts_len]          // one blob, all entry text concatenated
{ u32 off; u32 len; } × n_entries
```
The hash index is *derived state* and is rebuilt on load, in increasing id
order. Linear probing with no deletions has the property that insertion order
changes where a key sits but never whether it is found, and low-ids-first
preserves `dict_add`'s rule that the first id to claim a text keeps it — so
every lookup answers exactly as it did before the save.

**2. Chunk store**
```
u32  n_chunks
u64  dedup_counter
{ u64 cid; u32 n_tokens; u32 tokens[n_tokens]; } × n_chunks
```
Chunks are re-`put` in their original order so the open-addressed index lands
in the same shape. The stored `cid` is verified against the hash recomputed
from the tokens; a mismatch fails the load.

**3. Version DAG**
```
u32  n_nodes
per node, in post-order (children always at a lower index):
  u8 kind
  kind 0 (leaf):  u64 chunk_cid; u32 off; u32 len; u64 bytes; u32 prio
  kind 1 (inner): u32 left_idx;  u32 right_idx;    u32 prio
```
`0xFFFFFFFF` is the NULL index. Subtree token/byte totals are **recomputed
from children** on load rather than read from the file, so a corrupted total
cannot make a tree lie about its own size; only leaf byte counts are trusted,
and those are cross-checked against each version's recorded totals.

**4. Objects**
```
u32  n_obj
per object:
  u8   name_len; u8 name[name_len]
  u32  n_ver
  { u32 root_idx; u64 total_tokens; u64 total_bytes; u64 dict_version; } × n_ver
```
On load, each version's tree is checked against its recorded
`total_tokens`/`total_bytes`; a mismatch fails the load.

**5. Stats** — six `u64` counters (`gn_stats`).

### 2.5 Mappable stores (out-of-core)

`gn_open` on an ordinary store reads the whole file, decompresses it, and
copies every chunk into the heap. That caps a store at available RAM — which
for real datasets is the binding constraint long before any compression ratio
matters.

`gn_save_ex(e, path, GN_SAVE_MAPPABLE)` writes a store that can be
memory-mapped instead. The payload is reordered into two regions:

```
[ header 64B ][ metadata ][ pad to 4 ][ chunk token data ]
               ^-------- CRC'd ------^ ^-- mapped, not read at open
```

Chunk records in the metadata carry only `{u64 cid; u32 n_tokens}`; the token
arrays themselves are streamed into the trailing region in chunk-index order,
so the loader walks their offsets cumulatively. `gn_open` then points each
chunk's `gn_tok*` **straight into the mapping** and copies nothing. Pages are
faulted in by the OS only where the tree is actually read.

**Why the CRC had to be split.** The first working version of this was
byte-exact and still showed no memory win at all: 33.1 MB resident against
33.3 MB for the compressed store. The cause was the integrity check itself —
verifying a CRC over the whole payload reads every byte, which faults in the
entire mapping and destroys the only thing the feature was for. Splitting the
CRC so `gn_open` covers just the metadata took the same measurement to 1.0 MB.

What is still checked eagerly is everything that drives pointer arithmetic:
dictionary offsets, chunk lengths, node indices, extent ranges, and each
version's byte/token totals. Chunk *content* corruption yields wrong bytes,
not unsafe memory. It is not unchecked, only deferred: the bulk has its own
CRC32 in the header, and `gn_verify_chunks(path)` streams the tail and
verifies it on demand, in 1 MB blocks so that verifying a store bigger than
RAM does not require RAM.

**Measured** (`tools/run_oocore.sh`, incompressible random payload):

| payload | store on disk | compressed open | mapped open | resident | open time |
|---|---|---|---|---|---|
| 8 MB  | 32.1 MB | 33.3 MB | **1.0 MB**  | **32×** lighter | 169 ms → **1 ms** |
| 96 MB | 385.0 MB | 388.2 MB | **5.2 MB** | **74×** lighter | 2060 ms → **13 ms** |

The ratio grows with the store because mapped resident is essentially the
metadata only, which is near-constant, while a copied store tracks its own
size. Both sizes pass byte-exactness: a 1 MB read and 40 scattered 64 KB reads
through the mapping are `memcmp`-identical to the same reads from the heap
store, and a mapped store is still writable — new chunks are heap-owned while
untouched ones stay borrowed.

**What it costs, honestly.** A mappable store is **3.29× bigger on disk**
(385 MB for a 96 MB payload, 4.0× the payload itself). Two reasons, both
unavoidable here:

- You cannot point a `gn_tok*` into compressed bytes, so the whole-payload
  zstd pass is forfeited.
- The store holds one `u32` token per input byte for binary data, so chunk
  data is 4× the payload before any encoding.

That is the trade in one line: **more disk in exchange for not being bounded
by RAM.** It is chosen per store, and the reader detects which kind it got.

**Limits.**
- Zero-copy requires a little-endian host, since the mapped bytes *are* the
  `gn_tok` array, and a mapping base that is 4-byte aligned. On a big-endian
  host, or if `mmap`/`MapViewOfFile` refuses, the loader falls back to copying
  and byte-swapping — correct, but with no RAM win, and
  `gn_store_is_mapped()` reports 0. The file is written little-endian
  regardless of host, so a store written on a big-endian machine still maps on
  a little-endian one.
- **Creating** a large store still needs RAM for it; only *opening* does not.
  The measurements above stop at a 96 MB payload for that reason, on a machine
  with 15.3 GB RAM and 7.2 GB free disk. The claim demonstrated is the scaling
  law, not an unmeasured open of a store larger than RAM.
- A mapped store with a corrupted bulk **still opens** — that is the deferred
  check, and it is asserted as such in `oocore_test.c` so the gap cannot
  quietly close or quietly widen.
- The mapping is released by `gn_engine_free`/`gn_close`. Truncating or
  overwriting the file underneath a live mapped engine is undefined; the
  advisory store lock is the intended guard.

### 2.6 Delta-encoded chunks: measured, and not built

Content addressing is exact-match, so a one-line edit inside a 4 KB chunk
produces a chunk 99.9% identical to one already stored that shares nothing
with it. git does not have that problem — packfiles delta objects against
similar objects. That is a real argument for adding delta encoding, and it was
the plan.

The measurement says no. Same corpus (8.18 MB of Redis source), same 100
single-line insertions at the same byte offsets, `benchmarks/git_delta_cmp.py`
against `tools/run_deltabench.sh`:

| | bytes/commit |
|---|---|
| git, loose objects (`gc.auto=0`) | 2,551,907 |
| git, after `git gc --aggressive` — **delta compression ON** | **872** |
| **Genna, saved-store growth** | **110** |

Genna is already **7.9× better than git with delta compression fully
enabled**. Quoting git's loose-object number instead would have claimed 23,000×
and meant nothing — git writes a whole recompressed blob per commit until it
repacks, so an unrepacked repo is not the comparison anyone should make.

The reason is that the **whole-payload zstd pass is itself the delta encoder**:
it sees the near-identical chunks and codes them against each other. Content
addressing gets the exact duplicates; the compressor gets the near-duplicates.

That suggested one residual limit worth fixing — zstd's window at level 19 is
8 MB, so redundancy further apart than that is invisible. Enabling long-distance
matching with a 128 MB window is ten lines. Measured at 2000 versions:

| | store bytes | save time |
|---|---|---|
| level 19, default window | **1,999,660** | 3388 ms |
| level 19 + LDM, windowLog 27 | 1,999,724 | 3613 ms |

64 bytes **larger** and 7% slower. The window was never the binding constraint,
so the change was reverted rather than kept for the sake of having made one.

**Scope of this result.** It covers text with a learned dictionary, CDC
chunking, and insert-shaped edits. It does not establish that delta encoding is
worthless everywhere — in particular a **mappable** store forfeits the payload
compressor entirely (§2.5), so the near-duplicate redundancy is simply not
recovered there, and that is the one configuration where per-chunk delta would
have something to do. It is not built, and out-of-core stores pay for it in
disk.

### Atomicity

`gn_save` writes `<path>.tmp`, fsyncs it, then renames it over `<path>`
(`MoveFileEx` with `REPLACE_EXISTING|WRITE_THROUGH` on Windows, `rename()`
plus a directory fsync on POSIX). A crash at any point leaves either the
previous snapshot or the new one, never a half-written file.

---

## 3. Write-ahead log

Once an engine is bound to a path (by `gn_save` or `gn_open`), **every**
mutating verb appends a record and fsyncs it *before* the edit is applied in
memory. That ordering is the whole guarantee: if an edit is visible to anyone,
its record is already durable.

The hook lives at the head of each engine mutator (`gn_create`, `gn_update`,
`gn_delete`, `gn_graft`, `gn_cut`, `gn_trim_history`), so it cannot be
bypassed by forgetting to call a wrapper. It is a no-op when no WAL is
attached, and suppressed during replay.

### Header — 32 bytes
```
0  8  magic "GENNAwal"
8  4  format version (1)
12 4  flags
16 8  generation  (must equal the snapshot's, or the log is ignored)
24 4  CRC32 of bytes 0..23
28 4  reserved
```

### Records
```
u32 payload_len
u32 CRC32(payload)
u8  payload[payload_len]
```
Payload is an op byte followed by its arguments:

| op | payload |
|---|---|
| 1 CREATE | name, `u64` len, bytes |
| 2 UPDATE | name, `u64` off, `u64` del, `u64` len, bytes |
| 3 DELETE | name |
| 4 GRAFT  | dst name, src name, `u64` dst_off, `u64` src_off, `u64` src_len |
| 5 CUT    | name, `u64` off, `u64` len |
| 6 TRIM   | name, `u32` keep |

(name = `u8` length + bytes, max 63.)

### Replay

`gn_open` loads the snapshot, then replays records in order, stopping at the
first record that is short or fails its CRC — that is the tear left by the
kill, and everything after it is by definition not committed. A log whose
generation does not match the snapshot belongs to a checkpoint already
superseded, and is ignored entirely.

Replay always starts from the snapshot, so replaying the same log twice (crash,
recover, crash again) is idempotent.

### Durability modes

| `gn_wal_set_sync` | survives SIGKILL | survives power loss |
|---|---|---|
| `1` (default) | yes | yes |
| `0` | yes | **no** |

With sync off the bytes are still handed to the OS, and the page cache
outlives the process — so a killed process loses nothing, but a power cut can.

---

## 4. What is proven, and by which test

| guarantee | test | measured |
|---|---|---|
| Byte-exact across a process boundary | `tests/persist_test.c` | 31 versions, 74.5 MB `memcmp`'d in a fresh process |
| Structural sharing survives reload | `tests/persist_test.c` | save→open→save byte-identical over 739,810 bytes |
| Engine still writable after reload | `tests/persist_test.c` | post-reload edit correct; v0 still exact |
| No corruption after SIGKILL | `tests/crash_test.c` | 6 kills; every version equals a crash-free reference (5.1 GB compared) |
| No lost committed edits | `tests/crash_test.c` | recovered ≥ witnessed, every round |
| Torn tail discarded, not misread | `tests/crash_test.c` | garbage appended to WAL; edit count unchanged |
| The WAL is what carries the edits | `tests/crash_test.c` | deleting it drops to the last checkpoint |
| Randomized edits round-trip | `tests/fuzz_test.c` | 2,000 splices/seed × 12 seeds vs a shadow buffer |
| No allocator leaks | `tests/leak_test.c` | `--wrap` accounting, 0 bytes outstanding per phase |
| Corrupt stores never crash the loader | `tests/fuzz_test.c` `fuzz_corrupt` | 1,200 CRC-repaired mutants, 12 seeds, ASan+UBSan clean |

Run them:
```sh
bash tools/run_suite.sh        # everything, plain build
bash tools/run_sanitized.sh    # everything, -fsanitize=address,undefined
bash tools/run_leak.sh         # allocator accounting
bash tools/fuzz_campaign.sh    # many seeds
bash tools/run_curate.sh       # save -> exit -> open -> rollback
```

---

## 5. What this does NOT handle

Stated plainly, because the alternative is someone finding out in production.

**Concurrency**
- **Optimistic concurrency, detection only — no merge.** `gn_commit(e, path,
  expected_gen)` saves only if the store is still at the generation the writer
  loaded; otherwise it returns `GN_CONFLICT` and the writer must reload and
  redo. Measured with 8 real processes racing over 5 rounds: exactly one
  writer committed per generation, the other seven were told they lost, the
  store held exactly the winning edits, and a retry loop landed all 8.
- **There is deliberately no merge.** Reconciling two divergent edit histories
  needs semantics this layer does not have and should not invent. Genna tells
  you there was a conflict; resolving it is the application's job.
- The store lock is **advisory**: a writer that calls `gn_save()` directly
  instead of `gn_commit()` is not stopped. It is also not stealable on a
  timeout — a writer killed while holding it leaves the file behind, and
  `gn_lock_break()` is manual on purpose, because auto-stealing is how two
  writers come to believe they both hold it.
- No reader/writer isolation beyond snapshot atomicity: a reader opening
  mid-checkpoint sees whichever snapshot the rename has landed on.
- No concurrent reader/writer isolation. A reader opening a store while
  another process is mid-checkpoint sees whichever snapshot the rename has
  landed on — consistent, but possibly stale by one checkpoint.
- The engine itself is not thread-safe, and persistence does not change that.

**Scale and shape**
- `gn_save` writes the **whole** store every time. There is no incremental or
  differential snapshot, so checkpointing a large store costs `O(store)`.
  Cheap edits plus expensive checkpoints is a deliberate trade; pick the
  checkpoint interval accordingly.
- `gn_open` reads the entire snapshot into RAM. No mmap, no lazy/partial
  load, so opening costs `O(store)` memory and time. A store larger than RAM
  will not open.
- The WAL grows without bound until the next `gn_save`. Nothing checkpoints
  automatically; replay time is proportional to log length.
- Dictionary text offsets are `u32`, capping the dictionary text blob at 4 GB.

**Integrity**
- Torn-record detection is CRC32, so a corrupt record has a ~2⁻³² chance of
  being accepted as valid. CRC32 is an integrity check, **not** a security
  one.
- The loader *is* fuzzed against corrupt input: `fuzz_corrupt` in
  `tests/fuzz_test.c` mutates a valid store and **repairs the checksums**, so
  the mutation reaches the structural validators instead of being bounced by
  the CRC. 1,200 mutated stores across 12 seeds load-or-reject and are fully
  read under ASan+UBSan with no error (`tools/run_corruptfuzz.sh`). All reads
  are bounds-checked, and node indices, name lengths, extent ranges and
  per-version totals are validated.
- Even so, treat a store as **trusted input**. Byte-count corruption that
  still sums to the recorded per-version totals is not separately detected,
  and the parser has not been reviewed as a security boundary.
- No per-chunk checksums; the payload has one CRC covering everything.
- No encryption, no compression of the snapshot.

**Alternative language modules**
- **Only the text dictionary (`genna_dict2.c`) can be persisted.** The video
  module `genna_vdict.c` replaces the same `gn_dict_*` interface but keeps
  `u64` blob offsets and a per-frame IDR flag, which the snapshot's dictionary
  section cannot represent. It therefore declares itself non-serializable and
  **`gn_save` returns `-1`/`ENOTSUP`** on such an engine rather than writing a
  store that would reload with truncated offsets and lost IDR flags. Persisting
  a video store needs a per-module, versioned dictionary section — not built.

**Behaviour**
- Treap priorities are persisted, but the priority RNG's *state* is not, so
  trees built after a reload draw from a fresh sequence. This changes tree
  shape, never content — all byte-exactness tests pass across reloads.
- `gn_delete` followed by `gn_create` of the same name replays in order and is
  fine, but the engine has never deduplicated object names; two objects may
  share a name and lookup returns the first.
- `gn_ext_arena_free()` is process-global: it releases the treap arena for
  *all* engines, so it is only safe when no engine holds live nodes.

**Platform**
- LeakSanitizer does not exist on Windows (ASan there reports "detect_leaks is
  not supported on this platform"), so leak coverage comes from
  `tests/leak_test.c`'s own `--wrap` accounting instead. ASan and UBSan
  themselves work and the suite is clean under both.

---

## 6. Changes made to the engine

Persistence is a layer, not a rewrite. The engine changes were:

- `struct gn_engine` gained one field: an opaque `void *wal`.
- One `gn_wal__*` logging call at the head of each mutator.
- Read-only accessors so the on-disk layer can see the store, object table
  and dictionary without those structs entering the public header.
- `gn_ext_mk_leaf_p` / `gn_ext_mk_inner_p` plus node getters, for rebuilding
  the DAG.

Four bugs were fixed in passing, each found by work on this feature:

1. **`gn_delete` leaked every version tree.** It freed the object without
   releasing `ver[v].root`, so an object's entire node graph stayed live.
   Proven fixed by a node-count assertion in `tests/leak_test.c`.
2. **`ver_from_root` left `ext`/`n_ext` uninitialized.** `o->ver` is grown
   with `realloc`, so each new version inherited whatever bytes were there.
   Unused by this engine, but genuinely uninitialized memory.
3. **Three silent data-loss bugs in `gn_open`'s log handling**, all found by
   re-reading the recovery path rather than by a failing test — then each
   pinned down by one. In every case the store *looked* durable and was not:

   - **Missing log.** `gn_open` did `fopen(wpath, "ab")` unconditionally,
     which *creates an empty, headerless log*. Everything written to it
     afterwards failed the magic check on the next open and was discarded.
   - **Stale generation.** A log left by a superseded snapshot was correctly
     ignored for replay — and then appended to anyway, so the new records
     were ignored too.
   - **Torn tail.** Replay correctly stops at the tear a kill left behind,
     but the append point was the *end of file*, past the garbage. The next
     open stopped at the same tear and never saw anything written since.

   `gn_open` now rebuilds a valid log when the existing one is missing,
   corrupt or stale, and truncates back to the end of the last intact record
   when replay stopped early.

   `tools/verify_walfix.sh` proves those tests are load-bearing by rebuilding
   the pre-fix behaviour behind `-DGN_WALFIX_OFF`:
   ```
   fix DISABLED: FAIL  6 edits after a torn tail    (178 -> 178)
                 FAIL  5 edits after log rebuilt    (175 -> 175)
                 FAIL  4 edits after a stale log    (175 -> 175)
   fix ENABLED:  ok    6 edits after a torn tail    (169 -> 175)
                 ok    5 edits after log rebuilt    (150 -> 155)
                 ok    4 edits after a stale log    (150 -> 154)
   ```

4. **`gn_open` name collision.** The old object-lookup `gn_open(engine, name)`
   is now `gn_object_open`; `gn_open(path)` is the store opener. No caller in
   the tree used the old name.

5. **A heap-buffer-overflow reachable from a corrupt store** — found by the
   corrupt-store fuzzer under ASan, not by inspection. The engine assumes
   every extent lies inside its chunk (it built them all, so `read_leaf`
   indexes chunk tokens without checking). A store whose leaf carried an
   out-of-range `(off, len)` therefore read past the chunk allocation:
   ```
   ERROR: AddressSanitizer: heap-buffer-overflow
     READ of size 4 in read_leaf src/genna_engine3.c:260
       <- gn_ext_walk <- read_ver <- gn_read_version
   ```
   Fixed at the trust boundary: the loader now verifies each leaf's chunk
   exists and that `off + len <= chunk_tokens`, rather than adding a bounds
   test to the engine's hot read path.

Related hardening: `wal_append` used to return silently when a write or fsync
failed, so a full disk would void the durability guarantee with no signal. It
now latches the log broken and `gn_wal_ok()` reports it.

Two further defects were caught by review of this layer's own code before it
shipped: a name-length byte from a store file was `memcpy`'d into
`gn_object::name[64]` without a bound (up to 255), and an error path in the
object loader freed a partly-built object without releasing the version roots
it had already retained.

Two test-side fixes:

4. **`test_genna`'s rewrite baseline was being optimized away.** `copy` was
   written and freed without ever being read, so modern GCC deleted the whole
   `malloc`/`memcpy`/`free` chain and timed it at 0.0 ms — silently turning
   the headline "genna vs rewrite" ratio into `0x`. A volatile sink fixes it.
5. **`genna-curate`'s `export` failed silently** on an unwritable path,
   which made a broken export indistinguishable from a working one.
