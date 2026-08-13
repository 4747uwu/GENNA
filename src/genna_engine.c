/* genna_engine.c — Views 3+4+5: immutable chunk sea, edit-list objects,
 * CRUD as metadata ops, search below the layer. */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>

/* ---- chunk store (L2: immutable, content-addressed, dedup) ----------- */

typedef struct { gn_cid id; gn_tok *toks; uint32_t n;
                 uint64_t blen; bool blen_set; } chunk;

struct gn_store {
    chunk    *ch;      uint32_t n_ch, cap_ch;
    /* hash id -> index+1, open addressing */
    uint64_t *hid;     uint32_t *hidx;    uint32_t h_cap;
    uint64_t  deduped;
    uint64_t  tok_bytes;
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

void gn_store_free(gn_store *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->n_ch; i++) free(s->ch[i].toks);
    free(s->ch); free(s->hid); free(s->hidx); free(s);
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
};

gn_engine *gn_engine_new(void) {
    gn_engine *e = calloc(1, sizeof *e);
    e->dict = gn_dict_new(); e->store = gn_store_new();
    e->cap_obj = 64; e->obj = malloc(e->cap_obj * sizeof(void*));
    return e;
}
void gn_engine_free(gn_engine *e) {
    if (!e) return;
    for (uint32_t i = 0; i < e->n_obj; i++) {
        gn_object *o = e->obj[i];
        for (uint32_t v = 0; v < o->n_ver; v++) free(o->ver[v].ext);
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
    if (x->off == 0 && x->len == c->n) {          /* whole chunk          */
        if (!c->blen_set) {
            c->blen = gn_detok_len(e->dict, c->toks, c->n);
            c->blen_set = true;
        }
        return c->blen;
    }
    return gn_detok_len(e->dict, c->toks + x->off, x->len);
}

static void ver_finalize(gn_engine *e, gn_version *v) {
    v->total_tokens = 0; v->total_bytes = 0;
    for (uint32_t i = 0; i < v->n_ext; i++) {
        v->total_tokens += v->ext[i].len;
        v->total_bytes  += ext_bytes(e, &v->ext[i]);
    }
    v->dict_version = gn_dict_version(e->dict);
}

/* ---- C: create -------------------------------------------------------- */

gn_object *gn_create(gn_engine *e, const char *name,
                     const uint8_t *text, size_t len) {
    e->st.bytes_in += len;
    /* learn then tokenize: the model shortens the language first */
    gn_dict_learn(e->dict, text, len, 1u << 20);
    gn_tok *toks = malloc((len + 1) * sizeof(gn_tok));
    size_t n = gn_tokenize(e->dict, text, len, toks, len + 1);

    /* cut into chunks, put each (dedup automatic) */
    uint32_t n_ext = (uint32_t)((n + GN_CHUNK_TARGET_TOKENS - 1)
                                / GN_CHUNK_TARGET_TOKENS);
    if (n_ext == 0) n_ext = 1;
    gn_object *o = calloc(1, sizeof *o);
    strncpy(o->name, name, 63);
    o->ver = calloc(1, sizeof(gn_version));
    o->n_ver = 1;
    gn_version *v = &o->ver[0];
    v->ext = malloc(n_ext * sizeof(gn_extent));
    v->n_ext = 0;
    for (size_t i = 0; i < n; i += GN_CHUNK_TARGET_TOKENS) {
        size_t cn = n - i < GN_CHUNK_TARGET_TOKENS ? n - i
                                                   : GN_CHUNK_TARGET_TOKENS;
        gn_cid id = gn_store_put(e->store, toks + i, cn);
        v->ext[v->n_ext++] = (gn_extent){ id, 0, (uint32_t)cn };
        e->st.chunks_created++;
    }
    if (n == 0) { /* empty file: one empty extent-less version */ v->n_ext = 0; }
    free(toks);
    ver_finalize(e, v);
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
            gn_object *o = e->obj[i];
            for (uint32_t v = 0; v < o->n_ver; v++) free(o->ver[v].ext);
            free(o->ver); free(o);
            e->obj[i] = e->obj[--e->n_obj];
            return 0;
        }
    return -1;
}

