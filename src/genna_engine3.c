/* genna_engine.c — Views 3+4+5: immutable chunk sea, edit-list objects,
 * CRUD as metadata ops, search below the layer. */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include <stdlib.h>
#include <string.h>

/* Frees the WAL handle hung off the engine (implemented in genna_persist.c). */
void gn_wal__destroy(void *w);

/* ---- chunk store (L2: immutable, content-addressed, dedup) ----------- */

typedef struct { gn_cid id; gn_tok *toks; uint32_t n;
                 uint64_t blen; bool blen_set;
                 /* toks points into a memory mapping, not the heap: it must
                  * not be freed, and it costs no resident memory until the
                  * page is touched. */
                 bool borrowed; } chunk;

struct gn_store {
    chunk    *ch;      uint32_t n_ch, cap_ch;
    /* hash id -> index+1, open addressing */
    uint64_t *hid;     uint32_t *hidx;    uint32_t h_cap;
    uint64_t  deduped;
    uint64_t  tok_bytes;
    /* the mapping backing borrowed chunks, owned by the store because the
     * chunks outlive whoever created it */
    void     *map;     size_t map_len;
    void     *map_h;   /* Windows file/section handles                     */
    uint64_t  mapped_bytes;
};

static uint64_t chunk_hash(const gn_tok *t, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = (const uint8_t*)t; size_t bytes = n * sizeof(gn_tok);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h ? h : 1;   /* 0 = empty slot sentinel */
}

gn_store *gn_store_new(void) {
    gn_store *s = calloc(1, sizeof *s);
    s->cap_ch = 1u << 12; s->ch = malloc(s->cap_ch * sizeof(chunk));
    s->h_cap = 1u << 13;
    s->hid  = calloc(s->h_cap, 8);
    s->hidx = calloc(s->h_cap, 4);
    return s;
}

void gn_store_unmap(gn_store *s);   /* defined in genna_persist.c */

void gn_store_free(gn_store *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->n_ch; i++)
        if (!s->ch[i].borrowed) free(s->ch[i].toks);
    free(s->ch); free(s->hid); free(s->hidx);
    gn_store_unmap(s);              /* after the chunks, which point into it */
    free(s);
}

static void store_hgrow(gn_store *s) {
    uint32_t oc = s->h_cap; uint64_t *oid = s->hid; uint32_t *oix = s->hidx;
    s->h_cap *= 2;
    s->hid  = calloc(s->h_cap, 8);
    s->hidx = calloc(s->h_cap, 4);
    for (uint32_t i = 0; i < oc; i++) if (oid[i]) {
        uint32_t m = s->h_cap - 1, j = (uint32_t)oid[i] & m;
        while (s->hid[j]) j = (j + 1) & m;
        s->hid[j] = oid[i]; s->hidx[j] = oix[i];
    }
    free(oid); free(oix);
}

gn_cid gn_store_put(gn_store *s, const gn_tok *toks, size_t n) {
    uint64_t h = chunk_hash(toks, n);
    uint32_t m = s->h_cap - 1, i = (uint32_t)h & m;
    while (s->hid[i]) {
        if (s->hid[i] == h) {
            chunk *c = &s->ch[s->hidx[i]];
            if (c->n == n && memcmp(c->toks, toks, n*sizeof(gn_tok)) == 0) {
                s->deduped++; return c->id;      /* dedup hit */
            }
        }
        i = (i + 1) & m;
    }
    if (s->n_ch == s->cap_ch) {
        s->cap_ch *= 2; s->ch = realloc(s->ch, s->cap_ch * sizeof(chunk));
    }
    chunk *c = &s->ch[s->n_ch];
    c->borrowed = false;
    /* BUGFIX: blen_set was never initialized. s->ch comes from realloc, so
     * on a dirty heap a stale non-zero byte made ext_bytes() return an
     * uninitialized c->blen -- corrupting every cached byte length above
     * it in the tree. Invisible in a fresh process (OS pages arrive zeroed)
     * and only reproducible after earlier allocations have dirtied the
     * heap, which is why the test suite never saw it. */
    c->blen = 0; c->blen_set = false;
    c->id = h; c->n = (uint32_t)n;
    c->toks = malloc(n * sizeof(gn_tok));
    memcpy(c->toks, toks, n * sizeof(gn_tok));
    s->hid[i] = h; s->hidx[i] = s->n_ch;
    s->n_ch++;
    s->tok_bytes += n * sizeof(gn_tok);
    if (s->n_ch * 10 > s->h_cap * 7) store_hgrow(s);
    return h;
}

