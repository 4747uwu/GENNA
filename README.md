# Genna

**A versioned data store that edits in place instead of rewriting: splice a
byte range in O(log n), keep every version, get the old bytes back exactly.**

**MIT licensed.** One package, no dependencies, no build step.
[`LICENSE`](LICENSE)

---

## Install

```bash
pip install genna
```

The wheel ships the engine as a prebuilt shared library, so there is no
compiler step and `pip list` shows exactly one package.

**Requirements:** Python 3.9+, on Linux (x86-64, arm64), macOS (Intel, Apple
silicon) or Windows (x64). If no wheel matches your platform, pip falls back
to the sdist, which builds the C engine and needs a compiler
(`build-essential`, Xcode CLT, or MSYS2 mingw-w64).

---

## See it work

```bash
genna-example
```

Fifty lines, no benchmarks, nothing to configure. It ships inside the package,
so that command works straight after `pip install genna` — or run
[`examples/time_travel.py`](examples/time_travel.py) from a checkout, which
calls the same code. It writes a short document, edits it three times, then
goes back:

```
Wrote the first draft: 201 bytes, version 0.

Edit 1 - replaced the closing lines.        now version 1, 158 bytes
Edit 2 - added a title at the top.          now version 2, 173 bytes
Edit 3 - appended a closing line.           now version 3, 199 bytes

The notebook now has 4 versions and it never rewrote the file.

Here is version 0 again, straight out of the store:
...
Identical to what was written? True

What changed between version 2 and version 3?
  bytes  128-192  really changed:  doors down. I write until the light chan...
  (1 windows were skipped without reading a single byte)

Rolling back to version 1...
  content now matches version 1 exactly:    True
  and the history still holds every step:   5 versions
```

In code, that is the whole surface:

```python
import genna

e = genna.Engine()
o = e.create("train", open("data.jsonl", "rb").read())

o.update(1000, 20, b"corrected label")   # splice: O(log n), not a rewrite
o.update(5000, 0, b"appended record\n")

e.save("train.genna")                    # snapshot + write-ahead log
```

Every version stays addressable, and reopening gets the bytes back exactly:

```python
e = genna.open_store("train.genna")
o = e["train"]

o.read()                 # latest
o.versions[0].bytes()    # the original, byte-for-byte
len(o.versions)          # 3
```

There is also a packaged tour of the dataset layer, which generates its own
data and runs in about ten seconds:

```bash
genna-demo
```

---

## The rule that decides whether it helps you

Genna stores data as content-addressed chunks in a persistent tree. An edit
copies the path to the change and shares everything else, so a new version
costs the size of the *change*, not the size of the data.

That gives one rule, and it is the only thing you need to predict whether
Genna is worth your time:

> A chunk of `C` values survives an edit that touches a fraction `p` of values
> with probability **`(1 − p)^C`**.

**Sparse changes win.** Fix 200 labels in a 5 GB dataset, retrain one region
of a mesh, append to a log, patch a column: `p` is tiny, almost every chunk
survives, and the new version costs kilobytes.

**Diffuse changes do not.** Re-quantize every row, apply an Adam step to every
weight, re-encode a whole video: `p ≈ 1`, no chunk survives, and Genna stores
a second copy while a plain file would have stored one. It is *worse than
doing nothing* in that regime, and §"What it's bad at" has the measurement.

This is not a caveat hiding at the bottom of the page. It is the whole design.

---

## What it's bad at

**Full-precision model checkpoints — it does not work.** Nine checkpoints of a
2M-parameter model under Adam: **0.00%** of weights are byte-identical between
consecutive steps, because momentum moves every parameter that gets any
gradient. Genna lands at **0.9×** — *worse than storing each checkpoint
whole* — and git beats both. It only works where parameters receive no
gradient: LoRA **7.6×**, frozen layers 2.8×. If you came here for checkpoint
dedup on full fine-tunes, it will not help you.

**Geometry compression — Draco wins, decisively.** 2.07 B/vertex vs Genna's
12.00 in quantized mode, at lower error. Genna's advantage on 3D is *version
history of an evolving mesh*, not compressing one mesh. Use Draco for the
latter.

**Memory-mapped stores are 3.29× bigger on disk.** Zero-copy `mmap` means
giving up whole-payload compression: 385 MB on disk for a 96 MB payload. You
trade disk for not being bounded by RAM. Choose per store.

**Merge refuses more than it should.** Three-way merge is byte-exact on
disjoint edits and correctly refuses overlapping ones, but each side's change
is reduced to *one contiguous span*, so two sides that each made scattered
edits get reported as conflicting when a finer differ would have merged them.

