/* genna_merge.h — three-way merge of two divergent versions.
 *
 * Genna's persistence layer detects concurrent writers (gn_commit returns
 * GN_CONFLICT) but has never been able to RESOLVE them: a writer whose base
 * moved had to reload and redo its work. This adds the resolution for the
 * case where it is well defined.
 *
 *   base ── head        (what you have now; must be the object's latest)
 *      └─── other       (what someone else did from the same ancestor)
 *
 *   gn_merge(e, o, base_v, other_v, &info);
 *
 * On success a NEW version is appended containing both sides' changes, and
 * the structural sharing is preserved -- the merge is applied as one splice
 * on top of `head`, not by rewriting the object.
 *
 * WHAT IT WILL NOT DO
 *
 * It refuses rather than guesses. If both sides changed overlapping regions
 * there is no merge that is right without knowing what the bytes MEAN, so
 * gn_merge returns GN_MERGE_CONFLICT, appends nothing, and reports both
 * spans. Silently preferring one side is how merge tools lose data.
 *
 * PRECISION
 *
 * Each side's change is reduced to ONE contiguous span -- the region between
 * its longest common prefix and longest common suffix with the base. A side
 * that made several scattered edits therefore presents as one span covering
 * all of them, so two such sides can be reported as conflicting where a
 * finer-grained diff would have merged them. That is conservative in the safe
 * direction: this never merges something it should have refused, but it does
 * refuse some things a smarter differ would take. See MERGE.md.
 */
#ifndef GENNA_MERGE_H
#define GENNA_MERGE_H

#include "genna.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GN_MERGE_OK        0
#define GN_MERGE_CONFLICT (-2)   /* both sides changed overlapping bytes */
#define GN_MERGE_EINVAL   (-1)

typedef struct {
    /* Each side's changed span, in BASE byte coordinates. lo == hi means the
     * side changed nothing. */
    uint64_t head_lo, head_hi;
    uint64_t other_lo, other_hi;
    /* Replacement lengths, so a caller can see how much each side inserted. */
    uint64_t head_len, other_len;
    int      conflict;          /* 1 if the spans overlap */
    /* 1 if both sides made byte-identical changes to the same span. That is
     * convergence, not a clash, so it merges to a no-op instead of
     * conflicting -- without it, merging a version into itself would be
     * refused, since identical spans overlap by definition. */
    int      identical;
    uint32_t merged_version;    /* valid only on GN_MERGE_OK */
} gn_merge_info;

/* Merge `other_v` into the object's LATEST version, given their common
 * ancestor `base_v`. Requires base_v and other_v to be versions of `o`.
 *
 * Returns GN_MERGE_OK (a new version was appended), GN_MERGE_CONFLICT
 * (nothing appended; `info` describes both spans), or GN_MERGE_EINVAL.
 *
 * `info` may be NULL. */
int gn_merge(gn_engine *e, gn_object *o, uint32_t base_v, uint32_t other_v,
             gn_merge_info *info);

/* The span analysis on its own, without merging. Useful for showing a caller
 * why a merge would conflict before attempting it. */
int gn_merge_preview(gn_engine *e, gn_object *o, uint32_t base_v,
                     uint32_t other_v, gn_merge_info *info);

#ifdef __cplusplus
}
#endif
#endif /* GENNA_MERGE_H */