const gn_tok *gn_store_get(const gn_store *s, gn_cid id, size_t *n_out) {
    uint32_t m = s->h_cap - 1, i = (uint32_t)id & m;
    while (s->hid[i]) {
        if (s->hid[i] == id) { *n_out = s->ch[s->hidx[i]].n;
                               return s->ch[s->hidx[i]].toks; }
        i = (i + 1) & m;
    }
    *n_out = 0; return NULL;
}

uint64_t gn_store_chunks(const gn_store *s) { return s->n_ch; }
uint64_t gn_store_bytes(const gn_store *s)  { return s->tok_bytes; }

/* ---- engine ----------------------------------------------------------- */

struct gn_engine {
    gn_dict  *dict;
    gn_store *store;
    gn_object **obj; uint32_t n_obj, cap_obj;
    gn_stats  st;
    /* Persistence: opaque WAL handle, NULL unless the engine is bound to a
     * store path. The engine's only knowledge of the on-disk layer is this
     * pointer and the gn_wal__* logging call at the head of each mutator. */
    void     *wal;
};

gn_engine *gn_engine_new(void) {
    gn_engine *e = calloc(1, sizeof *e);
    e->dict = gn_dict_new(); e->store = gn_store_new();
    e->cap_obj = 64; e->obj = malloc(e->cap_obj * sizeof(void*));
    return e;
}
void gn_engine_free(gn_engine *e) {
    if (!e) return;
    if (e->wal) { gn_wal__destroy(e->wal); e->wal = NULL; }
    for (uint32_t i = 0; i < e->n_obj; i++) {
        gn_object *o = e->obj[i];
        for (uint32_t v = 0; v < o->n_ver; v++) gn_ext_release(o->ver[v].root);
        free(o->ver); free(o);
    }
    free(e->obj); gn_dict_free(e->dict); gn_store_free(e->store); free(e);
}
gn_dict  *gn_engine_dict(gn_engine *e)  { return e->dict; }
gn_store *gn_engine_store(gn_engine *e) { return e->store; }

/* helper: byte length of an extent. Whole-chunk extents (the common
 * case) hit a memoized per-chunk cache -> O(1) skip. Partial extents
 * (only at splice seams, at most 2 per version) pay the walk once.       */
static chunk *store_chunk(gn_store *s, gn_cid id) {
    uint32_t m = s->h_cap - 1, i = (uint32_t)id & m;
    while (s->hid[i]) {
        if (s->hid[i] == id) return &s->ch[s->hidx[i]];
        i = (i + 1) & m;
    }
    return NULL;
}
static uint64_t ext_bytes(gn_engine *e, const gn_extent *x) {
    chunk *c = store_chunk(e->store, x->chunk);
    if (!c) return 0;
    if (x->off == 0 && x->len == c->n) {
        if (!c->blen_set) {
            c->blen = gn_detok_len(e->dict, c->toks, c->n);
            c->blen_set = true;
        }
        return c->blen;
    }
    return gn_detok_len(e->dict, c->toks + x->off, x->len);
}

/* trampoline so the extent tree can ask the engine for a byte length      */
static uint64_t ext_bytes_cb(void *ctx, const gn_extent *x) {
    return ext_bytes((gn_engine*)ctx, x);
}

static void ver_from_root(gn_engine *e, gn_version *v, gn_enode *root) {
    /* The v1/v2 flat-array fields are unused by this engine, but o->ver is
     * grown with realloc, so leaving them alone hands each new version
     * whatever bytes the allocator last had there. Zero them: uninitialized
     * struct members are exactly the kind of thing a sanitizer build (and a
     * serializer that walks every version) will trip over. */
    v->ext = NULL; v->n_ext = 0;
    v->root = root;
    v->total_tokens = gn_ext_tokens(root);
    v->total_bytes  = gn_ext_bytes(root);
    v->dict_version = gn_dict_version(e->dict);
}

/* ---- C: create -------------------------------------------------------- */

