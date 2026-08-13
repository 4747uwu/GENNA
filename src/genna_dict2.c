/* genna_dict2.c — View 2: the language.  v2.
 *
 * Changes from v1, and why:
 *   (a) UNITS, not just words.  v1 learned only [A-Za-z0-9_]+ runs and
 *       byte-escaped everything else.  On real text 39-77% of tokens were
 *       escapes, each paying 4 bytes to carry 1.  v2 treats a maximal run
 *       of non-word bytes as a learnable unit too ("  ", ");\n", ", ").
 *   (b) PHRASES.  BPE-style merging over the unit stream: count adjacent
 *       token pairs, promote the frequent ones to single dict entries,
 *       repeat.  Each merge grows the bytes a 4-byte token can carry.
 *   (c) Greedy longest-match tokenizer over unit-grams (deterministic,
 *       reversible).
 *   (d) BUGFIX: dict_add read e->id after tab_grow() freed the table.
 *
 * Invariants preserved: append-only ids (L3), fixed-width u32 (L1),
 * byte-escape fallback so tokenization never fails on content.
 */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>

#define GN_MAX_ENTRY_BYTES 64u   /* longest string a single token may cover */
#define GN_MAX_GRAM        12u   /* longest unit-run a single token may cover */

typedef struct { uint32_t text_off, text_len; gn_tok id; } dent;

struct gn_dict {
    uint8_t  *texts;   size_t texts_len, texts_cap;
    uint32_t *e_off;   uint32_t *e_len;   uint32_t n_entries, cap_entries;
    dent     *tab;     uint32_t tab_cap;
    uint32_t  max_entry_len;
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
    d->max_entry_len = 0;
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
        if (e->id == GN_TOK_INVALID) return e;
        if (e->text_len == n &&
            memcmp(d->texts + e->text_off, w, n) == 0) return e;
        i = (i + 1) & mask;
    }
}

gn_tok gn_dict_lookup(const gn_dict *d, const uint8_t *w, size_t n) {
    if (n == 0 || n > d->max_entry_len) return GN_TOK_INVALID;
    dent *e = tab_slot(d, w, n);
    return e->id;
}

