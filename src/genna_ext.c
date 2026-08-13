/* genna_ext.c — the extent tree.
 *
 * Replaces the flat gn_extent[] that gn_update copied wholesale on every
 * version.  That copy made edits O(n_ext) = O(file/chunk) and made version
 * history cost O(n_ext) per version: 200 small edits on a 128MB file
 * produced 97MB of version metadata.
 *
 * This is a persistent (immutable, path-copying) treap whose leaves are
 * extents.  split() and join() allocate O(log n) new nodes and SHARE every
 * untouched subtree with the previous version.  So:
 *   - an edit touches O(log n) nodes, not O(n_ext)
 *   - a new version costs O(log n) bytes, not O(n_ext) bytes
 *   - old versions stay valid for free, because nothing is ever mutated
 *
 * Nodes are arena-allocated and never freed: with structural sharing you
 * cannot free a node without refcounting the whole DAG, and for measuring
 * the architecture the arena is the honest simplification.  Real GC is
 * refcount-on-node, decrement-on-version-drop.
 */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct gn_enode {
    gn_enode *l, *r;        /* both NULL => leaf                          */
    gn_extent ext;          /* meaningful only on a leaf                  */
    uint64_t  toks, bytes;  /* subtree totals                             */
#ifdef GN_NODE_AGG
    uint64_t  agg;          /* monoid over this subtree; valid iff AGG_OK */
#endif
    uint32_t  prio;         /* treap priority                             */
    uint32_t  rc;           /* parents pointing here; 0 => reclaimable    */
};

#ifdef GN_NODE_AGG
/* Aggregates are computed eagerly at node construction, and only when a
 * monoid has been registered. With none registered -- the default -- nothing
 * is computed and nothing is read, so the cost of this feature is exactly the
 * 8 bytes of node width and no time at all.
 *
 * Eager rather than lazy on purpose: a "computed yet?" flag needs a field,
 * and with padding that is another 8 bytes on top of the 8 for the value.
 * Node width is the entire cost here (it is what a mapped store keeps
 * resident), so paying it twice to avoid work the caller opted into is a bad
 * trade. The consequence is real and is measured in AGGREGATES.md: with a
 * monoid registered, LOADING computes a leaf aggregate per leaf, which reads
 * chunk tokens -- and on a mapped store that faults in pages. This is the
 * conflict between aggregates and out-of-core, stated as a number rather
 * than as a caveat. */
static gn_monoid g_mon;
static int       g_mon_set;

void gn_ext_set_monoid(const gn_monoid *m) {
    if (m) { g_mon = *m; g_mon_set = 1; }
    else   { memset(&g_mon, 0, sizeof g_mon); g_mon_set = 0; }
}
int gn_ext_monoid_set(void) { return g_mon_set; }

/* Repoint the monoid's resolve context without changing the operation.
 *
 * The snapshot loader builds every leaf INSIDE gn_open, before the caller can
 * see the new engine -- so a monoid attached to some earlier engine would
 * resolve every chunk against a store that no longer owns them (or has been
 * freed). gn_open calls this with the engine it is building. */
void gn_ext_monoid_bind(void *ud) { if (g_mon_set) g_mon.ud = ud; }
#else
void gn_ext_set_monoid(const gn_monoid *m) { (void)m; }
int  gn_ext_monoid_set(void) { return 0; }
void gn_ext_monoid_bind(void *ud) { (void)ud; }
uint64_t gn_ext_range_agg(const gn_enode *n, uint64_t off, uint64_t len) {
    (void)n; (void)off; (void)len; return 0;
}
#endif

/* Reference counting over the sharing DAG.
 * Path copying means one node can have many parents (that is the whole
 * point -- an untouched subtree is reused by every version that did not
 * change it). So a node is reclaimable only when NO version and no other
 * node still points at it. Every structural link takes a reference; each
 * version root holds one; dropping a version releases exactly one, and the
 * release cascades only as far as counts actually reach zero. */