gn_object *gn_create(gn_engine *e, const char *name,
                     const uint8_t *text, size_t len) {
    gn_wal__create(e, name, text, len);     /* durable BEFORE it is applied */
    e->st.bytes_in += len;
    gn_dict_learn(e->dict, text, len, 1u << 20);
    gn_tok *toks = malloc((len + 1) * sizeof(gn_tok));
    size_t n = gn_tokenize(e->dict, text, len, toks, len + 1);

    uint32_t n_ext = (uint32_t)((n + GN_CHUNK_TARGET_TOKENS - 1)
                                / GN_CHUNK_TARGET_TOKENS);
    gn_object *o = calloc(1, sizeof *o);
    strncpy(o->name, name, 63);
    o->ver = calloc(1, sizeof(gn_version));
    o->n_ver = 1;

    gn_enode *root = NULL;
    if (n > 0) {
        gn_extent *ex = malloc(n_ext * sizeof(gn_extent));
        uint64_t  *bl = malloc(n_ext * sizeof(uint64_t));
        uint32_t k = 0;
        for (size_t i = 0; i < n; i += GN_CHUNK_TARGET_TOKENS) {
            size_t cn = n - i < GN_CHUNK_TARGET_TOKENS ? n - i
                                                       : GN_CHUNK_TARGET_TOKENS;
            gn_cid id = gn_store_put(e->store, toks + i, cn);
            ex[k] = (gn_extent){ id, 0, (uint32_t)cn };
            bl[k] = ext_bytes(e, &ex[k]);
            k++;
            e->st.chunks_created++;
        }
        root = gn_ext_build(ex, bl, k);
        free(ex); free(bl);
    }
    free(toks);
    ver_from_root(e, &o->ver[0], root);

    if (e->n_obj == e->cap_obj) {
        e->cap_obj *= 2; e->obj = realloc(e->obj, e->cap_obj * sizeof(void*));
    }
    e->obj[e->n_obj++] = o;
    e->st.chunks_deduped = e->store->deduped;
    e->st.bytes_resident = gn_store_bytes(e->store);
    return o;
}

gn_object *gn_object_open(gn_engine *e, const char *name) {
    for (uint32_t i = 0; i < e->n_obj; i++)
        if (strcmp(e->obj[i]->name, name) == 0) return e->obj[i];
    return NULL;
}

int gn_delete(gn_engine *e, const char *name) {
    for (uint32_t i = 0; i < e->n_obj; i++)
        if (strcmp(e->obj[i]->name, name) == 0) {
            gn_wal__delete(e, name);
            gn_object *o = e->obj[i];
            /* release every version's tree, or the whole object's node graph
             * leaks -- gn_engine_free does this and gn_delete did not.     */
            for (uint32_t v = 0; v < o->n_ver; v++) gn_ext_release(o->ver[v].root);
            free(o->ver); free(o);
            e->obj[i] = e->obj[--e->n_obj];
            return 0;
        }
    return -1;
}

/* ---- R: read a byte range (View 5 left path) -------------------------- */

typedef struct {
    gn_engine *e; uint64_t off; size_t len, written; uint64_t pos; uint8_t *out;
} readctx;

/* `start` is the leaf's absolute byte offset, supplied by the range walk, so
 * this no longer needs to be handed every preceding leaf to track position. */
static void read_leaf(void *ctx, const gn_extent *x, uint64_t xb, uint64_t start) {
    (void)xb;
    readctx *R = (readctx*)ctx;
    if (R->written >= R->len) return;
    uint64_t pos = start;

    size_t cn; const gn_tok *ct = gn_store_get(R->e->store, x->chunk, &cn);
    const gn_tok *t = ct + x->off;
    for (uint32_t k = 0; k < x->len && R->written < R->len; k++) {
        size_t tl; const uint8_t *tp = gn_dict_text(R->e->dict, t[k], &tl);
        R->e->st.tokens_detokenized++;
        if (pos + tl <= R->off) { pos += tl; continue; }
        size_t soff = R->off > pos ? (size_t)(R->off - pos) : 0;
        size_t take  = tl - soff;
        if (take > R->len - R->written) take = R->len - R->written;
        memcpy(R->out + R->written, tp + soff, take);
        R->written += take; pos += tl;
    }
}

static size_t read_ver(gn_engine *e, const gn_version *v,
                       uint64_t off, size_t len, uint8_t *out) {
    if (off >= v->total_bytes || len == 0) return 0;
    if (off + len > v->total_bytes) len = v->total_bytes - off;
    readctx R = { e, off, len, 0, 0, out };
    /* Range-limited: only the O(log n) spine plus the covered leaves. Using
     * gn_ext_walk here made a 1 KB read cost O(total leaves). */
    gn_ext_walk_range(v->root, off, len, read_leaf, &R);
    return R.written;
}

