"""What using Genna actually looks like, end to end.

No byte offsets, no C, no build step. This is the workflow the engine exists
for: clean a dataset in steps, keep every step, and be able to go back to any
of them and get the exact bytes.

Run it:  python examples/curate_a_dataset.py
"""
import json
import os
import random
import tempfile

import genna

# ---------------------------------------------------------------- setup ---
# A messy dataset, the way they actually arrive: duplicates, empty rows,
# boilerplate, and a label someone got wrong.
random.seed(7)
work = tempfile.mkdtemp()
raw = os.path.join(work, "train.jsonl")

rows = []
for i in range(5000):
    rows.append({"id": i, "text": "sample document number %d about cats" % i,
                 "label": "cat"})
for i in range(300):                                   # exact duplicates
    rows.append(dict(rows[i]))
for i in range(120):                                   # empty / junk
    rows.append({"id": 90000 + i, "text": "", "label": "cat"})
for i in range(80):                                    # boilerplate to strip
    rows.append({"id": 91000 + i, "text": "LOREM IPSUM PLACEHOLDER",
                 "label": "cat"})
random.shuffle(rows)

with open(raw, "wb") as f:
    for r in rows:
        f.write(json.dumps(r).encode() + b"\n")

start_size = os.path.getsize(raw)
print("raw dataset: %d records, %.2f MB\n" % (len(rows), start_size / 1e6))

# ----------------------------------------------------------- curation ---
ds = genna.Dataset.from_jsonl(raw)
print("loaded: %d records" % len(ds))

# Order matters, and not in a subtle way: dedup() compares the `text` FIELD,
# not the whole record, so running it first would also collapse the 120 empty
# rows (all text "") and the 80 boilerplate rows into one each -- and the
# later steps would then look like they found nothing. Drop the junk first.
s1 = ds.filter_length(min_bytes=60)    # the empty ones
s2 = ds.drop_containing("LOREM IPSUM")  # boilerplate
s3 = ds.dedup()                         # records with duplicate text

for label, step in (("filter_length", s1), ("drop_containing", s2), ("dedup", s3)):
    print("  %-16s removed %5d records  (v%d -> v%d)"
          % (label, step.removed, step.version_before, step.version_after))

# Each removed record is its own version -- that is what makes rollback
# fine-grained, but it means "3 curation steps" is hundreds of versions.
# Step boundaries are the thing to roll back to.
print("\n  %d versions from 3 logical steps" % len(ds.versions))

print("\nafter curation: %d records" % len(ds))

# ------------------------------------------------------------- persist ---
store = os.path.join(work, "train.genna")
ds.engine.save(store)
print("\nstore: %.2f MB on disk, holding ALL %d versions"
      % (os.path.getsize(store) / 1e6, len(ds.versions)))
print("  (the raw file alone was %.2f MB, and that is one version)"
      % (start_size / 1e6))

# --------------------------------------------------------- time travel ---
# Reopen in a fresh handle, the way a later session would.
ds2 = genna.Dataset.open(store)
print("\nreopened: %d records, %d versions" % (len(ds2), len(ds2.versions)))

v0 = ds2.version_bytes(0)
with open(raw, "rb") as f:
    original = f.read()
print("  v0 is byte-identical to the original file: %s" % (v0 == original))

# "Actually, that dedup was too aggressive." Go back to the step boundary.
ds2.rollback(s3.version_before)
print("  rolled back to before dedup: %d records" % len(ds2))

# ------------------------------------------------------------- export ---
out = os.path.join(work, "clean.jsonl")
with open(out, "wb") as f:
    f.write(ds.bytes())
print("\nexported the curated version: %s (%.2f MB)"
      % (os.path.basename(out), os.path.getsize(out) / 1e6))
print("every intermediate version is still in %s" % os.path.basename(store))
