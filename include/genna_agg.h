/* genna_agg.h — aggregates in the node.
 *
 * The treap already carries subtree token and byte counts, which is exactly
 * why a range read can prune instead of walking every leaf. Those counts are
 * the sum monoid over leaf sizes. This makes the slot general: register an
 * associative operation and every node carries its subtree's value, so an
 * aggregate over a byte range costs O(log n) instead of O(range).
 *
 *   gn_agg_attach(e, GN_AGG_MAX);
 *   uint64_t hi = gn_range_agg_latest(e, o, 1u<<20, 4096);
 *
 * Cost, stated up front (measured in AGGREGATES.md):
 *   * 8 bytes on every node, when built with -DGN_NODE_AGG. Without that
 *     define this header still compiles and the calls are no-ops.
 *   * Once a monoid is registered, every leaf construction computes a leaf
 *     aggregate, which READS CHUNK TOKENS. On a memory-mapped store that
 *     faults pages in, which is in direct tension with out-of-core
 *     (genna_persist.h, GN_SAVE_MAPPABLE). Attaching a monoid to a mapped
 *     store gives up most of the mapping's benefit. That is a real conflict,
 *     not a footnote.
 *
 * The monoid is per-process, not per-object: nodes are SHARED between objects
 * and versions, so two objects cannot disagree about what their shared
 * annotations mean.
 */
#ifndef GENNA_AGG_H
#define GENNA_AGG_H

#include "genna.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GN_AGG_NONE = 0,   /* clear; nodes stop computing anything     */
    GN_AGG_MIN,        /* smallest token value in the range        */
    GN_AGG_MAX,        /* largest token value in the range         */
    GN_AGG_SUM         /* sum of token values in the range         */
} gn_agg_kind;

/* Register one of the built-in monoids on `e`'s store. Trees built BEFORE
 * this call carry annotations computed under the previous setting, so attach
 * before ingesting. Returns 0 on success. */
int gn_agg_attach(gn_engine *e, gn_agg_kind kind);

/* Aggregate over [off, off+len) bytes of a version, in O(log n).
 *
 * IT AGGREGATES TOKEN VALUES, NOT BYTES. For an object ingested with
 * gn_create_binary a token is a byte escape (GN_BYTE_BASE + b), so MIN/MAX
 * order the same way bytes do and SUM is a fixed offset per byte. For
 * dictionary-tokenized text a token is a dictionary id, and these numbers are
 * about ids, not characters.
 *
 * The two can MIX inside one object: gn_update goes through the tokenizer, so
 * editing a binary object can leave dictionary ids in the edited region while
 * the rest stays byte escapes. The aggregate is still exactly the monoid over
 * the tokens present -- but a caller modelling it as "the largest byte in the
 * range" will be wrong there. Use a binary-only ingest/edit path if you need
 * byte semantics.
 *
 * Byte offsets against a token tree are exact where bytes and tokens agree.
 * Where they do not, a partial leaf at either end is rounded OUTWARD to whole
 * tokens, so the answer covers at least the range asked for and never less.
 * For MIN/MAX that is a safe bound (a superset can only widen the extremes,
 * which is what predicate pushdown wants). For SUM it OVER-COUNTS at the two
 * boundary leaves; only trust SUM on ranges that align to token boundaries,
 * which for binary objects is every range. */
uint64_t gn_range_agg(gn_engine *e, gn_object *o, uint32_t version,
                      uint64_t off, uint64_t len);
uint64_t gn_range_agg_latest(gn_engine *e, gn_object *o,
                             uint64_t off, uint64_t len);

#ifdef __cplusplus
}
#endif
#endif /* GENNA_AGG_H */