size_t gn_read(gn_engine *e, gn_object *o,
               uint64_t off, size_t len, uint8_t *out) {
    return read_ver(e, &o->ver[o->n_ver - 1], off, len, out);
}
size_t gn_read_version(gn_engine *e, gn_object *o, uint32_t version,
                       uint64_t off, size_t len, uint8_t *out) {
    if (version >= o->n_ver) return 0;
    return read_ver(e, &o->ver[version], off, len, out);
}

/* ---- U: splice (View 4, the money verb) -------------------------------
 * Replace byte range [off, off+del) with text[0..len).
 * Cost: locate (walk extents) + split ≤2 boundary chunks + tokenize insert.
 * Never touches chunks outside the boundary.                             */

/* locus: a position expressed as a GLOBAL token index plus the byte offset
 * inside that token.  Found in O(log n) via the tree, then a bounded walk
 * inside one leaf's chunk (<= GN_CHUNK_TARGET_TOKENS).                    */
typedef struct { uint64_t tok; size_t byte_in_tok; uint64_t bytes_before; } locus;

static locus locate(gn_engine *e, const gn_version *v, uint64_t off) {
    locus L = {0,0,0};
    if (off >= v->total_bytes) { L.tok = v->total_tokens; L.bytes_before = v->total_bytes; return L; }
    gn_extent x; uint64_t tb, bb;
    if (!gn_ext_locate_byte(v->root, off, &x, &tb, &bb)) {
        L.tok = v->total_tokens; L.bytes_before = v->total_bytes; return L;
    }
    size_t cn; const gn_tok *ct = gn_store_get(e->store, x.chunk, &cn);
    const gn_tok *t = ct + x.off;
    uint64_t pos = bb;
    for (uint32_t k = 0; k < x.len; k++) {
        size_t tl; gn_dict_text(e->dict, t[k], &tl);
        if (pos + tl > off) {
            L.tok = tb + k; L.byte_in_tok = (size_t)(off - pos); L.bytes_before = pos;
            return L;
        }
        pos += tl;
    }
    L.tok = tb + x.len; L.bytes_before = pos;
    return L;
}

/* materialize the bytes of one global token index (seam repair only) */
static const uint8_t *tok_at(gn_engine *e, const gn_version *v, uint64_t ti,
                             size_t *tl) {
    gn_enode *lo = NULL, *hi = NULL;
    gn_ext_split_tok(v->root, ti, &lo, &hi, ext_bytes_cb, e);
    const uint8_t *res = NULL; *tl = 0;
    gn_extent x; uint64_t tb, bb;
    if (hi && gn_ext_locate_byte(hi, 0, &x, &tb, &bb)) {
        size_t cn; const gn_tok *ct = gn_store_get(e->store, x.chunk, &cn);
        res = gn_dict_text(e->dict, ct[x.off], tl);
    }
    gn_ext_release(lo); gn_ext_release(hi);
    return res;
}

