/* genna_vdict.c — View 2 for video.  The SAME gn_dict interface, but the
 * "words" of the language are H.264 access units instead of text words.
 *
 * The whole point of the probe: genna_engine3.c is not modified at all.
 * Only the language module is swapped.  If the engine really is medium-
 * independent, CRUD-below-the-layer should work on video unchanged.
 *
 * Mapping:
 *   text word        -> access unit (one coded frame + its preceding
 *                       parameter-set / SEI / AUD NALs)
 *   dict entry       -> unique compressed frame, content-addressed
 *   token (u32)      -> frame id
 *   detokenize       -> concatenate frame bytes = a playable elementary
 *                       stream, byte-identical to the input
 *   chunk            -> a run of frames (set to one GOP at build time)
 *
 * Dedup falls out for free: two identical coded frames anywhere in the
 * corpus hash to the same entry and are stored once.
 */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t off; uint32_t len; gn_tok id; } vent;

struct gn_dict {
    uint8_t  *blob;   uint64_t blob_len, blob_cap;   /* frame bytes        */
    uint64_t *e_off;  uint32_t *e_len;  uint8_t *e_idr;
    uint32_t  n_entries, cap_entries;
    vent     *tab;    uint32_t tab_cap;
    uint64_t  dup_bytes;      /* bytes saved by identical-frame dedup      */
    uint32_t  dup_frames;
};

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* OPT: frames are 10s of KB; hashing every byte made learn() O(stream).
 * Hash the length plus a bounded sample (head, middle, tail) instead, so
 * hashing is O(1) per frame.  Correctness is unaffected: tab_slot() still
 * does a full memcmp on every candidate, so a sample collision costs one
 * extra compare, never a wrong answer. */
