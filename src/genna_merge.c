/* genna_merge.c — three-way merge.
 *
 * A layer over the engine, like persistence: it reads versions through the
 * public API and applies its result with one gn_update. The treap, the chunk
 * store and the version DAG are untouched.
 */
#include "../include/genna.h"
#include "../include/genna_merge.h"
#include <stdlib.h>
#include <string.h>

/* Longest common prefix, then longest common suffix that does not overlap it.
 *
 * The suffix scan is capped so the two never cross: for base "aaaa" and side
 * "aa", an uncapped suffix of 2 plus a prefix of 2 would claim 4 bytes of a
 * 2-byte string and produce a negative-length span. */
static void span_of(const uint8_t *b, uint64_t bn,
                    const uint8_t *s, uint64_t sn,
                    uint64_t *lo, uint64_t *hi, uint64_t *rep_len) {
    uint64_t p = 0, max_p = bn < sn ? bn : sn;
    while (p < max_p && b[p] == s[p]) p++;

    uint64_t suf = 0, max_s = (bn < sn ? bn : sn) - p;
    while (suf < max_s && b[bn - 1 - suf] == s[sn - 1 - suf]) suf++;

    *lo = p;
    *hi = bn - suf;               /* base coordinates */
    *rep_len = sn - suf - p;      /* what the side put there */
}

static uint8_t *read_ver(gn_engine *e, gn_object *o, uint32_t v, uint64_t *n) {
    if (v >= o->n_ver) return NULL;
    uint64_t len = o->ver[v].total_bytes;
    uint8_t *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    size_t got = len ? gn_read_version(e, o, v, 0, (size_t)len, buf) : 0;
    if ((uint64_t)got != len) { free(buf); return NULL; }
    *n = len;
    return buf;
}

static int analyse(gn_engine *e, gn_object *o, uint32_t base_v, uint32_t other_v,
                   gn_merge_info *info,
                   uint8_t **other_buf_out, uint64_t *other_n_out) {
    if (!e || !o || o->n_ver == 0) return GN_MERGE_EINVAL;
    uint32_t head_v = o->n_ver - 1;
    if (base_v >= o->n_ver || other_v >= o->n_ver) return GN_MERGE_EINVAL;

    uint64_t bn = 0, hn = 0, on = 0;
    uint8_t *b = read_ver(e, o, base_v,  &bn);
    uint8_t *h = read_ver(e, o, head_v,  &hn);
    uint8_t *ot = read_ver(e, o, other_v, &on);
    if (!b || !h || !ot) { free(b); free(h); free(ot); return GN_MERGE_EINVAL; }

    uint64_t hlo, hhi, hrep, olo, ohi, orep;
    span_of(b, bn, h,  hn, &hlo, &hhi, &hrep);
    span_of(b, bn, ot, on, &olo, &ohi, &orep);

    /* Overlap in BASE coordinates. Touching-but-not-overlapping (hhi == olo)
     * is fine: the two sides changed adjacent, disjoint regions. An empty
     * span (a side that changed nothing) never conflicts. */
    int h_empty = (hlo == hhi && hrep == 0);
    int o_empty = (olo == ohi && orep == 0);
    int overlap = !h_empty && !o_empty && (hlo < ohi) && (olo < hhi);

    /* Both sides making the SAME change is not a clash, it is convergence --
     * two writers who independently applied the same patch, or a merge of a
     * version into itself. Identical spans overlap by definition, so without
     * this the safe answer would be a conflict on a merge that has nothing to
     * decide. Compared by content, not by version identity, so it also covers
     * two separately-authored identical edits. */
    int same = overlap && hlo == olo && hhi == ohi && hrep == orep &&
               (hrep == 0 || memcmp(h + hlo, ot + olo, (size_t)hrep) == 0);
    if (same) { overlap = 0; o_empty = 1; olo = ohi = 0; orep = 0; }

    if (info) {
        info->head_lo = hlo;  info->head_hi = hhi;  info->head_len = hrep;
        info->other_lo = olo; info->other_hi = ohi; info->other_len = orep;
        info->conflict = overlap;
        info->identical = same;
        info->merged_version = 0;
    }
    (void)o_empty;
    free(b); free(h);
    if (other_buf_out) { *other_buf_out = ot; *other_n_out = on; }
    else free(ot);
    return overlap ? GN_MERGE_CONFLICT : GN_MERGE_OK;
}

int gn_merge_preview(gn_engine *e, gn_object *o, uint32_t base_v,
                     uint32_t other_v, gn_merge_info *info) {
    return analyse(e, o, base_v, other_v, info, NULL, NULL);
}

int gn_merge(gn_engine *e, gn_object *o, uint32_t base_v, uint32_t other_v,
             gn_merge_info *info) {
    gn_merge_info local;
    if (!info) info = &local;

    uint8_t *ot = NULL; uint64_t on = 0;
    int rc = analyse(e, o, base_v, other_v, info, &ot, &on);
    if (rc != GN_MERGE_OK) { free(ot); return rc; }

    /* Nothing to bring over. */
    if (info->other_lo == info->other_hi && info->other_len == 0) {
        free(ot);
        info->merged_version = o->n_ver - 1;
        return GN_MERGE_OK;
    }

    /* Map the other side's span from BASE coordinates into HEAD coordinates.
     * Head's own edit changed the length of the region it touched, so
     * anything after that region shifts by the difference. The spans are
     * known disjoint here, so it is one comparison, not an interval map. */
    int64_t shift = 0;
    if (info->other_lo >= info->head_hi)
        shift = (int64_t)info->head_len - (int64_t)(info->head_hi - info->head_lo);

    uint64_t at  = (uint64_t)((int64_t)info->other_lo + shift);
    uint64_t del = info->other_hi - info->other_lo;

    /* The bytes the other side put in its span. */
    const uint8_t *ins = ot + info->other_lo;

    int urc = gn_update(e, o, at, del, ins, (size_t)info->other_len);
    free(ot);
    if (urc != 0) return GN_MERGE_EINVAL;

    info->merged_version = o->n_ver - 1;
    return GN_MERGE_OK;
}