int gn_update(gn_engine *e, gn_object *o,
              uint64_t off, uint64_t del, const uint8_t *text, size_t len) {
    gn_version *cur = &o->ver[o->n_ver - 1];
    /* Log only edits that will actually be attempted: a rejected splice
     * changes nothing, so logging it would make replay diverge.           */
    if (off > cur->total_bytes) return -1;
    gn_wal__update(e, o->name, off, del, text, len);
    if (off + del > cur->total_bytes) del = cur->total_bytes - off;

    locus a = locate(e, cur, off);
    locus b = locate(e, cur, off + del);

    /* seam: tail of token a + insert + head of token b, re-tokenized so the
       language stays canonical across the join.                           */
    uint8_t *mid = NULL; size_t mid_len = 0, mid_cap = len + 4096;
    mid = malloc(mid_cap);
    if (a.byte_in_tok > 0) {
        size_t tl; const uint8_t *tp = tok_at(e, cur, a.tok, &tl);
        if (tp) { memcpy(mid, tp, a.byte_in_tok); mid_len = a.byte_in_tok; }
    }
    if (text && len) { memcpy(mid + mid_len, text, len); mid_len += len; }
    if (b.byte_in_tok > 0) {
        size_t tl; const uint8_t *tp = tok_at(e, cur, b.tok, &tl);
        if (tp && tl > b.byte_in_tok) {
            size_t rest = tl - b.byte_in_tok;
            if (mid_len + rest > mid_cap) { mid_cap = mid_len + rest; mid = realloc(mid, mid_cap); }
            memcpy(mid + mid_len, tp + b.byte_in_tok, rest); mid_len += rest;
        }
    }

    /* BUGFIX: learn before tokenizing. gn_update's input is bytes the
     * engine has never seen; without a learn pass every novel unit falls
     * through to byte-escape. In text that is a quiet 4x expansion of
     * inserted content. In video a single unseen frame explodes into tens
     * of thousands of one-byte tokens. Append-only, so L3 still holds. */
    gn_dict_learn(e->dict, mid, mid_len, 1u << 20);
    gn_tok *mtoks = malloc((mid_len + 1) * sizeof(gn_tok));
    size_t mn = gn_tokenize(e->dict, mid, mid_len, mtoks, mid_len + 1);
    free(mid);

    /* THE EDIT: two splits and two joins.  O(log n) nodes allocated; every
       untouched subtree is shared with the previous version.              */
    uint64_t keep_left  = a.tok;                                   /* tokens */
    uint64_t drop_right = b.tok + (b.byte_in_tok > 0 ? 1 : 0);

    gn_enode *L = NULL, *rest = NULL, *M = NULL, *R = NULL;
    gn_ext_split_tok(cur->root, keep_left, &L, &rest, ext_bytes_cb, e);
    gn_ext_split_tok(rest, drop_right - keep_left, &M, &R, ext_bytes_cb, e);
    gn_ext_release(rest);   /* consumed by the second split                */
    gn_ext_release(M);      /* the removed middle: no version refers to it */

    gn_enode *mid_node = NULL;
    if (mn > 0) {
        gn_cid mid_id = gn_store_put(e->store, mtoks, mn);
        e->st.chunks_created++;
        gn_extent mx = { mid_id, 0, (uint32_t)mn };
        uint64_t mb = ext_bytes(e, &mx);
        mid_node = gn_ext_build(&mx, &mb, 1);
    }
    free(mtoks);

    gn_enode *lm = gn_ext_join(L, mid_node);
    gn_enode *root = gn_ext_join(lm, R);
    gn_ext_release(L); gn_ext_release(mid_node);
    gn_ext_release(lm); gn_ext_release(R);

    o->ver = realloc(o->ver, (o->n_ver + 1) * sizeof(gn_version));
    ver_from_root(e, &o->ver[o->n_ver], root);
    o->n_ver++;
    e->st.bytes_resident = gn_store_bytes(e->store);
    return 0;
}

/* Drop all but the newest `keep` versions. Releases their roots; nodes go
 * back to the free list exactly when no surviving version still shares
 * them. This is what makes an all-day editing session bounded. */
uint32_t gn_trim_history(gn_engine *e, gn_object *o, uint32_t keep) {
    if (keep == 0) keep = 1;
    if (o->n_ver <= keep) return 0;
    gn_wal__trim(e, o->name, keep);
    uint32_t drop = o->n_ver - keep;
    for (uint32_t i = 0; i < drop; i++) gn_ext_release(o->ver[i].root);
    memmove(o->ver, o->ver + drop, keep * sizeof(gn_version));
    o->n_ver = keep;
    return drop;
}

/* ---- graft: the timeline primitive -----------------------------------
 * Insert bytes [src_off, src_off+src_len) OF ANOTHER OBJECT into dst at
 * dst_off, without re-parsing or copying a single frame.  Three splits and
 * two joins over shared subtrees: O(log n), independent of clip length.
 *
 * This is what an NLE actually does all day (copy, move, ripple, duplicate)
 * and it is where the engine should beat a file-based tool by orders of
 * magnitude rather than constants.  gn_update() must re-tokenize because
 * its input is raw bytes the engine has never seen; graft's input is
 * already in the language, so there is nothing to convert.
 *
 * All three offsets must land on token (frame) boundaries -- splicing
 * mid-frame would emit an undecodable stream.  Returns -1 if they do not.
 */