#define GN_HSAMPLE 192u
static uint64_t frame_hash(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL ^ (uint64_t)n * 1099511628211ULL;
    size_t k = n < GN_HSAMPLE ? n : GN_HSAMPLE;
    for (size_t i = 0; i < k; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    if (n > GN_HSAMPLE) {
        const uint8_t *t = p + n - k;
        for (size_t i = 0; i < k; i++) { h ^= t[i]; h *= 1099511628211ULL; }
        const uint8_t *m = p + n / 2 - (k / 2);
        for (size_t i = 0; i < k; i++) { h ^= m[i]; h *= 1099511628211ULL; }
    }
    return h;
}

static void tab_grow(gn_dict *d);

/* ---- persistence interface -------------------------------------------
 * The snapshot's dictionary section stores u32 text offsets over one blob.
 * This module's entries are u64 offsets plus a per-frame IDR flag, which
 * that section cannot represent. Rather than write a store that silently
 * loses the IDR flags and truncates offsets above 4 GB, this module declares
 * itself non-serializable and gn_save() refuses with ENOTSUP.
 *
 * Persisting a video store needs a versioned, per-module dictionary section;
 * it is listed as not-yet-handled in PERSISTENCE.md rather than faked here. */
int gn_dict__serializable(const gn_dict *d) { (void)d; return 0; }

uint32_t        gn_dict__entries(const gn_dict *d) { return d->n_entries; }
const uint8_t  *gn_dict__texts(const gn_dict *d, uint64_t *len_out) {
    (void)d; *len_out = 0; return NULL;
}
const uint32_t *gn_dict__offs(const gn_dict *d) { (void)d; return NULL; }
const uint32_t *gn_dict__lens(const gn_dict *d) { (void)d; return NULL; }
int gn_dict__restore(gn_dict *d, const uint8_t *texts, uint64_t texts_len,
                     const uint32_t *offs, const uint32_t *lens, uint32_t n) {
    (void)d; (void)texts; (void)texts_len; (void)offs; (void)lens; (void)n;
    return -1;
}

gn_dict *gn_dict_new(void) {
    gn_dict *d = calloc(1, sizeof *d);
    d->tab_cap = 1u << 14;
    d->tab = calloc(d->tab_cap, sizeof(vent));
    d->cap_entries = 1u << 12;
    d->e_off = malloc(d->cap_entries * 8);
    d->e_len = malloc(d->cap_entries * 4);
    d->e_idr = malloc(d->cap_entries);
    d->blob_cap = 1u << 22; d->blob = malloc(d->blob_cap);
    return d;
}
void gn_dict_free(gn_dict *d) {
    if (!d) return;
    free(d->blob); free(d->e_off); free(d->e_len); free(d->e_idr);
    free(d->tab); free(d);
}
uint32_t gn_dict_size(const gn_dict *d)    { return d->n_entries; }
uint64_t gn_dict_version(const gn_dict *d) { return d->n_entries; }

/* dedup accounting, for the probe report */
uint64_t gn_vdict_dup_bytes(const gn_dict *d)  { return d->dup_bytes; }
uint32_t gn_vdict_dup_frames(const gn_dict *d) { return d->dup_frames; }
int      gn_vdict_is_idr(const gn_dict *d, gn_tok t) {
    if (GN_TOK_IS_BYTE(t) || t == GN_TOK_INVALID || t > d->n_entries) return 0;
    return d->e_idr[t-1];
}

static vent *tab_slot(const gn_dict *d, const uint8_t *w, size_t n) {
    uint64_t h = frame_hash(w, n);
    uint32_t mask = d->tab_cap - 1, i = (uint32_t)h & mask;
    for (;;) {
        vent *e = &d->tab[i];
        if (e->id == GN_TOK_INVALID) return e;
        if (e->len == n && memcmp(d->blob + e->off, w, n) == 0) return e;
        i = (i + 1) & mask;
    }
}

gn_tok gn_dict_lookup(const gn_dict *d, const uint8_t *w, size_t n) {
    if (n == 0) return GN_TOK_INVALID;
    return tab_slot(d, w, n)->id;
}

static gn_tok dict_add(gn_dict *d, const uint8_t *w, size_t n, int idr) {
    vent *e = tab_slot(d, w, n);
    if (e->id != GN_TOK_INVALID) {                 /* identical frame seen */
        d->dup_frames++; d->dup_bytes += n;
        return e->id;
    }
    if (d->blob_len + n > d->blob_cap) {
        while (d->blob_len + n > d->blob_cap) d->blob_cap *= 2;
        d->blob = realloc(d->blob, d->blob_cap);
    }
    if (d->n_entries == d->cap_entries) {
        d->cap_entries *= 2;
        d->e_off = realloc(d->e_off, d->cap_entries * 8);
        d->e_len = realloc(d->e_len, d->cap_entries * 4);
        d->e_idr = realloc(d->e_idr, d->cap_entries);
    }
    uint64_t off = d->blob_len;
    memcpy(d->blob + off, w, n);
    d->blob_len += n;
    uint32_t idx = d->n_entries++;
    d->e_off[idx] = off; d->e_len[idx] = (uint32_t)n; d->e_idr[idx] = (uint8_t)idr;
    e->off = off; e->len = (uint32_t)n; e->id = idx + 1;
    gn_tok nid = e->id;
    if (d->n_entries * 10 > d->tab_cap * 7) tab_grow(d);
    return nid;
}

static void tab_grow(gn_dict *d) {
    uint32_t oc = d->tab_cap; vent *old = d->tab;
    d->tab_cap *= 2;
    d->tab = calloc(d->tab_cap, sizeof(vent));
    for (uint32_t i = 0; i < oc; i++) {
        if (old[i].id == GN_TOK_INVALID) continue;
        vent *e = tab_slot(d, d->blob + old[i].off, old[i].len);
        *e = old[i];
    }
    free(old);
}

/* Thread-safety fix: v1 returned a pointer to a single shared mutable byte,
 * so two concurrent readers materializing different escape tokens clobbered
 * each other. A const 256-entry identity table gives every byte value its
 * own immutable cell: no writes, no sharing, pointer valid forever. */
static const uint8_t GN_BYTE_TABLE[256] = {
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
     32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
     48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
     64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
     80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
     96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,111,
    112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
    160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
    192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
    208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
    224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,
};

const uint8_t *gn_dict_text(const gn_dict *d, gn_tok t, size_t *len_out) {
    if (GN_TOK_IS_BYTE(t)) { *len_out = 1; return &GN_BYTE_TABLE[GN_TOK_BYTE_VAL(t)]; }
    if (t == GN_TOK_INVALID || t > d->n_entries) { *len_out = 0; return NULL; }
    *len_out = d->e_len[t-1];
    return d->blob + d->e_off[t-1];
}

/* ---- Annex-B access-unit segmentation --------------------------------
 * NALs are delimited by 00 00 01 / 00 00 00 01.  An access unit is closed
 * by a VCL NAL (type 1 = non-IDR slice, type 5 = IDR slice); any preceding
 * AUD/SPS/PPS/SEI NALs belong to that unit.  Byte-exact partition, so
 * concatenating units reproduces the stream exactly.                     */

/* OPT: every Annex-B start code ends in 0x01, so let memchr (SIMD in glibc)
 * find the candidates and only then check the two preceding zero bytes.
 * Replaces a byte-at-a-time scan over the whole elementary stream. */
static size_t next_sc(const uint8_t *b, size_t n, size_t i, size_t *sc_len) {
    if (i + 2 >= n) return n;
    const uint8_t *p = b + i + 2;
    size_t remain = n - i - 2;
    while (remain > 0) {
        const uint8_t *q = memchr(p, 0x01, remain);
        if (!q) return n;
        size_t at = (size_t)(q - b);           /* index of the 0x01 byte    */
        if (at >= 2 && b[at-1] == 0 && b[at-2] == 0) {
            size_t start = at - 2;
            if (start >= i) {
                if (start >= 1 && b[start-1] == 0 && start - 1 >= i) {
                    *sc_len = 4; return start - 1;
                }
                *sc_len = 3; return start;
            }
        }
        remain -= (size_t)(q - p) + 1;
        p = q + 1;
    }
    return n;
}

/* find AU boundaries: starts[] gets each unit's start offset, starts[nu]=len */
static size_t au_segment(const uint8_t *b, size_t n, uint64_t *starts,
                         uint8_t *is_idr) {
    size_t nu = 0, i = 0, sc;
    size_t cur_start = 0; int seen_vcl = 0, cur_idr = 0;
    i = next_sc(b, n, 0, &sc);
    cur_start = i;
    while (i < n) {
        size_t nal = i + sc;
        if (nal >= n) break;
        uint8_t type = b[nal] & 0x1F;
        size_t sc2; size_t nxt = next_sc(b, n, nal + 1, &sc2);
        int vcl = (type == 1 || type == 5);
        if (vcl) {
            if (seen_vcl) {                 /* previous unit ends here     */
                starts[nu] = cur_start; is_idr[nu] = (uint8_t)cur_idr; nu++;
                cur_start = i; cur_idr = 0;
            }
            seen_vcl = 1;
            if (type == 5) cur_idr = 1;
        } else if (seen_vcl) {              /* non-VCL after a VCL: new AU */
            starts[nu] = cur_start; is_idr[nu] = (uint8_t)cur_idr; nu++;
            cur_start = i; seen_vcl = 0; cur_idr = 0;
        }
        i = nxt; sc = sc2;
    }
    if (cur_start < n) { starts[nu] = cur_start; is_idr[nu] = (uint8_t)cur_idr; nu++; }
    starts[nu] = n;
    return nu;
}

int gn_dict_learn(gn_dict *d, const uint8_t *text, size_t len, uint32_t max_new) {
    (void)max_new;
    /* OPT: pre-size the blob. Growing 4MB->8->16->32 re-copied the whole
     * frame store on every doubling and dominated learn(). */
    if (d->blob_len + len > d->blob_cap) {
        while (d->blob_len + len > d->blob_cap) d->blob_cap *= 2;
        d->blob = realloc(d->blob, d->blob_cap);
    }
    uint64_t *st = malloc((len / 8 + 64) * sizeof(uint64_t));
    uint8_t  *idr = malloc(len / 8 + 64);
    size_t nu = au_segment(text, len, st, idr);
    uint32_t added = 0;
    for (size_t u = 0; u < nu; u++) {
        size_t l = (size_t)(st[u+1] - st[u]);
        if (!l) continue;
        uint32_t before = d->n_entries;
        dict_add(d, text + st[u], l, idr[u]);
        if (d->n_entries > before) added++;
    }
    free(st); free(idr);
    return (int)added;
}

int gn_dict_train(gn_dict *d, const uint8_t *text, size_t len,
                  uint32_t rounds, uint32_t merges, uint32_t mincnt) {
    (void)rounds; (void)merges; (void)mincnt;
    gn_dict_learn(d, text, len, 0);
    return (int)d->n_entries;
}

size_t gn_tokenize(const gn_dict *d, const uint8_t *text, size_t len,
                   gn_tok *out, size_t cap) {
    uint64_t *st = malloc((len / 8 + 64) * sizeof(uint64_t));
    uint8_t  *idr = malloc(len / 8 + 64);
    size_t nu = au_segment(text, len, st, idr);
    size_t n = 0;
    for (size_t u = 0; u < nu; u++) {
        size_t l = (size_t)(st[u+1] - st[u]);
        if (!l) continue;
        gn_tok t = gn_dict_lookup(d, text + st[u], l);
        if (t == GN_TOK_INVALID) {          /* unlearned frame: byte-escape */
            for (size_t k = 0; k < l; k++) {
                if (n == cap) { free(st); free(idr); return (size_t)-1; }
                out[n++] = GN_BYTE_BASE + text[st[u] + k];
            }
            continue;
        }
        if (n == cap) { free(st); free(idr); return (size_t)-1; }
        out[n++] = t;
    }
    free(st); free(idr);
    return n;
}

size_t gn_detok_len(const gn_dict *d, const gn_tok *toks, size_t n) {
    size_t total = 0, l;
    for (size_t i = 0; i < n; i++) {
        if (GN_TOK_IS_BYTE(toks[i])) { total += 1; continue; }
        gn_dict_text(d, toks[i], &l); total += l;
    }
    return total;
}

size_t gn_detokenize(const gn_dict *d, const gn_tok *toks, size_t n,
                     uint8_t *out, size_t cap) {
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l; const uint8_t *p = gn_dict_text(d, toks[i], &l);
        if (w + l > cap) return w;
        memcpy(out + w, p, l); w += l;
    }
    return w;
}