/* ---- arena ----------------------------------------------------------- */
#define ARENA_BLK 4096
typedef struct arena_blk { struct arena_blk *next; gn_enode n[ARENA_BLK]; } arena_blk;
static arena_blk *g_arena = NULL;
static uint32_t   g_arena_used = ARENA_BLK;
static uint64_t   g_nodes_alloced = 0;   /* currently live               */
static uint64_t   g_nodes_total   = 0;   /* ever allocated               */
static uint64_t   g_nodes_freed   = 0;   /* reclaimed by refcounting     */
static gn_enode  *g_free = NULL;         /* free list, reuses arena cells */

static gn_enode *node_alloc(void) {
    gn_enode *n;
    if (g_free) { n = g_free; g_free = n->l; }
    else {
        if (g_arena_used == ARENA_BLK) {
            arena_blk *b = malloc(sizeof *b);
            b->next = g_arena; g_arena = b; g_arena_used = 0;
        }
        n = &g_arena->n[g_arena_used++];
        g_nodes_total++;
    }
    g_nodes_alloced++;
    n->rc = 0;
    return n;
}

/* take a reference (called on every structural link) */
gn_enode *gn_ext_retain(gn_enode *n) { if (n) n->rc++; return n; }

/* drop a reference; cascade only where counts actually hit zero */
void gn_ext_release(gn_enode *n) {
    while (n) {
        if (--n->rc > 0) return;
        gn_enode *l = n->l, *r = n->r;
        n->l = g_free; g_free = n;          /* recycle the cell           */
        g_nodes_alloced--; g_nodes_freed++;
        if (l) { if (r) gn_ext_release(r); n = l; }   /* tail-loop on left */
        else n = r;
    }
}

uint64_t gn_ext_nodes_total(void) { return g_nodes_total; }
uint64_t gn_ext_nodes_freed(void) { return g_nodes_freed; }

uint64_t gn_ext_nodes_alloced(void) { return g_nodes_alloced; }
uint64_t gn_ext_node_bytes(void)    { return g_nodes_alloced * sizeof(gn_enode); }

/* Bytes one node actually occupies.
 *
 * Several benchmarks used to hardcode 24 here. On x86-64 the struct is 56
 * bytes (two pointers, a gn_extent, two u64 totals, prio and rc), so that
 * understated Genna's own bytes-written by 2.33x and inflated every ratio
 * derived from it by the same factor. Ask, do not assume. */
uint64_t gn_ext_node_size(void)     { return sizeof(gn_enode); }
uint64_t gn_ext_arena_bytes(void)   { return g_nodes_total * sizeof(gn_enode); }

void gn_ext_arena_free(void) {
    while (g_arena) { arena_blk *n = g_arena->next; free(g_arena); g_arena = n; }
    g_arena_used = ARENA_BLK; g_nodes_alloced = 0; g_nodes_total = 0;
    g_nodes_freed = 0; g_free = NULL;
}

/* ---- xorshift for treap priorities ----------------------------------- */
static uint32_t g_rng = 2463534242u;
static uint32_t xrnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

/* ---- accessors ------------------------------------------------------- */
uint64_t gn_ext_tokens(const gn_enode *n) { return n ? n->toks  : 0; }
uint64_t gn_ext_bytes (const gn_enode *n) { return n ? n->bytes : 0; }
static inline bool is_leaf(const gn_enode *n) { return n && !n->l && !n->r; }

#ifdef GN_NODE_AGG
/* Aggregate of one extent, from the tokens it names. */
static uint64_t leaf_agg(const gn_extent *x) {
    if (!g_mon_set || !g_mon.leaf || !g_mon.resolve) return 0;
    uint32_t n = 0;
    const gn_tok *t = g_mon.resolve(x->chunk, &n, g_mon.ud);
    if (!t || (uint64_t)x->off + x->len > (uint64_t)n) return g_mon.identity;
    return g_mon.leaf(t + x->off, x->len, g_mon.ud);
}
static inline uint64_t agg_of(const gn_enode *n) {
    return n ? n->agg : (g_mon_set ? g_mon.identity : 0);
}
static inline uint64_t agg_join(uint64_t a, uint64_t b) {
    return (g_mon_set && g_mon.combine) ? g_mon.combine(a, b, g_mon.ud) : 0;
}
#endif