int gn_graft(gn_engine *e, gn_object *dst, uint64_t dst_off,
             gn_object *src, uint64_t src_off, uint64_t src_len) {
    gn_version *dv = &dst->ver[dst->n_ver - 1];
    gn_version *sv = &src->ver[src->n_ver - 1];
    if (dst_off > dv->total_bytes) return -1;
    if (src_off + src_len > sv->total_bytes) return -1;

    locus d0 = locate(e, dv, dst_off);
    locus s0 = locate(e, sv, src_off);
    locus s1 = locate(e, sv, src_off + src_len);
    if (d0.byte_in_tok || s0.byte_in_tok || s1.byte_in_tok) return -1;
    gn_wal__graft(e, dst->name, dst_off, src->name, src_off, src_len);

    gn_enode *sa = NULL, *sr = NULL, *mid = NULL, *sz = NULL;
    gn_ext_split_tok(sv->root, s0.tok, &sa, &sr, ext_bytes_cb, e);
    gn_ext_split_tok(sr, s1.tok - s0.tok, &mid, &sz, ext_bytes_cb, e);
    gn_ext_release(sr); gn_ext_release(sa); gn_ext_release(sz);

    gn_enode *L = NULL, *R = NULL;
    gn_ext_split_tok(dv->root, d0.tok, &L, &R, ext_bytes_cb, e);

    gn_enode *lm = gn_ext_join(L, mid);
    gn_enode *root = gn_ext_join(lm, R);
    gn_ext_release(L); gn_ext_release(mid); gn_ext_release(lm); gn_ext_release(R);

    dst->ver = realloc(dst->ver, (dst->n_ver + 1) * sizeof(gn_version));
    ver_from_root(e, &dst->ver[dst->n_ver], root);
    dst->n_ver++;
    return 0;
}

/* cut bytes [off, off+len) at frame boundaries -- no seam re-tokenization */
int gn_cut(gn_engine *e, gn_object *o, uint64_t off, uint64_t len) {
    gn_version *v = &o->ver[o->n_ver - 1];
    if (off + len > v->total_bytes) return -1;
    locus a = locate(e, v, off), b = locate(e, v, off + len);
    if (a.byte_in_tok || b.byte_in_tok) return -1;
    gn_wal__cut(e, o->name, off, len);
    gn_enode *L = NULL, *rest = NULL, *M = NULL, *R = NULL;
    gn_ext_split_tok(v->root, a.tok, &L, &rest, ext_bytes_cb, e);
    gn_ext_split_tok(rest, b.tok - a.tok, &M, &R, ext_bytes_cb, e);
    gn_ext_release(rest); gn_ext_release(M);
    gn_enode *root = gn_ext_join(L, R);
    gn_ext_release(L); gn_ext_release(R);
    o->ver = realloc(o->ver, (o->n_ver + 1) * sizeof(gn_version));
    ver_from_root(e, &o->ver[o->n_ver], root);
    o->n_ver++;
    return 0;
}

/* ---- search v2: dictionary-as-index ----------------------------------
 * The v1 scan assumed the needle tokenizes the same way in isolation as in
 * context.  With phrase merging that is false: "PacketResponder" is absorbed
 * into a longer entry and its standalone id never appears in the stream.
 *
 * Fix: the dictionary is small (~1-2MB of text) compared to the corpus, so
 * we can brute-force it.  Build the set S of entry ids that could BEGIN a
 * match -- any entry that contains the needle, or whose suffix matches a
 * prefix of the needle.  Scan the token stream testing membership in S via a
 * bitmap, then byte-verify forward only at candidates.  If S is empty the
 * needle cannot occur anywhere: the instant corpus-wide "no" survives.      */

typedef struct { gn_engine *e; gn_tok *flat; uint64_t w; } flatctx;
static void flat_leaf(void *ctx, const gn_extent *x, uint64_t xb) {
    (void)xb; flatctx *F = (flatctx*)ctx;
    size_t cn; const gn_tok *ct = gn_store_get(F->e->store, x->chunk, &cn);
    memcpy(F->flat + F->w, ct + x->off, x->len * sizeof(gn_tok));
    F->w += x->len;
}

