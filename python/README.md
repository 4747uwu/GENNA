# genna — versioned datasets that don't rewrite themselves

Curating a training set means editing it: dedup, filter, relabel, drop the bad
shard. Every tool you have makes a **copy** for each of those steps, so you
either keep one version and lose the ability to answer "what did we change?",
or you keep ten copies of a 200 GB dataset.

Genna stores the dataset once and stores the *edits*. Every curation step is a
byte-exact versioned edit costing O(log n), so the full history is kept for
roughly the size of the edits rather than a copy per step.

```
24,484 curation steps on a 2.4 MB dataset  ->  4.3 MB on disk, reopens in 18 ms
                                               every step still rollback-able
```

---

## Install

```bash
pip install genna                    # core: no ML dependencies at all
pip install "genna[semantic]"        # + semantic near-dedup (onnxruntime, no torch)
```

Building from a checkout needs a C compiler (`gcc`/`clang`/MSYS2 mingw-w64).
There is no Python-ABI matching to get wrong: the native library is plain C
loaded with ctypes, so one build works across CPython versions and PyPy.

## Five minutes

```python
import genna

ds = genna.Dataset.from_jsonl("train.jsonl")
print(len(ds), "records")

ds.dedup()                      # exact duplicates
ds.filter_length(min_bytes=20)  # drop stubs
ds.drop_containing("lorem ipsum")
ds.replace("teh", "the")

print(ds.summary())
ds.save("train.genna")           # dataset AND its whole history, one file
```

Later, in another process:

```python
ds = genna.open("train.genna")
ds.rollback(0)                   # back to the original, byte-exact
ds.to_jsonl("original.jsonl")
```

Nothing above copies the dataset. `rollback` works after a restart because the
version graph itself is what was saved.

### Semantic near-dedup

```python
ds.near_dedup(threshold=0.45)
```

Runs `all-MiniLM-L6-v2` through onnxruntime — the real model, no torch. Measured
cosine similarities on the pairs in `tests/test_semantic.py`:

| pair type | this model | the old lexical method |
|---|---|---|
| whitespace / typo variants | 0.84 – 0.99 | 0.83 – 0.92 |
| **paraphrases** (same meaning, no shared wording) | **0.56 – 0.77** | 0.00 – 0.33 |
| unrelated sentences | < 0.12 | < 0.09 |

The lexical method cannot separate paraphrases from unrelated text at *any*
threshold (its worst duplicate scores 0.001, below its best non-duplicate at
0.085). The embedding model separates them with a 0.445 margin.

**Pick your own threshold.** `0.85` (the default) removes near-copies only, on
purpose — deleting training data you meant to keep is the expensive mistake.
`~0.45` also removes paraphrases. Measure a few of your own pairs first:

```python
from genna.embed import Embedder
Embedder().similarity("How do I reset my password?",
                      "What's the procedure for changing my login?")   # 0.587
```

## Lower level

```python
eng = genna.Engine()
eng.train(data)                      # learn the corpus language first
obj = eng.create("dataset", data)

obj.update(offset, delete=10, insert=b"...")   # a splice, O(log n)
obj.read(offset, length)                        # only this range is materialized
obj.versions[0].read()                          # any past version
eng.search("needle")                            # exact search, all objects

eng.save("store.genna")
eng.set_durability(fsync_every_edit=False)      # faster bulk curation
```

## What's actually true

Measured, with the tests that prove each (`python tests/test_core.py`):

- **Byte-exact.** 58 checks across the bindings, including 400 randomized
  splices differentially tested against a Python shadow buffer, all 256 byte
  values, embedded NULs, and 26 versions compared byte-for-byte after a reopen.
- **Crash-safe.** After `save()`, every edit is written to a write-ahead log
  and fsynced *before* it is applied. The C suite kills the process with
  SIGKILL mid-edit 6 times and recovers every committed edit; 2,326 versions
  compared against a crash-free reference (5.6 GB) with zero mismatches.
- **Clean under ASan + UBSan.**

## What is NOT true yet

- **No concurrent writers.** No file locking exists. One writer at a time; the
  engine is not thread-safe, and the bindings serialize calls with a process
  lock so Python threads cannot corrupt the shared node arena.
- **`save()` writes the whole store.** No incremental checkpoint, so
  checkpointing a large store costs O(store).
- **`open()` reads it all into RAM.** No mmap, no lazy load. A store bigger
  than memory will not open.
- **`near_dedup` is O(n²) in the worst case.** Blocked matrix multiply, fine
  into the tens of thousands of records; an ANN index is the scale path and is
  not built.
- **Records are line-delimited.** One record per line (JSONL or plain text).
  Parquet/Arrow ingestion is not built.
- Only the text dictionary can be persisted; the video module cannot, and
  `save()` refuses rather than writing a store that would reload wrong.

See `PERSISTENCE.md` for the on-disk format and the full list.

## Threading

The engine keeps process-global state (the treap node arena). ctypes releases
the GIL around every foreign call, so two Python threads really can be inside
the engine at once. Every mutating call in this package holds a module-level
lock, which makes it safe but serializes it. For parallelism, use one process
per store.