static gn_enode *mk_leaf(gn_extent e, uint64_t bytes) {
    gn_enode *n = node_alloc();
    n->l = n->r = NULL; n->ext = e;
    n->toks = e.len; n->bytes = bytes;
    n->prio = xrnd();
#ifdef GN_NODE_AGG
    n->agg = leaf_agg(&e);
#endif
    return n;
}

/* internal node: recompute totals from children (path copy) */
static gn_enode *mk_inner(gn_enode *l, gn_enode *r, uint32_t prio) {
    gn_enode *n = node_alloc();
    n->l = gn_ext_retain(l); n->r = gn_ext_retain(r);
    memset(&n->ext, 0, sizeof n->ext);
    n->toks  = gn_ext_tokens(l) + gn_ext_tokens(r);
    n->bytes = gn_ext_bytes(l)  + gn_ext_bytes(r);
    n->prio  = prio;
#ifdef GN_NODE_AGG
    n->agg   = agg_join(agg_of(l), agg_of(r));
#endif
    return n;
}

/* ---- join: concatenate two sequences ---------------------------------
 * Standard treap join by priority.  Allocates only along the spine.      */
/* Ownership convention: the public split/join/build hand back roots with a
 * reference ALREADY TAKEN for the caller. The caller either stores the tree
 * in a version (which keeps that reference) or calls gn_ext_release() on it.
 * Recursion uses the non-retaining *_core so an intermediate is not counted
 * twice: mk_inner takes its own reference on each child it links. */
static gn_enode *join_core(gn_enode *a, gn_enode *b) {
    if (!a) return b;
    if (!b) return a;
    if (a->prio >= b->prio) {
        if (is_leaf(a)) return mk_inner(a, b, a->prio);
        return mk_inner(a->l, join_core(a->r, b), a->prio);
    } else {
        if (is_leaf(b)) return mk_inner(a, b, b->prio);
        gn_enode *sub = join_core(a, b->l);
        return mk_inner(sub, b->r, b->prio);
    }
}

gn_enode *gn_ext_join(gn_enode *a, gn_enode *b) {
    return gn_ext_retain(join_core(a, b));
}

/* ---- split by TOKEN offset -------------------------------------------
 * Returns *lo = first k tokens, *hi = the rest.  A leaf whose interior the
 * split falls inside is divided into two extents over the same chunk --
 * no chunk is copied, only the (chunk, off, len) triple.
 * split_bytes() is needed to give each half its byte total; the caller
 * supplies it because only the engine knows the dictionary.              */
typedef uint64_t (*ext_bytes_fn)(void *ctx, const gn_extent *x);

static void split_core(gn_enode *n, uint64_t k,
                       gn_enode **lo, gn_enode **hi,
                       ext_bytes_fn bytes_of, void *ctx) {
    if (!n) { *lo = *hi = NULL; return; }
    if (k == 0)        { *lo = NULL; *hi = n; return; }
    if (k >= n->toks)  { *lo = n;    *hi = NULL; return; }

    if (is_leaf(n)) {
        gn_extent a = n->ext, b = n->ext;
        a.len = (uint32_t)k;
        b.off = n->ext.off + (uint32_t)k;
        b.len = n->ext.len - (uint32_t)k;
        uint64_t ab = bytes_of(ctx, &a);
        uint64_t bb = n->bytes - ab;      /* exact: halves partition bytes */
        *lo = mk_leaf(a, ab);
        *hi = mk_leaf(b, bb);
        return;
    }
    uint64_t lt = gn_ext_tokens(n->l);
    if (k <= lt) {
        gn_enode *a, *b;
        split_core(n->l, k, &a, &b, bytes_of, ctx);
        *lo = a;
        /* LEAK FIX: test b BEFORE allocating. The old order built a node
         * (which retained n->r) and then threw it away when b was NULL,
         * stranding both the node and the reference it had taken. */
        *hi = b ? mk_inner(b, n->r, n->prio) : n->r;
    } else {
        gn_enode *a, *b;
        split_core(n->r, k - lt, &a, &b, bytes_of, ctx);
        *lo = a ? mk_inner(n->l, a, n->prio) : n->l;
        *hi = b;
    }
}

