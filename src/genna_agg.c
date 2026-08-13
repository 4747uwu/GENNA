/* genna_agg.c — the monoids, and the object-level range query.
 *
 * genna_ext.c owns the annotation slot and the O(log n) descent. This file is
 * everything a caller actually wants on top of it: three ready-made monoids,
 * the store hookup, and a query that takes an object and a version instead of
 * a raw tree node.
 *
 * It is a separate translation unit so the core stays as it was -- the treap
 * gained one field and two lines, and none of the policy lives there.
 */
#include "../include/genna.h"
#include "../include/genna_agg.h"
#include <string.h>

/* ---- resolving an extent to its tokens -------------------------------- */

static const gn_tok *store_resolve(gn_cid chunk, uint32_t *n_out, void *ud) {
    size_t n = 0;
    const gn_tok *t = gn_store_get((const gn_store *)ud, chunk, &n);
    *n_out = (uint32_t)n;
    return t;
}

/* ---- the monoids ------------------------------------------------------
 * Tokens, not bytes. For a binary object a token IS a byte (GN_BYTE_BASE + b)
 * so these read naturally; for dictionary-tokenized text a token is a
 * dictionary id, and min/max/sum over ids is well-defined but means something
 * different. gn_agg_attach takes the mode explicitly rather than guessing.  */

static uint64_t leaf_min(const gn_tok *t, uint32_t n, void *ud) {
    (void)ud;
    uint64_t m = (uint64_t)-1;
    for (uint32_t i = 0; i < n; i++) if (t[i] < m) m = t[i];
    return m;
}
static uint64_t comb_min(uint64_t a, uint64_t b, void *ud) {
    (void)ud; return a < b ? a : b;
}

static uint64_t leaf_max(const gn_tok *t, uint32_t n, void *ud) {
    (void)ud;
    uint64_t m = 0;
    for (uint32_t i = 0; i < n; i++) if (t[i] > m) m = t[i];
    return m;
}
static uint64_t comb_max(uint64_t a, uint64_t b, void *ud) {
    (void)ud; return a > b ? a : b;
}

static uint64_t leaf_sum(const gn_tok *t, uint32_t n, void *ud) {
    (void)ud;
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += t[i];
    return s;
}
static uint64_t comb_sum(uint64_t a, uint64_t b, void *ud) {
    (void)ud; return a + b;
}

int gn_agg_attach(gn_engine *e, gn_agg_kind kind) {
    if (!e) return -1;
    if (kind == GN_AGG_NONE) { gn_ext_set_monoid(NULL); return 0; }

    gn_monoid m;
    memset(&m, 0, sizeof m);
    m.resolve = store_resolve;
    m.ud      = gn_engine_store(e);
    switch (kind) {
        case GN_AGG_MIN: m.identity = (uint64_t)-1;
                         m.leaf = leaf_min; m.combine = comb_min; break;
        case GN_AGG_MAX: m.identity = 0;
                         m.leaf = leaf_max; m.combine = comb_max; break;
        case GN_AGG_SUM: m.identity = 0;
                         m.leaf = leaf_sum; m.combine = comb_sum; break;
        default: return -1;
    }
    gn_ext_set_monoid(&m);
    return gn_ext_monoid_set() ? 0 : -1;
}

/* ---- the query -------------------------------------------------------- */

uint64_t gn_range_agg(gn_engine *e, gn_object *o, uint32_t version,
                      uint64_t off, uint64_t len) {
    (void)e;
    if (!o || version >= o->n_ver) return 0;
    return gn_ext_range_agg(o->ver[version].root, off, len);
}

uint64_t gn_range_agg_latest(gn_engine *e, gn_object *o,
                             uint64_t off, uint64_t len) {
    if (!o || o->n_ver == 0) return 0;
    return gn_range_agg(e, o, o->n_ver - 1, off, len);
}
