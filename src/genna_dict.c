/* genna_dict.c — View 2: the language. Learned word dictionary,
 * fixed-width tokenizer, byte-escape fallback. No dependencies. */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>

/* ---- open-addressing hash: word bytes -> token id -------------------- */

typedef struct { uint32_t text_off, text_len; gn_tok id; } dent;

struct gn_dict {
    /* entry i (token id i+1) owns text at texts[off..off+len) */
    uint8_t  *texts;   size_t texts_len, texts_cap;
    uint32_t *e_off;   uint32_t *e_len;   uint32_t n_entries, cap_entries;
    /* hash table of dents, power-of-two */
    dent     *tab;     uint32_t tab_cap;  /* slots */
};

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static void tab_grow(gn_dict *d);

gn_dict *gn_dict_new(void) {
    gn_dict *d = calloc(1, sizeof *d);
    d->tab_cap = 1u << 16;
    d->tab = calloc(d->tab_cap, sizeof(dent));
    d->cap_entries = 1u << 14;
    d->e_off = malloc(d->cap_entries * 4);
    d->e_len = malloc(d->cap_entries * 4);
    d->texts_cap = 1u << 20; d->texts = malloc(d->texts_cap);
    return d;
}

void gn_dict_free(gn_dict *d) {
    if (!d) return;
    free(d->texts); free(d->e_off); free(d->e_len); free(d->tab); free(d);
}

uint32_t gn_dict_size(const gn_dict *d)    { return d->n_entries; }
uint64_t gn_dict_version(const gn_dict *d) { return d->n_entries; }

static dent *tab_slot(const gn_dict *d, const uint8_t *w, size_t n) {
    uint64_t h = fnv1a(w, n);
    uint32_t mask = d->tab_cap - 1, i = (uint32_t)h & mask;
    for (;;) {
        dent *e = &d->tab[i];
        if (e->id == GN_TOK_INVALID) return e;                 /* empty  */
        if (e->text_len == n &&
            memcmp(d->texts + e->text_off, w, n) == 0) return e; /* hit  */
        i = (i + 1) & mask;
    }
}

gn_tok gn_dict_lookup(const gn_dict *d, const uint8_t *w, size_t n) {
    if (n == 0) return GN_TOK_INVALID;
    dent *e = tab_slot(d, w, n);
    return e->id;   /* GN_TOK_INVALID if absent */
}

static gn_tok dict_add(gn_dict *d, const uint8_t *w, size_t n) {
    dent *e = tab_slot(d, w, n);
    if (e->id != GN_TOK_INVALID) return e->id;
    if (d->n_entries + 1 >= GN_BYTE_BASE - 1) return GN_TOK_INVALID;
    /* store text */
    if (d->texts_len + n > d->texts_cap) {
        while (d->texts_len + n > d->texts_cap) d->texts_cap *= 2;
        d->texts = realloc(d->texts, d->texts_cap);
        /* NOTE: tab entries reference offsets, not pointers — safe.     */
    }
    if (d->n_entries == d->cap_entries) {
        d->cap_entries *= 2;
        d->e_off = realloc(d->e_off, d->cap_entries * 4);
        d->e_len = realloc(d->e_len, d->cap_entries * 4);
    }
    uint32_t off = (uint32_t)d->texts_len;
    memcpy(d->texts + off, w, n);
    d->texts_len += n;
    uint32_t idx = d->n_entries++;
    d->e_off[idx] = off; d->e_len[idx] = (uint32_t)n;
    e->text_off = off; e->text_len = (uint32_t)n; e->id = idx + 1;
    gn_tok new_id = e->id;   /* BUGFIX: tab_grow frees d->tab -> e dangles */
    if (d->n_entries * 10 > d->tab_cap * 7) tab_grow(d);   /* 0.7 load   */
    return new_id;
}

static void tab_grow(gn_dict *d) {
    uint32_t old_cap = d->tab_cap; dent *old = d->tab;
    d->tab_cap *= 2;
    d->tab = calloc(d->tab_cap, sizeof(dent));
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].id == GN_TOK_INVALID) continue;
        dent *e = tab_slot(d, d->texts + old[i].text_off, old[i].text_len);
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
    return d->texts + d->e_off[t-1];
}

/* ---- word segmentation ------------------------------------------------
 * A "word" = maximal run of [A-Za-z0-9_] ; every other byte stands alone.
 * Deterministic, reversible, content-agnostic.                           */
static inline bool wordb(uint8_t c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}

int gn_dict_learn(gn_dict *d, const uint8_t *text, size_t len,
                  uint32_t max_new) {
    uint32_t added = 0; size_t i = 0;
    while (i < len && added < max_new) {
        if (!wordb(text[i])) { i++; continue; }
        size_t j = i; while (j < len && wordb(text[j])) j++;
        if (j - i >= 2) {                    /* 1-byte words: escape is fine */
            gn_tok before = gn_dict_lookup(d, text+i, j-i);
            if (before == GN_TOK_INVALID)
                if (dict_add(d, text+i, j-i) != GN_TOK_INVALID) added++;
            /* also learn "word " — prose is mostly word+space, and one
               token for both halves the stream on typical text          */
            if (j < len && text[j] == ' ' &&
                gn_dict_lookup(d, text+i, j-i+1) == GN_TOK_INVALID)
                if (dict_add(d, text+i, j-i+1) != GN_TOK_INVALID) added++;
        }
        i = j;
    }
    return (int)added;
}

size_t gn_tokenize(const gn_dict *d, const uint8_t *text, size_t len,
                   gn_tok *out, size_t cap) {
    size_t n = 0, i = 0;
    while (i < len) {
        if (wordb(text[i])) {
            size_t j = i; while (j < len && wordb(text[j])) j++;
            /* greedy: "word " first (one token for both), then bare word */
            if (j < len && text[j] == ' ') {
                gn_tok ts = gn_dict_lookup(d, text+i, j-i+1);
                if (ts != GN_TOK_INVALID) {
                    if (n == cap) return (size_t)-1;
                    out[n++] = ts; i = j + 1; continue;
                }
            }
            gn_tok t = gn_dict_lookup(d, text+i, j-i);
            if (t != GN_TOK_INVALID) {
                if (n == cap) return (size_t)-1;
                out[n++] = t; i = j; continue;
            }
            /* unknown word: byte-escape each byte */
            for (; i < j; i++) {
                if (n == cap) return (size_t)-1;
                out[n++] = GN_BYTE_BASE + text[i];
            }
            continue;
        }
        if (n == cap) return (size_t)-1;
        out[n++] = GN_BYTE_BASE + text[i++];
    }
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