/* ---- build a balanced tree from an extent array (gn_create path) ------ */
static gn_enode *build_core(const gn_extent *ext, const uint64_t *bytes, uint32_t n) {
    if (n == 0) return NULL;
    if (n == 1) return mk_leaf(ext[0], bytes[0]);
    uint32_t m = n / 2;
    gn_enode *l = build_core(ext, bytes, m);
    gn_enode *r = build_core(ext + m, bytes + m, n - m);
    /* priority must dominate children for treap invariants to hold on
       later joins; take max(child)+1 so the built tree is a valid treap. */
    uint32_t p = (l->prio > r->prio ? l->prio : r->prio);
    if (p != 0xFFFFFFFFu) p++;
    return mk_inner(l, r, p);
}

gn_enode *gn_ext_build(const gn_extent *ext, const uint64_t *bytes, uint32_t n) {
    return gn_ext_retain(build_core(ext, bytes, n));
}

/* ---- locate by BYTE offset -------------------------------------------
 * Descend to the leaf containing byte `off`; report the leaf's extent,
 * the token offset of the leaf's start, and bytes consumed before it.
 * O(log n) instead of the old O(n_ext) linear extent walk.               */
bool gn_ext_locate_byte(const gn_enode *n, uint64_t off,
                        gn_extent *ext_out, uint64_t *tok_before,
                        uint64_t *bytes_before) {
    uint64_t tb = 0, bb = 0;
    while (n) {
        if (is_leaf(n)) {
            *ext_out = n->ext; *tok_before = tb; *bytes_before = bb;
            return true;
        }
        uint64_t lb = gn_ext_bytes(n->l);
        if (off < bb + lb) { n = n->l; }
        else { bb += lb; tb += gn_ext_tokens(n->l); n = n->r; }
    }
    return false;
}

/* ---- in-order leaf iteration (read / search paths) -------------------- */
void gn_ext_walk(const gn_enode *n,
                 void (*fn)(void *ctx, const gn_extent *x, uint64_t bytes),
                 void *ctx) {
    if (!n) return;
    if (is_leaf(n)) { fn(ctx, &n->ext, n->bytes); return; }
    gn_ext_walk(n->l, fn, ctx);
    gn_ext_walk(n->r, fn, ctx);
}

/* ---- range-limited walk ----------------------------------------------
 * gn_ext_walk visits EVERY leaf. The read path used it and skipped the leaves
 * it did not need, which made a 1 KB read cost O(total leaves): measured, a
 * fixed 1 KB read grew 3x as the object grew from 1 MB to 32 MB, and would
 * cost ~640,000 leaf visits on a 10 GB object.
 *
 * This prunes on the subtree byte totals the tree already maintains, so only
 * the O(log n) spine plus the covered leaves are visited. The callback also
 * receives the leaf's absolute start offset, so it no longer has to be handed
 * every preceding leaf just to keep a running position. */
static void walk_range_rec(const gn_enode *n, uint64_t start,
                           uint64_t lo, uint64_t hi,
                           void (*fn)(void *, const gn_extent *, uint64_t, uint64_t),
                           void *ctx) {
    if (!n) return;
    uint64_t nb = n->bytes;
    if (start >= hi || start + nb <= lo) return;      /* disjoint: prune */
    if (is_leaf(n)) { fn(ctx, &n->ext, nb, start); return; }
    walk_range_rec(n->l, start, lo, hi, fn, ctx);
    walk_range_rec(n->r, start + gn_ext_bytes(n->l), lo, hi, fn, ctx);
}