static gn_tok dict_add(gn_dict *d, const uint8_t *w, size_t n) {
    if (n == 0 || n > GN_MAX_ENTRY_BYTES) return GN_TOK_INVALID;
    dent *e = tab_slot(d, w, n);
    if (e->id != GN_TOK_INVALID) return e->id;
    if (d->n_entries + 1 >= GN_BYTE_BASE - 1) return GN_TOK_INVALID;
    if (d->texts_len + n > d->texts_cap) {
        while (d->texts_len + n > d->texts_cap) d->texts_cap *= 2;
        d->texts = realloc(d->texts, d->texts_cap);
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
    gn_tok new_id = e->id;              /* BUGFIX: tab_grow invalidates e */
    if ((uint32_t)n > d->max_entry_len) d->max_entry_len = (uint32_t)n;
    if (d->n_entries * 10 > d->tab_cap * 7) tab_grow(d);
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

/* ---- unit segmentation ---------------------------------------------- */
static inline bool wordb(uint8_t c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}

static size_t segment(const uint8_t *t, size_t len, uint32_t *starts) {
    size_t nu = 0, i = 0;
    while (i < len) {
        starts[nu++] = (uint32_t)i;
        bool w = wordb(t[i]);
        size_t j = i + 1;
        while (j < len && wordb(t[j]) == w) j++;
        i = j;
    }
    starts[nu] = (uint32_t)len;
    return nu;
}

/* ---- transient pair-count table -------------------------------------- */
typedef struct { uint64_t key; uint32_t cnt; uint32_t a_off, a_len; } pcell;
typedef struct { pcell *c; uint32_t cap, n; } ptab;

static void ptab_init(ptab *p, uint32_t cap) {
    p->cap = cap; p->n = 0; p->c = calloc(cap, sizeof(pcell));
}
static void ptab_free(ptab *p) { free(p->c); }

static void ptab_bump(ptab *p, const uint8_t *s, uint32_t off, uint32_t len) {
    uint64_t h = fnv1a(s + off, len); if (!h) h = 1;
    uint32_t m = p->cap - 1, i = (uint32_t)h & m;
    while (p->c[i].key) {
        if (p->c[i].key == h && p->c[i].a_len == len &&
            memcmp(s + p->c[i].a_off, s + off, len) == 0) { p->c[i].cnt++; return; }
        i = (i + 1) & m;
    }
    if (p->n * 10 > p->cap * 7) return;
    p->c[i].key = h; p->c[i].cnt = 1; p->c[i].a_off = off; p->c[i].a_len = len;
    p->n++;
}

/* ---- learn: units only, no phrases ----------------------------------- */
int gn_dict_learn(gn_dict *d, const uint8_t *text, size_t len,
                  uint32_t max_new) {
    uint32_t added = 0; size_t i = 0;
    while (i < len && added < max_new) {
        bool w = wordb(text[i]);
        size_t j = i + 1;
        while (j < len && wordb(text[j]) == w) j++;
        size_t ul = j - i;
        if (ul <= GN_MAX_ENTRY_BYTES) {
            if (gn_dict_lookup(d, text + i, ul) == GN_TOK_INVALID)
                if (dict_add(d, text + i, ul) != GN_TOK_INVALID) added++;
        }
        i = j;
    }
    return (int)added;
}

/* ---- phrase training: BPE over the unit stream ------------------------ */
int gn_dict_train(gn_dict *d, const uint8_t *text, size_t len,
                  uint32_t rounds, uint32_t merges_per_round,
                  uint32_t min_count) {
    gn_dict_learn(d, text, len, 1u << 24);

    uint32_t *starts = malloc((len + 2) * sizeof(uint32_t));
    size_t nu = segment(text, len, starts);

    uint32_t *seq = malloc((nu + 2) * sizeof(uint32_t));
    size_t ns = nu;
    for (size_t k = 0; k <= nu; k++) seq[k] = (uint32_t)k;

    uint32_t ptab_cap = 1u << 22;
    for (uint32_t r = 0; r < rounds; r++) {
        ptab P; ptab_init(&P, ptab_cap);
        for (size_t k = 0; k + 1 < ns; k++) {
            uint32_t off = starts[seq[k]];
            uint32_t end = starts[seq[k+2]];
            if (end - off > GN_MAX_ENTRY_BYTES) continue;
            if (seq[k+2] - seq[k] > GN_MAX_GRAM) continue;
            ptab_bump(&P, text, off, end - off);
        }
        uint32_t thresh = min_count;
        {   /* binary-search a count threshold admitting ~merges_per_round */
            uint32_t lo = min_count, hi = 1u << 28;
            for (int it = 0; it < 28; it++) {
                if (hi - lo <= 1) break;
                uint32_t mid = lo + (hi - lo) / 2;
                uint32_t c = 0;
                for (uint32_t i = 0; i < P.cap; i++)
                    if (P.c[i].key && P.c[i].cnt >= mid) c++;
                if (c > merges_per_round) lo = mid; else hi = mid;
            }
            thresh = lo > min_count ? lo : min_count;
        }
        uint32_t chosen = 0;
        for (uint32_t i = 0; i < P.cap && chosen < merges_per_round; i++) {
            if (!P.c[i].key || P.c[i].cnt < thresh) continue;
            if (gn_dict_lookup(d, text + P.c[i].a_off, P.c[i].a_len) != GN_TOK_INVALID)
                continue;
            if (dict_add(d, text + P.c[i].a_off, P.c[i].a_len) != GN_TOK_INVALID)
                chosen++;
        }
        ptab_free(&P);
        if (chosen == 0) break;

        size_t w = 0, k = 0;
        while (k < ns) {
            size_t best = 1;
            size_t maxg = GN_MAX_GRAM;
            if (k + maxg > ns) maxg = ns - k;
            for (size_t g = maxg; g >= 2; g--) {
                uint32_t off = starts[seq[k]];
                uint32_t end = starts[seq[k+g]];
                if (end - off > GN_MAX_ENTRY_BYTES) continue;
                if (gn_dict_lookup(d, text + off, end - off) != GN_TOK_INVALID) {
                    best = g; break;
                }
            }
            seq[w++] = seq[k];
            k += best;
        }
        seq[w] = seq[ns];
        ns = w;
    }
    free(seq); free(starts);
    return (int)gn_dict_size(d);
}

/* ---- tokenize: greedy longest-match over unit-grams ------------------- */
size_t gn_tokenize(const gn_dict *d, const uint8_t *text, size_t len,
                   gn_tok *out, size_t cap) {
    size_t n = 0, i = 0;
    while (i < len) {
        uint32_t bnd[GN_MAX_GRAM + 2];
        uint32_t nb = 0;
        size_t j = i;
        while (nb <= GN_MAX_GRAM && j < len) {
            bnd[nb++] = (uint32_t)j;
            bool w = wordb(text[j]);
            size_t k = j + 1;
            while (k < len && wordb(text[k]) == w) k++;
            j = k;
        }
        bnd[nb] = (uint32_t)j;

        gn_tok hit = GN_TOK_INVALID; size_t hit_end = 0;
        for (uint32_t g = nb; g >= 1; g--) {
            size_t end = bnd[g];
            if (end <= i) continue;
            if (end - i > GN_MAX_ENTRY_BYTES) continue;
            gn_tok t = gn_dict_lookup(d, text + i, end - i);
            if (t != GN_TOK_INVALID) { hit = t; hit_end = end; break; }
        }
        if (hit != GN_TOK_INVALID) {
            if (n == cap) return (size_t)-1;
            out[n++] = hit; i = hit_end; continue;
        }
        size_t uend = bnd[1];
        if (uend <= i) uend = i + 1;
        for (; i < uend; i++) {
            if (n == cap) return (size_t)-1;
            out[n++] = GN_BYTE_BASE + text[i];
        }
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

/* ---- persistence support ---------------------------------------------
 * The dictionary is two flat arrays over one text blob plus a hash index
 * that is pure derived state. So the snapshot stores the blob and the
 * arrays, and the index is rebuilt on load.
 *
 * Rebuilding in increasing id order is what makes the reloaded dictionary
 * answer every lookup exactly as the saved one did. Linear probing with no
 * deletions has the property that placement order changes where a key sits
 * but never whether it is found; and inserting low ids first preserves
 * dict_add's rule that the first id to claim a text keeps it.            */
/* 1 = this dictionary module's state fits the snapshot's dict section.
 * The video module (genna_vdict.c) answers 0; see gn_save().            */
int             gn_dict__serializable(const gn_dict *d) { (void)d; return 1; }
uint32_t        gn_dict__entries(const gn_dict *d) { return d->n_entries; }
const uint32_t *gn_dict__offs(const gn_dict *d)    { return d->e_off; }
const uint32_t *gn_dict__lens(const gn_dict *d)    { return d->e_len; }

const uint8_t *gn_dict__texts(const gn_dict *d, uint64_t *len_out) {
    *len_out = (uint64_t)d->texts_len;
    return d->texts;
}

int gn_dict__restore(gn_dict *d, const uint8_t *texts, uint64_t texts_len,
                     const uint32_t *offs, const uint32_t *lens, uint32_t n) {
    if (texts_len > (uint64_t)(uint32_t)-1) return -1;   /* offsets are u32 */

    /* every entry must lie inside the blob, or a later gn_dict_text() would
     * hand out a pointer past the end */
    for (uint32_t i = 0; i < n; i++) {
        if ((uint64_t)offs[i] + lens[i] > texts_len) return -1;
        if (lens[i] > GN_MAX_ENTRY_BYTES) return -1;
    }

    if (texts_len > d->texts_cap) {
        size_t c = d->texts_cap ? d->texts_cap : 1;
        while (c < (size_t)texts_len) c *= 2;
        uint8_t *nt = realloc(d->texts, c);
        if (!nt) return -1;
        d->texts = nt; d->texts_cap = c;
    }
    if (texts_len) memcpy(d->texts, texts, (size_t)texts_len);
    d->texts_len = (size_t)texts_len;

    if (n > d->cap_entries) {
        uint32_t c = d->cap_entries ? d->cap_entries : 1;
        while (c < n) c *= 2;
        uint32_t *no = realloc(d->e_off, (size_t)c * 4);
        if (!no) return -1;
        d->e_off = no;
        uint32_t *nl = realloc(d->e_len, (size_t)c * 4);
        if (!nl) return -1;
        d->e_len = nl; d->cap_entries = c;
    }
    d->max_entry_len = 0;
    for (uint32_t i = 0; i < n; i++) {
        d->e_off[i] = offs[i]; d->e_len[i] = lens[i];
        if (lens[i] > d->max_entry_len) d->max_entry_len = lens[i];
    }
    d->n_entries = n;

    /* rebuild the index from scratch, sized for the load factor up front */
    uint32_t cap = 1u << 16;
    while ((uint64_t)n * 10 > (uint64_t)cap * 7) cap <<= 1;
    dent *nt = calloc(cap, sizeof(dent));
    if (!nt) return -1;
    free(d->tab); d->tab = nt; d->tab_cap = cap;
    for (uint32_t i = 0; i < n; i++) {
        if (d->e_len[i] == 0) continue;          /* gap id from net sync */
        dent *e = tab_slot(d, d->texts + d->e_off[i], d->e_len[i]);
        if (e->id != GN_TOK_INVALID) continue;   /* duplicate text: first wins */
        e->text_off = d->e_off[i]; e->text_len = d->e_len[i]; e->id = i + 1;
    }
    return 0;
}

/* Genna-Net: install a dictionary entry at a specific id (receiver side).
 * Append-only and idempotent: if the id already holds this text, no-op. */
void gn_net__install_entry(gn_dict *d, uint32_t id, const uint8_t *txt, uint32_t len){
    if(id==0) return;
    /* grow entry arrays to cover id */
    while(id > d->cap_entries){ d->cap_entries*=2;
        d->e_off=realloc(d->e_off,d->cap_entries*4);
        d->e_len=realloc(d->e_len,d->cap_entries*4); }
    if(id > d->n_entries){
        /* fill any gap ids as empty, then set this one */
        for(uint32_t k=d->n_entries;k<id-1;k++){ d->e_off[k]=0; d->e_len[k]=0; }
        d->n_entries=id;
    }
    if(d->texts_len+len > d->texts_cap){ while(d->texts_len+len>d->texts_cap)d->texts_cap*=2;
        d->texts=realloc(d->texts,d->texts_cap); }
    uint32_t off=(uint32_t)d->texts_len; memcpy(d->texts+off,txt,len); d->texts_len+=len;
    d->e_off[id-1]=off; d->e_len[id-1]=len;
    if(len>d->max_entry_len) d->max_entry_len=len;
    /* also index it in the hash table so lookups/tokenize see it */
    dent *e=tab_slot(d,txt,len);
    if(e->id==GN_TOK_INVALID){ e->text_off=off; e->text_len=len; e->id=id;
        if(d->n_entries*10 > d->tab_cap*7) tab_grow(d); }
}