/* Genna-Net: video frames are stored IN the chunks (as tokens that index the
 * frame blob), so a transmitted chunk carries frame-id tokens. The receiver
 * needs the frame bytes for those ids. For the sync benchmark we install a
 * frame entry at its id. Idempotent. */
void gn_net__install_entry(gn_dict *d, uint32_t id, const uint8_t *txt, uint32_t len){
    if(id==0) return;
    /* ensure capacity */
    while(id > d->cap_entries){ d->cap_entries*=2;
        d->e_off=realloc(d->e_off,d->cap_entries*8);
        d->e_len=realloc(d->e_len,d->cap_entries*4);
        d->e_idr=realloc(d->e_idr,d->cap_entries); }
    if(id > d->n_entries){
        for(uint32_t k=d->n_entries;k<id-1;k++){ d->e_off[k]=0; d->e_len[k]=0; d->e_idr[k]=0; }
        d->n_entries=id;
    }
    if(d->blob_len+len > d->blob_cap){ while(d->blob_len+len>d->blob_cap)d->blob_cap*=2;
        d->blob=realloc(d->blob,d->blob_cap); }
    uint64_t off=d->blob_len; memcpy(d->blob+off,txt,len); d->blob_len+=len;
    d->e_off[id-1]=off; d->e_len[id-1]=len;
    vent *e=tab_slot(d,txt,len);
    if(e->id==GN_TOK_INVALID){ e->off=(uint32_t)off; e->len=len; e->id=id;
        if(d->n_entries*10 > d->tab_cap*7) tab_grow(d); }
}