#ifdef GN_NODE_AGG
/* Aggregate over a byte range.
 *
 * The reason the value lives in the node rather than in a side index: a
 * subtree that falls ENTIRELY inside the range contributes n->agg in O(1),
 * so only the O(log n) boundary spine is ever descended. Partial leaves at
 * the two ends are recomputed from their tokens, which is the only place
 * chunk data is touched.
 *
 * Byte offsets against a token tree are exact only where bytes and tokens
 * agree; for a partial leaf the token sub-range is derived proportionally
 * and then clamped, so a caller asking for a mid-token boundary gets a
 * whole-token answer rather than a wrong one. */
static uint64_t range_agg_rec(const gn_enode *n, uint64_t start,
                              uint64_t lo, uint64_t hi) {
    if (!n) return g_mon.identity;
    uint64_t nb = n->bytes;
    if (start >= hi || start + nb <= lo) return g_mon.identity;   /* disjoint */
    if (start >= lo && start + nb <= hi) return n->agg;           /* covered  */
    if (is_leaf(n)) {
        /* Partial overlap of a leaf: recompute over the covered part. */
        if (nb == 0 || n->ext.len == 0) return g_mon.identity;
        uint64_t a = lo > start ? lo - start : 0;
        uint64_t b = hi < start + nb ? hi - start : nb;
        if (b <= a) return g_mon.identity;
        /* bytes -> tokens, rounded outward so the answer covers the ask */
        uint64_t t0 = a * n->ext.len / nb;
        uint64_t t1 = (b * n->ext.len + nb - 1) / nb;
        if (t1 > n->ext.len) t1 = n->ext.len;
        if (t1 <= t0) return g_mon.identity;
        gn_extent sub = n->ext;
        sub.off = n->ext.off + (uint32_t)t0;
        sub.len = (uint32_t)(t1 - t0);
        return leaf_agg(&sub);
    }
    uint64_t la = range_agg_rec(n->l, start, lo, hi);
    uint64_t ra = range_agg_rec(n->r, start + gn_ext_bytes(n->l), lo, hi);
    return agg_join(la, ra);
}

uint64_t gn_ext_range_agg(const gn_enode *n, uint64_t off, uint64_t len) {
    if (!g_mon_set) return 0;
    if (!n || len == 0) return g_mon.identity;
    uint64_t hi = off + len;
    if (hi < off) hi = (uint64_t)-1;
    return range_agg_rec(n, 0, off, hi);
}
#endif

void gn_ext_walk_range(const gn_enode *n, uint64_t off, uint64_t len,
                       void (*fn)(void *ctx, const gn_extent *x,
                                  uint64_t bytes, uint64_t start),
                       void *ctx) {
    if (!n || len == 0) return;
    uint64_t hi = off + len;
    if (hi < off) hi = (uint64_t)-1;                  /* overflow guard */
    walk_range_rec(n, 0, off, hi, fn, ctx);
}

uint32_t gn_ext_depth(const gn_enode *n) {
    if (!n) return 0;
    if (is_leaf(n)) return 1;
    uint32_t a = gn_ext_depth(n->l), b = gn_ext_depth(n->r);
    return 1 + (a > b ? a : b);
}

uint32_t gn_ext_leaves(const gn_enode *n) {
    if (!n) return 0;
    if (is_leaf(n)) return 1;
    return gn_ext_leaves(n->l) + gn_ext_leaves(n->r);
}

/* public split: both halves come back with a reference taken for the caller */
void gn_ext_split_tok(gn_enode *n, uint64_t k,
                      gn_enode **lo, gn_enode **hi,
                      ext_bytes_fn bytes_of, void *ctx) {
    split_core(n, k, lo, hi, bytes_of, ctx);
    gn_ext_retain(*lo); gn_ext_retain(*hi);
}

/* ---- persistence support ---------------------------------------------
 * Serializing the version DAG means walking nodes and rebuilding them, which
 * needs the node struct. Rather than export the struct (and let anyone mutate
 * a shared node), export a read-only view plus the two constructors.
 *
 * The constructors deliberately do NOT retain the node they return: the
 * loader takes its own reference on every node and drops them all in one
 * pass once the graph is wired, which is what makes the reloaded refcounts
 * come out identical to the ones the save-time graph had.               */