/* ---- R: read a byte range (View 5 left path) -------------------------- */

static size_t read_ver(gn_engine *e, const gn_version *v,
                       uint64_t off, size_t len, uint8_t *out) {
    if (off >= v->total_bytes || len == 0) return 0;
    if (off + len > v->total_bytes) len = v->total_bytes - off;

    size_t written = 0; uint64_t pos = 0;
    for (uint32_t i = 0; i < v->n_ext && written < len; i++) {
        uint64_t xb = ext_bytes(e, &v->ext[i]);
        if (pos + xb <= off) { pos += xb; continue; }   /* skip whole ext */

        size_t cn; const gn_tok *ct =
            gn_store_get(e->store, v->ext[i].chunk, &cn);
        const gn_tok *t = ct + v->ext[i].off;
        uint32_t tn = v->ext[i].len;

        /* walk tokens in this extent, emitting the overlap */
        for (uint32_t k = 0; k < tn && written < len; k++) {
            size_t tl; const uint8_t *tp = gn_dict_text(e->dict, t[k], &tl);
            e->st.tokens_detokenized++;
            if (pos + tl <= off) { pos += tl; continue; }
            size_t start = off > pos ? (size_t)(off - pos) : 0;
            size_t take  = tl - start;
            if (take > len - written) take = len - written;
            memcpy(out + written, tp + start, take);
            written += take; pos += tl;
        }
    }
    return written;
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

typedef struct { uint32_t ext_i; uint32_t tok_i; size_t byte_in_tok;
                 uint64_t bytes_before; } locus;

/* find token containing byte offset `off` (or the boundary before it).
 * O(n_ext) extent skip via cached byte lengths + O(chunk) walk inside
 * ONLY the boundary extent. This is the O(file)->O(chunk) fix.           */
static locus locate(gn_engine *e, const gn_version *v, uint64_t off) {
    locus L = {0,0,0,0}; uint64_t pos = 0;
    for (uint32_t i = 0; i < v->n_ext; i++) {
        uint64_t xb = ext_bytes(e, &v->ext[i]);        /* cached per chunk */
        if (pos + xb <= off) { pos += xb; continue; }  /* skip whole ext   */
        size_t cn; const gn_tok *ct = gn_store_get(e->store, v->ext[i].chunk, &cn);
        const gn_tok *t = ct + v->ext[i].off;
        for (uint32_t k = 0; k < v->ext[i].len; k++) {
            size_t tl; gn_dict_text(e->dict, t[k], &tl);
            if (pos + tl > off) {
                L.ext_i = i; L.tok_i = k; L.byte_in_tok = (size_t)(off - pos);
                L.bytes_before = pos; return L;
            }
            pos += tl;
        }
    }
    L.ext_i = v->n_ext; L.tok_i = 0; L.byte_in_tok = 0; L.bytes_before = pos;
    return L;
}

int gn_update(gn_engine *e, gn_object *o,
              uint64_t off, uint64_t del, const uint8_t *text, size_t len) {
    gn_version *cur = &o->ver[o->n_ver - 1];
    if (off > cur->total_bytes) return -1;
    if (off + del > cur->total_bytes) del = cur->total_bytes - off;

    locus a = locate(e, cur, off);
    locus b = locate(e, cur, off + del);

    /* Build the middle: tail-of-token-a + insert + head-of-token-b,
       re-tokenized so the language stays canonical at the seam.          */
    uint8_t *mid = NULL; size_t mid_len = 0, mid_cap = 0;
    /* leading partial token bytes */
    uint8_t tmp[4096];
    if (a.ext_i < cur->n_ext && a.byte_in_tok > 0) {
        size_t cn; const gn_tok *ct = gn_store_get(e->store, cur->ext[a.ext_i].chunk, &cn);
        size_t tl; const uint8_t *tp =
            gn_dict_text(e->dict, ct[cur->ext[a.ext_i].off + a.tok_i], &tl);
        mid_cap = a.byte_in_tok + len + 4096; mid = malloc(mid_cap);
        memcpy(mid, tp, a.byte_in_tok); mid_len = a.byte_in_tok;
    } else { mid_cap = len + 4096; mid = malloc(mid_cap); }
    if (text && len) { memcpy(mid + mid_len, text, len); mid_len += len; }
    if (b.ext_i < cur->n_ext && b.byte_in_tok > 0) {
        size_t cn; const gn_tok *ct = gn_store_get(e->store, cur->ext[b.ext_i].chunk, &cn);
        size_t tl; const uint8_t *tp =
            gn_dict_text(e->dict, ct[cur->ext[b.ext_i].off + b.tok_i], &tl);
        size_t rest = tl - b.byte_in_tok;
        if (mid_len + rest > mid_cap) { mid_cap = mid_len + rest; mid = realloc(mid, mid_cap); }
        memcpy(mid + mid_len, tp + b.byte_in_tok, rest); mid_len += rest;
        (void)tmp;
    }

    gn_tok *mtoks = malloc((mid_len + 1) * sizeof(gn_tok));
    size_t mn = gn_tokenize(e->dict, mid, mid_len, mtoks, mid_len + 1);
    free(mid);

    /* new version: exts before a | mid chunk | exts after b               */
    uint32_t cap = cur->n_ext + 4;
    gn_extent *nx = malloc(cap * sizeof(gn_extent));
    uint32_t nn = 0;

    /* full extents strictly before a.ext_i */
    for (uint32_t i = 0; i < a.ext_i && i < cur->n_ext; i++) nx[nn++] = cur->ext[i];
    /* partial head of a's extent: tokens [0, a.tok_i) */
    if (a.ext_i < cur->n_ext && a.tok_i > 0)
        nx[nn++] = (gn_extent){ cur->ext[a.ext_i].chunk,
                                cur->ext[a.ext_i].off, a.tok_i };
    /* the middle */
    if (mn > 0) {
        gn_cid mid_id = gn_store_put(e->store, mtoks, mn);
        e->st.chunks_created++;
        nx[nn++] = (gn_extent){ mid_id, 0, (uint32_t)mn };
    }
    free(mtoks);
    /* partial tail of b's extent: tokens (b.tok_i, end] — skip the split token */
    if (b.ext_i < cur->n_ext) {
        uint32_t skip = b.tok_i + (b.byte_in_tok > 0 ? 1 : 0);
        if (skip < cur->ext[b.ext_i].len)
            nx[nn++] = (gn_extent){ cur->ext[b.ext_i].chunk,
                                    cur->ext[b.ext_i].off + skip,
                                    cur->ext[b.ext_i].len - skip };
        /* full extents after b */
        for (uint32_t i = b.ext_i + 1; i < cur->n_ext; i++) {
            if (nn == cap) { cap *= 2; nx = realloc(nx, cap * sizeof(gn_extent)); }
            nx[nn++] = cur->ext[i];
        }
    }

    /* append new version (L2/L3: old version untouched)                  */
    o->ver = realloc(o->ver, (o->n_ver + 1) * sizeof(gn_version));
    gn_version *v2 = &o->ver[o->n_ver];
    v2->ext = nx; v2->n_ext = nn;
    ver_finalize(e, v2);
    o->n_ver++;
    e->st.bytes_resident = gn_store_bytes(e->store);
    return 0;
}

/* ---- search (View 5 right path): below the layer ---------------------- */

size_t gn_search(gn_engine *e, const uint8_t *needle, size_t nlen,
                 gn_hit *hits, size_t cap) {
    /* tokenize the needle with the SAME dictionary (query symmetry)      */
    gn_tok q[256];
    size_t qn = gn_tokenize(e->dict, needle, nlen, q, 256);
    if (qn == 0 || qn == (size_t)-1) return 0;

    /* last-token duality: stream may hold "word " where query has "word".
       Accept either for the final position.                              */
    gn_tok q_last_alt = GN_TOK_INVALID;
    if (!GN_TOK_IS_BYTE(q[qn-1])) {
        size_t wl; const uint8_t *wp = gn_dict_text(e->dict, q[qn-1], &wl);
        if (wl && wp[wl-1] != ' ') {
            uint8_t tmpw[256];
            if (wl < 255) {
                memcpy(tmpw, wp, wl); tmpw[wl] = ' ';
                q_last_alt = gn_dict_lookup(e->dict, tmpw, wl + 1);
            }
        }
    }
    #define LAST_OK(t) ((t) == q[qn-1] || ((t) == q_last_alt && q_last_alt))

    /* token -> byte length table: one array index instead of a function
       call per scanned token. Dict is small; build cost is negligible.   */
    uint32_t nd = gn_dict_size(e->dict);
    uint8_t *lens = malloc(nd + 1);
    lens[0] = 0;
    for (uint32_t ti = 1; ti <= nd; ti++) {
        size_t l; gn_dict_text(e->dict, ti, &l);
        lens[ti] = l < 255 ? (uint8_t)l : 255;
    }
    #define TLEN(t) (GN_TOK_IS_BYTE(t) ? 1u : (uint32_t)lens[t])

    /* instant corpus-wide no: a single-word query absent from the dict
       cannot exist in any object (it would have been learned at ingest)  */
    if (qn == 1 && GN_TOK_IS_BYTE(q[0]) && nlen >= 2) return 0;

    size_t nh = 0;
    for (uint32_t oi = 0; oi < e->n_obj && nh < cap; oi++) {
        gn_object *o = e->obj[oi];
        gn_version *v = &o->ver[o->n_ver - 1];
        uint64_t byte_pos = 0;

        /* flatten scan across extents with a small carry for straddles   */
        gn_tok carry[256]; size_t carry_n = 0; uint64_t carry_bytes = 0;
        for (uint32_t xi = 0; xi < v->n_ext && nh < cap; xi++) {
            size_t cn; const gn_tok *ct =
                gn_store_get(e->store, v->ext[xi].chunk, &cn);
            const gn_tok *t = ct + v->ext[xi].off;
            uint32_t tn = v->ext[xi].len;
            e->st.tokens_scanned += tn;

            for (uint32_t k = 0; k + 1 <= tn && nh < cap; k++) {
                /* match attempt starting in carry+current view           */
                if (carry_n) {
                    /* try matches that started in carry */
                    size_t need = qn - carry_n;
                    if (need >= 1 && need <= tn &&
                        memcmp(carry, q, carry_n*sizeof(gn_tok)) == 0 &&
                        memcmp(t, q + carry_n, (need-1)*sizeof(gn_tok)) == 0 &&
                        LAST_OK(t[need-1])) {
                        hits[nh++] = (gn_hit){ o->name, byte_pos - carry_bytes };
                    }
                    carry_n = 0; carry_bytes = 0;
                }
                if (k + qn <= tn &&
                    memcmp(t + k, q, (qn-1) * sizeof(gn_tok)) == 0 &&
                    LAST_OK(t[k + qn - 1])) {
                    hits[nh++] = (gn_hit){ o->name, byte_pos };
                }
                byte_pos += TLEN(t[k]);
            }
            /* account last token's bytes + set up straddle carry         */
            if (tn) {
                size_t tl; gn_dict_text(e->dict, t[tn-1], &tl);
                /* (loop above stopped at k=tn-1 inclusive; ensure bytes) */
            }
            if (qn > 1 && tn >= 1) {
                size_t keep = qn - 1 < tn ? qn - 1 : tn;
                memcpy(carry, t + (tn - keep), keep * sizeof(gn_tok));
                carry_n = keep; carry_bytes = 0;
                for (size_t c = 0; c < keep; c++)
                    carry_bytes += TLEN(carry[c]);
            }
        }
    }
    free(lens);
    return nh;
}

void gn_engine_stats(const gn_engine *e, gn_stats *out) {
    *out = e->st;
    out->chunks_deduped = e->store->deduped;
    out->bytes_resident = gn_store_bytes(e->store);
}
