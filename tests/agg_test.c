/* agg_test.c — do node aggregates give the right answer, and are they faster?
 *
 * Two independent things, and the second is worthless without the first:
 *
 *   1. Every range aggregate must equal a brute-force scan of the same bytes.
 *      An annotated tree is easy to make fast and wrong: the annotation is
 *      combined at every path copy, so an off-by-one in the covered/partial
 *      decision produces answers that are plausible and wrong, and only
 *      differ from the truth depending on where the treap happened to split.
 *
 *   2. It must actually be O(log n). The whole justification for widening
 *      every node is that a covered subtree answers in O(1); if the query
 *      still walks the range, the 8 bytes bought nothing.
 *
 * Both are measured against the same engine, on binary data where one token
 * is one byte so brute force is unambiguous.
 */
#include "../include/genna.h"
#include "../include/genna_agg.h"
#include "../include/genna_bin.h"
#include "../include/genna_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c, ...) do { \
    if (c) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static double ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* brute force over the raw bytes, in token space (GN_BYTE_BASE + byte) */
static uint64_t bf_max(const uint8_t *d, size_t a, size_t b) {
    uint64_t m = 0;
    for (size_t i = a; i < b; i++) { uint64_t v = GN_BYTE_BASE + d[i]; if (v > m) m = v; }
    return m;
}
static uint64_t bf_min(const uint8_t *d, size_t a, size_t b) {
    uint64_t m = (uint64_t)-1;
    for (size_t i = a; i < b; i++) { uint64_t v = GN_BYTE_BASE + d[i]; if (v < m) m = v; }
    return m;
}
static uint64_t bf_sum(const uint8_t *d, size_t a, size_t b) {
    uint64_t s = 0;
    for (size_t i = a; i < b; i++) s += GN_BYTE_BASE + d[i];
    return s;
}


/* Model-free brute force: sum the ACTUAL tokens of a version by walking its
 * leaves and reading the store, independent of the node annotations.
 *
 * The byte-model version above (GN_BYTE_BASE + byte) is only valid for data
 * ingested through gn_create_binary. gn_update goes through the text
 * tokenizer, so an edited region can hold dictionary ids instead of byte
 * escapes -- and then the aggregate is still right while a byte model of it
 * is wrong. Checking against the real tokens tests the annotation rather
 * than the tokenizer. */