size_t gn_search(gn_engine *e, const uint8_t *needle, size_t nlen,
                 gn_hit *hits, size_t cap) {
    if (nlen == 0) return 0;
    uint32_t nd = gn_dict_size(e->dict);

    /* Candidate table: for each entry, ALL byte offsets inside it at which a
     * match may begin -- either the needle is fully contained there, or the
     * entry's suffix from there equals a prefix of the needle (match runs on
     * into the following tokens).  Collecting only the first offset
     * undercounts needles that occur twice inside one token.               */
    uint32_t *coff_start = calloc((size_t)nd + 2, sizeof(uint32_t));
    uint32_t *coff = NULL; uint32_t coff_n = 0, coff_cap = 0;
    for (uint32_t id = 1; id <= nd; id++) {
        coff_start[id] = coff_n;
        size_t el; const uint8_t *ep = gn_dict_text(e->dict, id, &el);
        for (size_t o = 0; o < el; o++) {
            size_t avail = el - o;
            size_t cmp = avail < nlen ? avail : nlen;
            if (memcmp(ep + o, needle, cmp) != 0) continue;
            if (cmp == nlen || o + cmp == el) {
                if (coff_n == coff_cap) {
                    coff_cap = coff_cap ? coff_cap * 2 : 1024;
                    coff = realloc(coff, coff_cap * sizeof(uint32_t));
                }
                coff[coff_n++] = (uint32_t)o;
            }
        }
    }
    coff_start[nd + 1] = coff_n;
    /* fix up: coff_start[id+1] must be the end of id's run */
    {   uint32_t *cs = malloc(((size_t)nd + 2) * sizeof(uint32_t));
        for (uint32_t id = 1; id <= nd; id++) cs[id] = coff_start[id];
        cs[nd + 1] = coff_n;
        for (uint32_t id = nd; id >= 1; id--) {
            /* end of id = start of id+1 (already correct since we filled in
               increasing id order); nothing to do */
            if (id == 1) break;
        }
        free(cs);
    }

    size_t nh = 0;
    for (uint32_t oi = 0; oi < e->n_obj && nh < cap; oi++) {
        gn_object *o = e->obj[oi];
        gn_version *v = &o->ver[o->n_ver - 1];

        /* flatten this version's tokens once (v1 scanned extents directly;
         * flattening keeps the forward byte-verify simple and correct).    */
        uint64_t nt = v->total_tokens;
        gn_tok *flat = malloc((size_t)nt * sizeof(gn_tok));
        flatctx F = { e, flat, 0 };
        gn_ext_walk(v->root, flat_leaf, &F);
        e->st.tokens_scanned += nt;

        uint64_t byte_pos = 0;
        for (uint64_t k = 0; k < nt && nh < cap; k++) {
            gn_tok t = flat[k];
            size_t tl; const uint8_t *tp = gn_dict_text(e->dict, t, &tl);

            uint32_t c0, c1; uint32_t esc_one = 0;
            if (GN_TOK_IS_BYTE(t)) {
                esc_one = (needle[0] == GN_TOK_BYTE_VAL(t)) ? 1 : 0;
                c0 = c1 = 0;
            } else if (t <= nd) {
                c0 = coff_start[t]; c1 = coff_start[t + 1];
            } else { c0 = c1 = 0; }

            uint32_t ncand = esc_one ? 1 : (c1 - c0);
            for (uint32_t ci = 0; ci < ncand && nh < cap; ci++) {
                size_t so = esc_one ? 0 : coff[c0 + ci];
                size_t need = nlen, m = 0;
                size_t off_in = so; uint64_t kk = k;
                size_t cl = tl; const uint8_t *cp = tp;
                int ok = 1;
                while (m < need) {
                    if (off_in >= cl) {
                        kk++;
                        if (kk >= nt) { ok = 0; break; }
                        cp = gn_dict_text(e->dict, flat[kk], &cl);
                        off_in = 0;
                        if (cl == 0) { ok = 0; break; }
                    }
                    size_t take = cl - off_in;
                    if (take > need - m) take = need - m;
                    if (memcmp(cp + off_in, needle + m, take) != 0) { ok = 0; break; }
                    m += take; off_in += take;
                }
                if (ok) hits[nh++] = (gn_hit){ o->name, byte_pos + so };
            }
            byte_pos += tl;
        }
        free(flat);
    }
    free(coff_start); free(coff);
    return nh;
}

void gn_engine_stats(const gn_engine *e, gn_stats *out) {
    *out = e->st;
    out->chunks_deduped = e->store->deduped;
    out->bytes_resident = gn_store_bytes(e->store);
}

/* ---- persistence accessors -------------------------------------------
 * The on-disk layer needs to read the store, the object table and the stats
 * without those structs leaking into the public header, and needs to put an
 * object back exactly as it was. Read-only views plus one attach; no new
 * behaviour, nothing the engine itself calls.                              */

uint32_t gn_store__count(const gn_store *s) { return s->n_ch; }

const gn_tok *gn_store__at(const gn_store *s, uint32_t i,
                           uint32_t *n_out, gn_cid *id_out) {
    if (i >= s->n_ch) { *n_out = 0; *id_out = 0; return NULL; }
    *n_out = s->ch[i].n; *id_out = s->ch[i].id;
    return s->ch[i].toks;
}
uint64_t gn_store__deduped(const gn_store *s)          { return s->deduped; }
void     gn_store__set_deduped(gn_store *s, uint64_t v){ s->deduped = v; }

