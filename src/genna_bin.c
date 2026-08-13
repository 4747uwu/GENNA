/* genna_bin.c — binary objects with content-defined chunking.
 *
 * Why this exists. The text path does two things that are right for prose and
 * wrong for geometry:
 *
 *   1. It LEARNS. gn_create() calls gn_dict_learn() on its input, and the
 *      dictionary is shared and append-only. So ingesting mesh B after mesh A
 *      can tokenize byte sequences that are identical in both differently,
 *      because B's learning added entries that change greedy longest-match.
 *      Measured: a 1% localized edit shared only ~67% of chunks when it
 *      should have shared ~99%.
 *
 *   2. It chunks at FIXED token counts. Insert one vertex and every boundary
 *      after it shifts, so every downstream chunk hashes differently and
 *      nothing dedups. Adding and pruning points is what Gaussian-splat
 *      training does continuously, so this is the common case, not an edge
 *      case.
 *
 * This path fixes both: tokenization is the identity map (one byte, one
 * byte-escape token, no learning, so identical bytes always tokenize
 * identically), and boundaries are chosen by the CONTENT via a gear-hash
 * rolling window, so an insertion perturbs only the chunk containing it.
 *
 * The cost, stated plainly: a byte becomes a 4-byte token, so a binary object
 * costs 4x its size in the token store. That is the existing representation,
 * not something added here, but it is why this is a separate entry point and
 * not the default.
 */
#include "../include/genna.h"
#include "../include/genna_bin.h"

#include <stdlib.h>
#include <string.h>

/* engine-internal, from genna_engine3.c */
void gn_engine__attach(gn_engine *e, gn_object *o);

/* ---- gear hash ------------------------------------------------------- */
/* A rolling hash whose value depends only on the last ~32 bytes, so a cut
 * point is a property of the content around it rather than of the distance
 * from the start of the file. Table is a fixed SplitMix64 expansion: it must
 * be identical in every build or two processes would chunk differently and
 * share nothing. */
static uint64_t g_gear[256];
static int      g_gear_ready = 0;

static void gear_init(void) {
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < 256; i++) {
        x += 0x9E3779B97F4A7C15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        g_gear[i] = z ^ (z >> 31);
    }
    g_gear_ready = 1;
}

/* Next cut point at or before `max`, starting from `pos`. */
static size_t next_cut(const uint8_t *p, size_t n, size_t pos,
                       uint32_t min_sz, uint32_t avg_sz, uint32_t max_sz) {
    if (n - pos <= min_sz) return n;
    size_t limit = pos + max_sz; if (limit > n) limit = n;
    size_t i = pos + min_sz;                    /* never cut below min */
    uint64_t mask = ((uint64_t)avg_sz - 1);
    /* round mask up to a power of two minus one */
    mask |= mask >> 1; mask |= mask >> 2; mask |= mask >> 4;
    mask |= mask >> 8; mask |= mask >> 16; mask |= mask >> 32;

    uint64_t h = 0;
    for (; i < limit; i++) {
        h = (h << 1) + g_gear[p[i]];
        if ((h & mask) == 0) return i + 1;
    }
    return limit;
}

/* ---- public ---------------------------------------------------------- */