typedef struct { gn_store *s; uint64_t sum; } wctx;
static void wsum(void *ctx, const gn_extent *x, uint64_t bytes) {
    wctx *w = (wctx *)ctx; (void)bytes;
    size_t cn = 0;
    const gn_tok *t = gn_store_get(w->s, x->chunk, &cn);
    if (!t || (uint64_t)x->off + x->len > cn) return;
    for (uint32_t i = 0; i < x->len; i++) w->sum += t[x->off + i];
}
static uint64_t token_sum(gn_engine *e, gn_object *o, uint32_t v) {
    wctx w = { gn_engine_store(e), 0 };
    gn_ext_walk(o->ver[v].root, wsum, &w);
    return w.sum;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t MB = argc > 1 ? (size_t)atoi(argv[1]) : 8;
    size_t n = MB << 20;

    printf("=== aggregates in the node ===\n");
    printf("   node size: %llu bytes", (unsigned long long)gn_ext_node_size());
#ifdef GN_NODE_AGG
    printf("   (built WITH -DGN_NODE_AGG)\n\n");
#else
    printf("   (built WITHOUT -DGN_NODE_AGG)\n\n");
#endif

    uint8_t *data = malloc(n);
    if (!data) { printf("  cannot allocate %zu MB\n", MB); return 2; }
    uint64_t st = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < n; i++) {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        data[i] = (uint8_t)(st >> 24);
    }
    /* plant known extremes so a monoid that silently returns identity, or
     * one that quietly ignores the tail of a range, cannot pass */
    data[n / 3]     = 0x00;
    data[n / 2]     = 0xFF;
    data[n - 1]     = 0xFE;

    gn_engine *e = gn_engine_new();
    if (gn_agg_attach(e, GN_AGG_MAX) != 0) {
#ifdef GN_NODE_AGG
        printf("  FAIL  gn_agg_attach(MAX)\n");
        return 1;
#else
        /* Not a failure: this build compiled the annotation out, which is
         * the A/B arm. Say so plainly -- printing FAIL here would put the
         * word in a log where nothing is wrong. */
        printf("  n/a   monoid unavailable: built without -DGN_NODE_AGG\n");
        printf("        (this arm exists to report the node size above)\n");
        free(data);
        return 0;
#endif
    }
    gn_bin_opts o; gn_bin_opts_default(&o); o.avg_chunk = 4096;
    gn_object *ob = gn_create_binary(e, "d", data, n, &o);
    CHECK(ob != NULL, "%zu MB ingested with a MAX monoid attached", MB);
    if (!ob) return 1;

    /* ---- 1. correctness against brute force -------------------------- */
    int bad = 0; uint64_t firstbad_got = 0, firstbad_want = 0; size_t bo = 0, bl = 0;
    for (int i = 0; i < 200; i++) {
        size_t a = (size_t)((uint64_t)i * 7919u * 1013u) % (n - 1);
        size_t len = 1 + (size_t)((uint64_t)i * 104729u) % 65536u;
        if (a + len > n) len = n - a;
        uint64_t got  = gn_range_agg_latest(e, ob, a, len);
        uint64_t want = bf_max(data, a, a + len);
        if (got != want) {
            if (!bad) { firstbad_got = got; firstbad_want = want; bo = a; bl = len; }
            bad++;
        }
    }
    CHECK(bad == 0, "200 random MAX ranges match a brute-force scan exactly%s", "");
    if (bad) printf("        first mismatch at [%zu,+%zu): got %llu want %llu\n",
                    bo, bl, (unsigned long long)firstbad_got,
                    (unsigned long long)firstbad_want);

    /* the planted extremes, specifically */
    CHECK(gn_range_agg_latest(e, ob, n / 2, 1) == GN_BYTE_BASE + 0xFF,
          "the planted 0xFF is found by a 1-byte range");
    CHECK(gn_range_agg_latest(e, ob, 0, n) == GN_BYTE_BASE + 0xFF,
          "whole-object MAX is the planted 0xFF");

    /* ---- 2. is it actually O(log n)? --------------------------------- */
    /* A covered subtree answers in O(1), so a 1 MB range and a 64 MB range
     * should cost about the same. Compare against a real read of the range,
     * which is the honest alternative a caller would otherwise write. */
    uint8_t *rb = malloc(1u << 22);
    double t_small, t_big, t_read;
    {
        double t0 = ms();
        for (int k = 0; k < 2000; k++) gn_range_agg_latest(e, ob, 4096, 4096);
        t_small = (ms() - t0) / 2000;
    }
    {
        double t0 = ms();
        for (int k = 0; k < 2000; k++) gn_range_agg_latest(e, ob, 0, n);
        t_big = (ms() - t0) / 2000;
    }
    {
        double t0 = ms();
        for (int k = 0; k < 20; k++) gn_read(e, ob, 0, 1u << 22, rb);
        t_read = (ms() - t0) / 20;
    }
    printf("\n   MAX over 4 KB      : %.4f ms\n", t_small);
    printf("   MAX over %zu MB     : %.4f ms\n", MB, t_big);
    printf("   (for scale) read 4 MB: %.4f ms\n\n", t_read);
    CHECK(t_big < t_small * 8,
          "a whole-object aggregate costs about what a 4 KB one does "
          "(%.4f vs %.4f ms) - the subtree annotation is doing the work",
          t_big, t_small);

    /* ---- 3. the other monoids ---------------------------------------- */
    gn_engine_free(e); gn_ext_arena_free();

    e = gn_engine_new();
    gn_agg_attach(e, GN_AGG_MIN);
    ob = gn_create_binary(e, "d", data, n, &o);
    bad = 0;
    for (int i = 0; i < 100; i++) {
        size_t a = (size_t)((uint64_t)i * 3571u * 2027u) % (n - 1);
        size_t len = 1 + (size_t)((uint64_t)i * 40961u) % 32768u;
        if (a + len > n) len = n - a;
        if (gn_range_agg_latest(e, ob, a, len) != bf_min(data, a, a + len)) bad++;
    }
    CHECK(bad == 0, "100 random MIN ranges match brute force");
    gn_engine_free(e); gn_ext_arena_free();

    e = gn_engine_new();
    gn_agg_attach(e, GN_AGG_SUM);
    ob = gn_create_binary(e, "d", data, n, &o);
    bad = 0;
    for (int i = 0; i < 100; i++) {
        size_t a = (size_t)((uint64_t)i * 6151u * 1543u) % (n - 1);
        size_t len = 1 + (size_t)((uint64_t)i * 24593u) % 32768u;
        if (a + len > n) len = n - a;
        if (gn_range_agg_latest(e, ob, a, len) != bf_sum(data, a, a + len)) bad++;
    }
    CHECK(bad == 0, "100 random SUM ranges match brute force");
    CHECK(gn_range_agg_latest(e, ob, 0, n) == bf_sum(data, 0, n),
          "whole-object SUM matches brute force exactly");

    /* ---- 4. aggregates survive edits (path copy recombines) ----------
     * Checked against the object's real tokens, not against a byte model:
     * gn_update tokenizes through the dictionary, so the edited region may
     * hold dictionary ids rather than byte escapes. An earlier version of
     * this test compared against GN_BYTE_BASE + byte and reported the ENGINE
     * as wrong when the test's model was. */
    CHECK(gn_range_agg_latest(e, ob, 0, ob->ver[ob->n_ver - 1].total_bytes)
              == token_sum(e, ob, ob->n_ver - 1),
          "before the edit: whole-object aggregate == sum of the real tokens");

    uint8_t hi[4]; memset(hi, 0xFF, sizeof hi);
    gn_update(e, ob, n / 4, 4, hi, 4);
    {
        uint32_t lv = ob->n_ver - 1;
        uint64_t tree = gn_range_agg_latest(e, ob, 0, ob->ver[lv].total_bytes);
        uint64_t real = token_sum(e, ob, lv);
        CHECK(tree == real,
              "after an edit, the whole-object aggregate still equals the sum "
              "of the real tokens (tree %llu, tokens %llu, diff %lld)",
              (unsigned long long)tree, (unsigned long long)real,
              (long long)(tree - real));
    }

    /* ---- 5. old versions keep their own aggregates -------------------- */
    CHECK(gn_range_agg(e, ob, 0, 0, n) == token_sum(e, ob, 0),
          "version 0's aggregate is unchanged by the edit (structural sharing)");

    /* ---- 6. aggregates survive a save/open round trip -----------------
     * The loader builds leaves through gn_ext_mk_leaf_p, not mk_leaf. That
     * path did not compute the annotation, so every reopened store had
     * uninitialized aggregates and answered plausibly-wrong numbers. Nothing
     * above this check would have noticed, because nothing above it reopens.
     */
    {
        char sp[1024]; snprintf(sp, sizeof sp, "%s/agg_rt.gn",
                                getenv("GN_AGG_DIR") ? getenv("GN_AGG_DIR") : ".");
        uint32_t lv = ob->n_ver - 1;
        uint64_t before = gn_range_agg_latest(e, ob, 0, ob->ver[lv].total_bytes);
        int srt = gn_save(e, sp);
        CHECK(srt == 0, "store saved for the aggregate round trip");
        gn_engine_free(e); gn_ext_arena_free();

        /* Deliberately attach BEFORE the tree is built by the loader: the
         * monoid resolves chunks through the engine it was attached to, so a
         * monoid left pointing at the freed engine yields identity for every
         * leaf -- which is how this check first came back as a uniform 0. */
        gn_engine *probe = gn_engine_new();
        gn_agg_attach(probe, GN_AGG_SUM);
        gn_engine_free(probe);
        gn_ext_arena_free();

        gn_engine *e2 = gn_open(sp);
        CHECK(e2 != NULL, "store reopens");
        if (e2) {
            /* rebind to the engine that actually owns the chunks */
            gn_agg_attach(e2, GN_AGG_SUM);
            gn_object *o2 = gn_object_open(e2, "d");
            uint32_t v2 = o2->n_ver - 1;
            uint64_t after = gn_range_agg_latest(e2, o2, 0, o2->ver[v2].total_bytes);
            uint64_t real  = token_sum(e2, o2, v2);
            CHECK(after == real,
                  "after gn_open, the whole-object aggregate equals the sum of "
                  "the real tokens (tree %llu, tokens %llu)",
                  (unsigned long long)after, (unsigned long long)real);
            CHECK(after == before,
                  "and it is the same value the store had before saving "
                  "(%llu vs %llu)",
                  (unsigned long long)after, (unsigned long long)before);
            /* a partial range too, so covered-subtree and partial-leaf paths
             * are both exercised against a reloaded tree */
            uint64_t pr = gn_range_agg_latest(e2, o2, n / 8, 1u << 16);
            CHECK(pr != 0, "a partial range over a reopened store is non-zero "
                           "(%llu)", (unsigned long long)pr);
            gn_engine_free(e2); gn_ext_arena_free();
        }
        remove(sp);
        { char w[1100]; snprintf(w, sizeof w, "%s.wal", sp); remove(w); }
    }

    free(rb); free(data);
    printf("\n%s (%d failures)\n",
           fails ? "AGGREGATES: FAILURES" : "AGGREGATES: ALL PASS", fails);
    return fails != 0;
}
