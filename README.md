# Genna

**A versioned data store that edits in place instead of rewriting: splice a
byte range in O(log n), keep every version, get the old bytes back exactly.**

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

## Four numbers

All measured on one machine, one run, against the real tools — not against a
model of them. Full method, and every number that came out worse than I first
reported it, in [`TEST_REPORT.md`](TEST_REPORT.md).

| | Genna | the tool it replaces | |
|---|---|---|---|
| Edit 1 sample in a 36,718-sample dataset | **88 KB written** | MosaicML Streaming, 103 MB (shard rewrite) | **1,170× less, 689× faster** |
| Sync 100 scattered edits in a 10 MB file | **34,246 B on the wire** | rsync 3.4.4, 342,051 B | **10.0× less** |
| Storage growth per commit, 8 MB source tree | **110 B/commit** | git 2.55 *after `gc --aggressive`*, 872 B | **7.9× less** |
| Open a 770 MB store | **9.8 MB resident, 14 ms** | reading it in, 777 MB, 4,066 ms | **79× less RAM** |

The git row compares against a **repacked** repo, with delta compression fully
on. Comparing against loose objects would have shown ~23,000× and meant
nothing.

---

## What it's bad at

Read this before the install instructions, on purpose.

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

## Install

```bash
pip install genna
```

No compiler, no build step, no dependencies — the wheel ships the engine as a
prebuilt shared library, and `pip list` shows exactly one package.

Then see the whole thing work, on data it generates itself, in about ten
seconds:

```bash
genna-demo
```

It curates a messy dataset, saves every intermediate version, reads the
original back byte-for-byte, answers "which versions touched this record?",
and rolls a step back — printing measured numbers from *your* machine, not
quoted ones:

```
  Genna store           0.30 MB   1841 versions, saved in 309 ms
  one raw copy          1.93 MB   1 version
  4 files by hand       7.52 MB   raw + one copy per step, which is
                                  what you would otherwise keep

  -> 25.0x smaller than keeping the 4 files, and it holds
     1841 versions rather than 4.
```

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

### Curating a dataset

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
fine-grained — three curation steps became 501 versions. Roll back to a
`Step` boundary rather than counting versions yourself.

Optional extras, none of them needed for the above:

```bash
pip install "genna[table]"      # columnar tables (pyarrow)
pip install "genna[semantic]"   # embedding near-dedup (onnxruntime)
```

**Requirements:** Python 3.8+, on Linux (x86-64, arm64), macOS (Intel, Apple
silicon) or Windows (x64). If no wheel matches your platform, pip falls back
to the sdist, which builds the C engine and needs a compiler
(`build-essential`, Xcode CLT, or MSYS2 mingw-w64).

---

## Building from source

```bash
git clone https://github.com/genna-engine/genna
cd genna
make ci          # builds the engine and runs the C test suite
pip install .    # builds and installs the Python package
```

The full gate — sanitizers, fuzz campaigns, crash recovery, the benchmarks
above — is `bash tools/run_all.sh`, which writes `test-logs/`. It is MSYS2/
Windows-specific; `make ci` is the portable subset.

### What has actually been verified on which platform

Every number in this README and in [`TEST_REPORT.md`](TEST_REPORT.md) was
measured on **one Windows machine**. State of cross-platform evidence as of
15 August 2026:

| | status |
|---|---|
| Python suite, Linux + macOS + Windows, 3.9 and 3.12 | **passing in CI** |
| C engine, Linux | **passing in CI** |
| C engine, macOS | **one test still failing** (`oocore_test`) |
| Every benchmark number quoted above | **Windows only, not independently reproduced** |

Until the last two lines change, treat the benchmark figures as measurements
from a single machine rather than as cross-platform results. The CI workflow
that establishes any of this had **never executed on a single commit** before
15 August 2026 — it triggered on a branch that does not exist in this
repository. See error −2 in [`TEST_REPORT.md`](TEST_REPORT.md).

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

**Alpha (0.1.0).** The on-disk format is at version 3 and older stores are
rejected rather than misread; expect it to move again before 1.0.

## License

MIT — see [`LICENSE`](LICENSE).