gn_object *gn_create_binary(gn_engine *e, const char *name,
                            const uint8_t *data, size_t len,
                            const gn_bin_opts *opts) {
    if (!e || !name) return NULL;
    if (!g_gear_ready) gear_init();

    gn_bin_opts o;
    if (opts) o = *opts;
    else      gn_bin_opts_default(&o);
    if (o.avg_chunk < 64)  o.avg_chunk = 64;
    if (o.min_chunk == 0)  o.min_chunk = o.avg_chunk / 4;
    if (o.max_chunk == 0)  o.max_chunk = o.avg_chunk * 4;
    if (o.min_chunk >= o.max_chunk) o.min_chunk = o.max_chunk / 4;

    gn_object *ob = calloc(1, sizeof *ob);
    if (!ob) return NULL;
    strncpy(ob->name, name, 63);
    ob->ver = calloc(1, sizeof(gn_version));
    if (!ob->ver) { free(ob); return NULL; }
    ob->n_ver = 1;

    gn_enode *root = NULL;
    if (len > 0) {
        size_t cap = len / (o.avg_chunk ? o.avg_chunk : 1) + 8;
        gn_extent *ex = malloc(cap * sizeof(gn_extent));
        uint64_t  *bl = malloc(cap * sizeof(uint64_t));
        gn_tok    *tk = malloc((size_t)o.max_chunk * sizeof(gn_tok));
        if (!ex || !bl || !tk) { free(ex); free(bl); free(tk);
                                 free(ob->ver); free(ob); return NULL; }
        uint32_t k = 0;
        size_t pos = 0;
        while (pos < len) {
            size_t cut = o.fixed
                       ? (pos + o.avg_chunk < len ? pos + o.avg_chunk : len)
                       : next_cut(data, len, pos, o.min_chunk, o.avg_chunk,
                                  o.max_chunk);
            size_t clen = cut - pos;
            /* identity tokenization: no dictionary, no learning, so the same
             * bytes always produce the same tokens in every object */
            for (size_t i = 0; i < clen; i++)
                tk[i] = GN_BYTE_BASE + data[pos + i];
            gn_cid id = gn_store_put(gn_engine_store(e), tk, clen);
            if (k == cap) {
                cap *= 2;
                gn_extent *ne = realloc(ex, cap * sizeof(gn_extent));
                uint64_t  *nb = realloc(bl, cap * sizeof(uint64_t));
                if (!ne || !nb) { free(ne ? ne : ex); free(nb ? nb : bl);
                                  free(tk); free(ob->ver); free(ob); return NULL; }
                ex = ne; bl = nb;
            }
            ex[k] = (gn_extent){ id, 0, (uint32_t)clen };
            bl[k] = clen;                 /* 1 byte-escape token == 1 byte */
            k++;
            pos = cut;
        }
        root = gn_ext_build(ex, bl, k);
        free(ex); free(bl); free(tk);
    }

    ob->ver[0].root = root;
    ob->ver[0].ext = NULL;
    ob->ver[0].n_ext = 0;
    ob->ver[0].total_tokens = gn_ext_tokens(root);
    ob->ver[0].total_bytes  = gn_ext_bytes(root);
    ob->ver[0].dict_version = 0;          /* no dictionary is pinned */

    gn_engine__attach(e, ob);
    return ob;
}

void gn_bin_opts_default(gn_bin_opts *o) {
    memset(o, 0, sizeof *o);
    o->avg_chunk = 4096;
    o->min_chunk = 1024;
    o->max_chunk = 16384;
    o->fixed     = 0;
}

uint32_t gn_bin_chunk_count(const gn_object *o, uint32_t version) {
    if (!o || o->n_ver == 0) return 0;
    uint32_t v = (version == (uint32_t)-1) ? o->n_ver - 1 : version;
    if (v >= o->n_ver) return 0;
    return gn_ext_leaves(o->ver[v].root);
}

/* ---- Morton (Z-order) reordering ------------------------------------- */
/* Sorting vertices by interleaved bits of their quantized coordinates makes
 * points that are near each other in space near each other in the byte
 * stream. Without it, an edit confined to one region of the model can touch
 * bytes scattered through the whole file, and no chunking scheme can help. */

static uint64_t part1by2(uint32_t v) {          /* spread 21 bits, 3-apart */
    uint64_t x = v & 0x1FFFFFULL;
    x = (x | (x << 32)) & 0x1F00000000FFFFULL;
    x = (x | (x << 16)) & 0x1F0000FF0000FFULL;
    x = (x | (x <<  8)) & 0x100F00F00F00F00FULL;
    x = (x | (x <<  4)) & 0x10C30C30C30C30C3ULL;
    x = (x | (x <<  2)) & 0x1249249249249249ULL;
    return x;
}

typedef struct { uint64_t key; uint32_t idx; } mkey;

static int mkey_cmp(const void *a, const void *b) {
    const mkey *x = a, *y = b;
    if (x->key < y->key) return -1;
    if (x->key > y->key) return 1;
    return (x->idx < y->idx) ? -1 : (x->idx > y->idx);
}

int gn_morton_order(const float *xyz, uint32_t n, uint32_t *order_out) {
    if (!xyz || !n || !order_out) return -1;
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            float c = xyz[i * 3 + k];
            if (c < lo[k]) lo[k] = c;
            if (c > hi[k]) hi[k] = c;
        }
    double span[3];
    for (int k = 0; k < 3; k++) {
        span[k] = (double)hi[k] - (double)lo[k];
        if (span[k] <= 0.0) span[k] = 1.0;
    }
    mkey *keys = malloc((size_t)n * sizeof(mkey));
    if (!keys) return -1;
    const uint32_t GRID = (1u << 21) - 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t q[3];
        for (int k = 0; k < 3; k++) {
            double t = ((double)xyz[i * 3 + k] - (double)lo[k]) / span[k];
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            q[k] = (uint32_t)(t * (double)GRID);
        }
        keys[i].key = part1by2(q[0]) | (part1by2(q[1]) << 1) | (part1by2(q[2]) << 2);
        keys[i].idx = i;
    }
    qsort(keys, n, sizeof(mkey), mkey_cmp);
    for (uint32_t i = 0; i < n; i++) order_out[i] = keys[i].idx;
    free(keys);
    return 0;
}
