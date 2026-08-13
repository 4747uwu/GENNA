# Merge (confluent persistence)

Genna's concurrency layer could always *detect* divergence — `gn_commit`
returns `GN_CONFLICT` when the store moved under a writer — but never resolve
it. A writer whose base had moved had to reload and redo its work. That is
correct and useless: two people editing opposite ends of a 200 KB document
made one of them start over.

`gn_merge` resolves the case where resolution is well defined.

```c
#include "genna_merge.h"

gn_merge_info info;
int rc = gn_merge(e, o, base_version, other_version, &info);
if (rc == GN_MERGE_CONFLICT) {
    /* both sides changed overlapping bytes; nothing was appended */
}
```

```
base ── head        the object's LATEST version (what you have)
   └─── other       what someone else did from the same ancestor
```

On success a new version is appended holding both sides' changes. It is
applied as **one splice on top of head**, so structural sharing is preserved —
the merge does not rewrite the object.

---

## It refuses rather than guesses

If both sides changed overlapping regions there is no merge that is right
without knowing what the bytes *mean*. `gn_merge` returns `GN_MERGE_CONFLICT`,
appends nothing, and reports both spans so a caller can show them. Silently
preferring one side is how merge tools lose data, and it passes every
happy-path test while doing it — which is why the conflict case is tested as
hard as the clean one.

## Convergence is not conflict

Both sides making a byte-identical change to the same span is two writers
agreeing, not clashing. Identical spans overlap by definition, so the naive
rule refuses a merge that has nothing to decide — including merging a version
into itself. `gn_merge` compares the replacement bytes and treats an exact
match as a no-op, reporting `info.identical`. The comparison is by content,
so it also covers two separately-authored identical edits.

## Precision, stated honestly

Each side's change is reduced to **one contiguous span**: the region between
its longest common prefix and longest common suffix with the base. A side that
made several scattered edits therefore presents as one span covering all of
them.

The consequence: two such sides can be reported as conflicting where a
finer-grained diff would have merged them. This is conservative in the safe
direction — it never merges something it should have refused, but it does
refuse some things a smarter differ would take. A per-leaf diff using the fact
that unchanged subtrees are *the same pointer* would tighten this; it is not
built.

The suffix scan is capped so the prefix and suffix cannot cross. Without that
cap, base `"aaaa"` against side `"aa"` claims a 2-byte prefix and a 2-byte
suffix of a 2-byte string, producing a negative-length span.

---

## What is proven, and by which test

`tools/run_merge.sh` → `tests/merge_test.c`, on a 200,000-byte document:

| check | result |
|---|---|
| disjoint edits merge (head span [1000,1020), other [150000,150030)) | pass |
| merged document byte-identical to applying both edits by hand | **199,977 bytes, memcmp exact** |
| the head version merged into is unchanged | pass |
| the other version is unchanged | pass |
| the common ancestor is unchanged | pass |
| overlapping edits are REFUSED (`rc = -2`) | pass |
| a conflicting merge appends NO version (3 == 3) | pass |
| merging into an unchanged head fast-forwards | 199,993 bytes exact |
| merging head into itself succeeds, `identical == 1`, idempotent | pass |
| merged store saves, reopens, byte-exact | **199,929 bytes** |

The expected document is built by splicing the base array directly in the
test, independent of Genna, and compared with `memcmp` — not by re-deriving it
through the same engine that produced it.

## What is NOT handled

- **No branch topology.** Genna stores versions, not a DAG of branches, so the
  caller supplies the common ancestor. There is no `merge-base` computation.
- **Merging happens in memory, then commits.** `gn_merge` appends a version to
  the in-process object; combining it with `gn_commit`'s optimistic
  concurrency (reload → merge → commit) is left to the caller.
- **One span per side** (above).
- **Bytes, not structure.** Merging two versions of a *table* by rows, or two
  scenes by object, would need the semantics of those layers. This merges byte
  sequences.
- **No conflict markers.** A conflict is reported as two spans; it does not
  produce a `<<<<<<<`-annotated document.