**One machine.** There is no replication, no server, no distributed story.
Multi-writer is optimistic concurrency against a local file plus an advisory
lock.

**Every number here is a benchmark.** No team has run this on their own
corpus. That is the largest unknown and no amount of further engineering
fixes it.

---

## Four numbers

All measured on one machine, one run, against the real tools — not against a
model of them. Full method, and every number that came out worse than I first
reported it, in [`TEST_REPORT.md`](TEST_REPORT.md).

| | Genna | the tool it replaces | |
|---|---|---|---|
| Edit 1 sample in a 36,718-sample dataset | **88 KB written** | MosaicML Streaming, 103 MB (shard rewrite) | **1,170× less, 689× faster** |
| Sync 100 scattered edits in a 10 MB file | **34,246 B on the wire** | rsync 3.4.4, 342,051 B | **10.0× less** |
| Storage growth per commit, 8 MB source tree | **110 B/commit** | git 2.55 *after `gc --aggressive`*, 872 B | **7.9× less** |
| Open a 770 MB store (Windows) | **9.8 MB resident, 14 ms** | reading it in, 777 MB, 4,066 ms | **79× less RAM** |

The git row compares against a **repacked** repo, with delta compression fully
on. Comparing against loose objects would have shown ~23,000× and meant
nothing.

**The mmap row depends on both platform and payload size, so it needs
qualifying.** 79× is Windows, at a 770 MB store. The same comparison at an
8 MB payload measures **43.9× on Linux** and **18.7× on macOS**. The ratio
grows with store size, because what is saved is the payload you never fault
in while the fixed cost stays flat. Quote it with its platform and its size or
it means very little — and until 15 August 2026 the macOS figure could not
have been measured at all, because the resident-memory probe returned 0 on
that platform without saying so (error −1.5 in
[`TEST_REPORT.md`](TEST_REPORT.md)).

---

## Errors found in my own measurements

Fifteen so far, each one listed in
[`TEST_REPORT.md` §5](TEST_REPORT.md) with what it changed. Every one of them
was in Genna's favour before correction, which is the pattern worth knowing
about. The three most useful:

| | |
|---|---|
| **CI had never run.** The workflow triggered on `main`; the default branch is `master`. The project presented as continuously tested for its entire public life while running nothing. First real run: red on 7 of 8 jobs. |
| **The gate skipped a third of the Python suite and exited 0.** Missing extras printed `SKIP` and counted as passing. Skips are now failures. |
| **The macOS memory probe returned 0 for every reading.** `/proc/self/statm` does not exist there, so a measurement that could not be taken returned a number that looks like one. |

---

## Modules

Genna is the core. Adapters sit on top, and **the core never imports them.**

| module | status |
|---|---|
| `genna` (core engine) | **shipping** |
| `genna.dataset` (curation) | **shipping** — this is `genna-curate` |
| `genna.mapped` (out-of-core) | **shipping** |
| `genna.table` / `genna.sketch` | shipping, marked **unstable** |
| `langgraph-checkpoint-genna` | in development, separate repository |
| `genna.sweep`, `genna.kv`, `genna.config`, `genna.sync` | **not built** |

The last row is named rather than omitted. A repository advertising seven
modules with one working module reads as vapour; saying which ones do not
exist costs nothing and is the honest version.

The architectural rule, while it is still true and cheap to keep: **adapters
depend on the core, the core depends on nothing, and every adapter writes the
same file format.** A store written by one adapter opens in another.

---

## On-disk format and compatibility

The store carries a magic and an explicit format version, currently **v3**.

- Opening a store written by a **newer** Genna fails with a message naming
  both versions, rather than misparsing it.
- Opening a file that is **not a Genna store** says so, and a **truncated**
  store says something different again. Three outcomes, three messages.
- `genna.format_version()` reports what this build writes;
  `genna.store_format(path)` reports what a file on disk is.

**The policy, in plain words:**

> Within **0.x**, the on-disk format may change. Every change bumps the
> version, and ships either a converter or an explicit refusal that names the
> versions involved — never a silent misread. From **1.0**, the format is
> stable and stores written by any 1.x release stay readable by later 1.x
> releases.

The mechanism behind that promise is
[`python/tests/test_format_stability.py`](python/tests/test_format_stability.py),
which opens a committed fixture store and compares every version byte-for-byte
against a manifest recorded when it was written. If the format drifts, that
test fails on the machine that changed it.

---

## API stability