/* ---- out-of-core support --------------------------------------------
 * Install a chunk whose token array lives in a mapping. Identical to
 * gn_store_put except that nothing is copied and nothing will be freed.
 * The caller must have verified the hash, because this trusts it.        */
gn_cid gn_store__put_borrowed(gn_store *s, gn_cid id, gn_tok *toks, uint32_t n) {
    uint32_t m = s->h_cap - 1, i = (uint32_t)id & m;
    while (s->hid[i]) {
        if (s->hid[i] == id) {
            chunk *c = &s->ch[s->hidx[i]];
            if (c->n == n && memcmp(c->toks, toks, (size_t)n * sizeof(gn_tok)) == 0) {
                s->deduped++; return c->id;
            }
        }
        i = (i + 1) & m;
    }
    if (s->n_ch == s->cap_ch) {
        s->cap_ch *= 2; s->ch = realloc(s->ch, s->cap_ch * sizeof(chunk));
    }
    chunk *c = &s->ch[s->n_ch];
    c->blen = 0; c->blen_set = false; c->borrowed = true;
    c->id = id; c->n = n; c->toks = toks;
    s->hid[i] = id; s->hidx[i] = s->n_ch;
    s->n_ch++;
    s->tok_bytes += (uint64_t)n * sizeof(gn_tok);
    s->mapped_bytes += (uint64_t)n * sizeof(gn_tok);
    if (s->n_ch * 10 > s->h_cap * 7) store_hgrow(s);
    return id;
}

void gn_store__set_map(gn_store *s, void *map, size_t len, void *h) {
    s->map = map; s->map_len = len; s->map_h = h;
}
void  gn_store__get_map(const gn_store *s, void **map, size_t *len, void **h) {
    *map = s->map; *len = s->map_len; *h = s->map_h;
}
uint64_t gn_store__mapped_bytes(const gn_store *s) { return s->mapped_bytes; }

int gn_store_is_mapped(const gn_engine *e) {
    return (e && e->store && e->store->map) ? 1 : 0;
}
uint64_t gn_store_mapped_bytes(const gn_engine *e) {
    return (e && e->store) ? e->store->mapped_bytes : 0;
}

uint32_t   gn_engine__n_obj(const gn_engine *e) { return e->n_obj; }
gn_object *gn_engine__obj(const gn_engine *e, uint32_t i) {
    return i < e->n_obj ? e->obj[i] : NULL;
}
void gn_engine__attach(gn_engine *e, gn_object *o) {
    if (e->n_obj == e->cap_obj) {
        e->cap_obj *= 2; e->obj = realloc(e->obj, e->cap_obj * sizeof(void*));
    }
    e->obj[e->n_obj++] = o;
}
void gn_engine__stats_get(const gn_engine *e, gn_stats *out) { *out = e->st; }
void gn_engine__stats_set(gn_engine *e, const gn_stats *in)  { e->st = *in; }
void *gn_engine__wal(const gn_engine *e)          { return e->wal; }
void  gn_engine__set_wal(gn_engine *e, void *w)   { e->wal = w; }

/* public object enumeration (declared in genna_persist.h) */
uint32_t   gn_engine_objects(const gn_engine *e)             { return e->n_obj; }
gn_object *gn_engine_object(const gn_engine *e, uint32_t i)  { return gn_engine__obj(e, i); }

/* ---- Genna-Net hooks -------------------------------------------------- */
/* rebuild an object from a resolved extent list (chunks already in store) */
gn_object* gn_net__rebuild(gn_engine*e,const char*name,gn_extent*ex,uint32_t ne){
    gn_object*o=calloc(1,sizeof*o);
    strncpy(o->name,name,63);
    o->ver=calloc(1,sizeof(gn_version)); o->n_ver=1;
    uint64_t*bl=malloc((ne?ne:1)*sizeof(uint64_t));
    for(uint32_t i=0;i<ne;i++) bl[i]=ext_bytes(e,&ex[i]);
    gn_enode*root = ne? gn_ext_build(ex,bl,ne) : NULL;
    free(bl);
    ver_from_root(e,&o->ver[0],root);
    if(e->n_obj==e->cap_obj){e->cap_obj*=2;e->obj=realloc(e->obj,e->cap_obj*sizeof(void*));}
    e->obj[e->n_obj++]=o;
    e->st.bytes_resident=gn_store_bytes(e->store);
    return o;
}