int       gn_ext_is_leaf(const gn_enode *n) { return is_leaf(n) ? 1 : 0; }
gn_enode *gn_ext_left (const gn_enode *n)   { return n ? n->l : NULL; }
gn_enode *gn_ext_right(const gn_enode *n)   { return n ? n->r : NULL; }
uint32_t  gn_ext_prio (const gn_enode *n)   { return n ? n->prio : 0; }

void gn_ext_leaf_info(const gn_enode *n, gn_extent *ext_out, uint64_t *bytes_out) {
    if (!n) { memset(ext_out, 0, sizeof *ext_out); *bytes_out = 0; return; }
    *ext_out = n->ext; *bytes_out = n->bytes;
}

gn_enode *gn_ext_mk_leaf_p(gn_extent e, uint64_t bytes, uint32_t prio) {
    gn_enode *n = node_alloc();
    n->l = n->r = NULL; n->ext = e;
    n->toks = e.len; n->bytes = bytes;
    n->prio = prio;
#ifdef GN_NODE_AGG
    /* The snapshot loader builds leaves through here rather than mk_leaf (it
     * must restore the saved priority). Omitting this left every loaded leaf
     * with an UNINITIALIZED agg -- node_alloc only clears rc -- so inner
     * nodes combined garbage and every aggregate over a reopened store was
     * silently wrong. agg_test now saves and reopens for exactly this. */
    n->agg = leaf_agg(&e);
#endif
    return n;
}

/* Totals are recomputed from the children rather than read from the file:
 * a corrupt or hand-edited total can therefore never make the tree lie about
 * its own size, and the leaf byte counts are the only thing trusted.      */
gn_enode *gn_ext_mk_inner_p(gn_enode *l, gn_enode *r, uint32_t prio) {
    return mk_inner(l, r, prio);
}

/* ---- refcount audit --------------------------------------------------
 * Recompute, from scratch, how many references SHOULD point at every node
 * reachable from the given version roots: one per parent link, plus one per
 * root. Compare against the stored rc. Any mismatch is a leak (rc too high)
 * or a premature-free waiting to happen (rc too low). */
typedef struct anode { gn_enode *n; uint32_t seen; struct anode *next; } anode;
#define AUD_BUCKETS 65536
static anode *aud_tab[AUD_BUCKETS];

static anode *aud_find(gn_enode *n, int create) {
    uintptr_t h = ((uintptr_t)n >> 4) & (AUD_BUCKETS - 1);
    for (anode *a = aud_tab[h]; a; a = a->next) if (a->n == n) return a;
    if (!create) return NULL;
    anode *a = malloc(sizeof *a);
    a->n = n; a->seen = 0; a->next = aud_tab[h]; aud_tab[h] = a;
    return a;
}
static void aud_count(gn_enode *n) {
    if (!n) return;
    anode *a = aud_find(n, 1);
    if (a->seen++ > 0) return;          /* already descended */
    aud_count(n->l); aud_count(n->r);
}

int gn_ext_audit(gn_enode **roots, uint32_t nroots, int verbose) {
    memset(aud_tab, 0, sizeof aud_tab);
    for (uint32_t i = 0; i < nroots; i++) aud_count(roots[i]);
    int bad = 0; uint64_t nodes = 0;
    for (uint32_t h = 0; h < AUD_BUCKETS; h++)
        for (anode *a = aud_tab[h]; a; a = a->next) {
            nodes++;
            if (a->n->rc != a->seen) {
                bad++;
                if (verbose && bad <= 5)
                    fprintf(stderr, "  [audit] node %p rc=%u expected=%u %s\n",
                        (void*)a->n, a->n->rc, a->seen,
                        a->n->rc > a->seen ? "(LEAK)" : "(PREMATURE FREE)");
            }
        }
    if (verbose)
        fprintf(stderr, "  [audit] %llu reachable nodes, %d refcount mismatches\n",
            (unsigned long long)nodes, bad);
    for (uint32_t h = 0; h < AUD_BUCKETS; h++) {
        anode *a = aud_tab[h];
        while (a) { anode *nx = a->next; free(a); a = nx; }
        aud_tab[h] = NULL;
    }
    return bad;
}