Every public name is marked **stable** or **unstable**. There is no unmarked
name, and that is enforced rather than promised:
[`python/tests/test_api_stability.py`](python/tests/test_api_stability.py)
fails if a public name has no mark, if a mark outlives the name it described,
or if anything marked stable has no test exercising it.

- **stable** — covered by a test; will not change shape within 0.x.
- **unstable** — may change or disappear in any 0.x release without a major
  bump. Usable, but pin your version.

Python: `Engine`, `Object`, `Version`, `GennaError`, `open`, `open_store`,
`Dataset`, `format_version`, `store_format` are **stable**. `Table`, `Schema`,
`Record`, `Stats` are **unstable**. The live table is `genna.__stability__`.

C: see the stability section at the top of
[`include/genna.h`](include/genna.h). The CRUD, persistence and history calls
are stable; `gn_ext_*`, `gn_net_*`, `gn_dict_*` and the token layer are not.

When in doubt a name is marked unstable. Unstable-and-honest beats
stable-and-regretted.

---

## Curating a dataset

`Engine` is the byte-level layer. For datasets you work in records, not
offsets — [`examples/curate_a_dataset.py`](examples/curate_a_dataset.py) is
runnable and prints exactly this:

```python
ds = genna.Dataset.from_jsonl("train.jsonl")   # 5,500 records, 0.43 MB

ds.filter_length(min_bytes=60)                 # removed 120
ds.drop_containing("LOREM IPSUM")              # removed  80
ds.dedup()                                     # removed 300

ds.engine.save("train.genna")
```

```
store: 0.07 MB on disk, holding ALL 501 versions
  (the raw file alone was 0.43 MB, and that is one version)
```

Every intermediate state is addressable, and reopening gets the exact bytes:

```python
ds2 = genna.Dataset.open("train.genna")
ds2.version_bytes(0) == open("train.jsonl", "rb").read()   # True
ds2.rollback(step.version_before)                          # undo one step
```

Each removed record is its own version, which is what makes rollback
fine-grained — three curation steps became 501 versions.

Optional extras, none of them needed for the above:

```bash
pip install "genna[table]"      # columnar tables (pyarrow)
pip install "genna[semantic]"   # embedding near-dedup (onnxruntime)
```

---

## Building from source

```bash
git clone https://github.com/4747uwu/GENNA
cd GENNA
make ci          # builds the engine and runs the C test suite
pip install .    # builds and installs the Python package
```

The full gate — sanitizers, fuzz campaigns, crash recovery, the benchmarks
above — is `bash tools/run_all.sh`, which writes `test-logs/`. It is MSYS2/
Windows-specific; `make ci` is the portable subset.

### What has actually been verified on which platform

The **correctness** suites pass on Linux, macOS and Windows, on machines
nobody here owns —
[run 31900068263](https://github.com/4747uwu/GENNA/actions/runs/31900068263),
8 of 8 jobs green, commit `b993f24`:

| | status |
|---|---|
| C engine — Linux, macOS (incl. ASan + UBSan) | **passing** |
| Python suite — Linux, macOS, Windows × 3.9, 3.12 | **passing** |
| Build a wheel, install *that*, run the example from outside the repo | **passing** |
| Every **benchmark number** quoted above | **Windows only, not independently reproduced** |

**One caveat that belongs here rather than buried.** CI installs zlib and
zstd on Linux and macOS but not on Windows, so the Windows build has no
compression — and a store written by a compressed build **cannot be opened by
a build without that codec.** Genna says so plainly when it happens (it names
the codec rather than calling your file damaged), but until Windows CI gets
zstd, stores are not freely portable between platform builds of the same
release. If you move stores between machines, check
`genna.open_store` succeeds on both, or save with `raw=True`.

That last row is the important one. CI proves the engine is *correct*
elsewhere; it does not re-measure the ratios. Treat every performance figure
in this README as a measurement from a single Windows machine until it is
reproduced on yours.

---

## Documentation

| | |
|---|---|
| [`TEST_REPORT.md`](TEST_REPORT.md) | Every measured number, plus the errors I made measuring them |
| [`PERSISTENCE.md`](PERSISTENCE.md) | On-disk format, crash guarantees, out-of-core, and what is *not* handled |
| [`MERGE.md`](MERGE.md) | Three-way merge and exactly when it refuses |
| [`AGGREGATES.md`](AGGREGATES.md) | O(log n) range aggregates, and what they cost out-of-core |
| [`SPATIAL.md`](SPATIAL.md) | 3D/mesh results, including the ones that failed |

## Status

**Alpha (0.1.0).** The on-disk format is at v3; see the compatibility policy
above for what that promises and what it does not.

## License

MIT — see [`LICENSE`](LICENSE).
