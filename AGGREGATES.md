# Aggregates in the node

The treap already carries two aggregates on every node — subtree token count
and subtree byte count — and that is exactly why a range read can prune
instead of visiting every leaf. Those counts are the *sum monoid over leaf
sizes*. Nothing about the structure is specific to counting.

This generalises the slot. Register an associative operation and every node
carries its subtree's value, so an aggregate over a byte range costs
**O(log n)** instead of O(range).

```c
#include "genna_agg.h"

gn_agg_attach(e, GN_AGG_MAX);
gn_object *o = gn_create_binary(e, "d", data, n, &opts);

uint64_t hi = gn_range_agg_latest(e, o, 1u << 20, 4096);   /* O(log n) */
```

Built with `-DGN_NODE_AGG`. Without it the header still compiles, the calls
are no-ops, and the node is unchanged — so the feature can be A/B'd rather
than argued about.

---

## What it buys

A subtree lying entirely inside the queried range answers from its own
annotation in O(1), so only the O(log n) boundary spine is descended.
Measured on an 8 MB binary object (`tools/run_agg.sh`):

| query | time |
|---|---|
| MAX over 4 KB | 0.0021 ms |
| MAX over the whole 8 MB | **0.0000 ms** |
| (for scale) `gn_read` of 4 MB | 17.0 ms |

The whole-object aggregate is not slower than the 4 KB one — it is faster,
because it terminates at the root instead of descending to a boundary. That is
the shape the annotation is supposed to produce.

## What it costs

**1. Node width: 56 → 64 bytes (+14%).** Reported by `gn_ext_node_size()`,
not hardcoded — an earlier bug in this repo came from a test asserting 24
when the node was 56.

**2. It fights out-of-core, badly.** Once a monoid is registered, every leaf
computes its aggregate at construction, and that *reads the leaf's chunk
tokens*. On a memory-mapped store (`GN_SAVE_MAPPABLE`), reading is faulting.
Same 32 MB store, one fresh process each (`tools/run_oocore.sh`):

| | resident after open | open time |
|---|---|---|
| no monoid | **2.9 MB** | 3 ms |
| `GN_AGG_MAX` attached | **130.0 MB** | 69 ms |

**44.9× more resident, 23× slower** — 130 MB being essentially the entire
128 MB of chunk data pulled in. Attaching a monoid to a mapped store gives up
almost all of what mapping was for. The two features are usable together only
if you actually want the whole store resident.

That cost is the leaf computation, not the node width. Widening nodes by 8
bytes costs ~14% of the tree; computing leaf aggregates costs the entire
payload.

---

## Semantics, including the sharp edges

**It aggregates TOKEN values, not bytes.** For `gn_create_binary` a token is a
byte escape (`GN_BYTE_BASE + b`), so MIN/MAX order exactly as bytes do. For
dictionary-tokenized text a token is a dictionary id, and the numbers are
about ids.

The two can mix inside one object: `gn_update` goes through the tokenizer, so
editing a binary object can leave dictionary ids in the edited region while
the rest stays byte escapes. The aggregate is still exactly the monoid over
the tokens present — but a caller reading it as "the largest byte in this
range" will be wrong there.

This is not hypothetical. The first version of `agg_test` compared post-edit
aggregates against a `GN_BYTE_BASE + byte` model and reported the engine as
broken; the engine was right and the test's model was wrong. The test now
checks against the object's real tokens.

**Partial leaves round outward.** A byte range that ends mid-token is widened
to whole tokens, so the answer covers at least what was asked. For MIN/MAX
that is a safe bound — a superset can only widen the extremes, which is what
predicate pushdown wants. **For SUM it over-counts** at the two boundary
leaves; trust SUM only on token-aligned ranges, which for binary objects is
every range.

**The monoid is per-process, not per-object.** Nodes are shared between
objects and versions, so a per-object monoid would let one object read
annotations another object computed under different rules.

**It binds to a store.** `gn_agg_attach(e, kind)` resolves chunks through
`e`'s store. Opening a *different* engine leaves the monoid pointing at the
old one. `gn_open` calls `gn_ext_monoid_bind()` internally so loaded trees
annotate against the engine being loaded — but if you keep two engines alive
and query both, re-attach before querying the second.

---

## What is proven, and by which test

`tools/run_agg.sh` → `tests/agg_test.c`, 8 MB of incompressible data with
extremes planted at 1/3, 1/2 and the last byte so a monoid that silently
returns identity cannot pass:

| check | result |
|---|---|
| 200 random MAX ranges vs brute-force scan | exact |
| planted `0xFF` found by a 1-byte range | pass |
| whole-object MAX is the planted `0xFF` | pass |
| whole-object aggregate ≈ cost of a 4 KB one | pass |
| 100 random MIN ranges vs brute force | exact |
| 100 random SUM ranges vs brute force | exact |
| whole-object SUM vs brute force | exact |
| after an edit, whole-object aggregate == sum of real tokens | exact (diff 0) |
| version 0 unchanged by a later edit (structural sharing) | pass |
| **after `gn_save` + `gn_open`, aggregate == sum of real tokens** | pass |
| **and equals the value held before saving** | pass |

That last pair is there because of a bug this document would otherwise have
been wrong about: the snapshot loader builds leaves through
`gn_ext_mk_leaf_p`, not `mk_leaf`, and that path did not compute the
annotation. Every reopened store had **uninitialized** aggregates — plausible
numbers, silently wrong — and nothing in the suite reopened a store with a
monoid attached, so nothing noticed. Both the leaf constructor and the
round-trip test were added together.

## What is NOT handled

- **Aggregates are not persisted.** They are recomputed at load, which is what
  makes them expensive to open (above). Storing them would need a monoid
  identity in the header so a store annotated under MAX cannot be read as SUM.
- **No predicate pushdown API.** MIN/MAX per subtree is the ingredient for
  "skip this range, nothing in it can match"; the skipping itself is not
  written.
- **Only `uint64_t` aggregates.** Enough for min/max/sum/count and for a
  packed small sketch, not for an HLL register array or an automaton
  transition vector. Those need a variable-width slot and an allocator, which
  is a different design.
- **No float semantics.** Tokens are `uint32_t`; summing them is exact. A
  float column would need the leaf to know a dtype, and the columnar layer
  stores columns as Arrow IPC streams (framing bytes, not raw values), so a
  byte range there does not map to numeric values at all.
