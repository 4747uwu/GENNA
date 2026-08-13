/* bin_test.c — did content-defined chunking actually fix anything?
 *
 * A/B on real scanned geometry. Same data, same edits, only the chunking
 * strategy differs, so any change in sharing is caused by the thing under
 * test and nothing else.
 *
 * Workloads, chosen because they are what 3D pipelines actually do:
 *   move      transform a contiguous region      (artist edits a limb)
 *   insert    add vertices mid-stream            (remesh / splat densify)
 *   delete    remove vertices mid-stream         (splat pruning)
 *   jitter    perturb every vertex slightly      (a training step)
 *
 * The jitter row is expected to stay bad. Every vertex genuinely changed;
 * no chunking scheme can share data that does not exist. It is measured
 * anyway so the claim cannot quietly drift.
 */
#include "../include/genna.h"
#include "../include/genna_bin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c, ...) do { \
    if (c) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static uint64_t RS = 99194853094755497ULL;
static double rnd01(void) {
    RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17;
    return (double)((RS >> 11) & 0xFFFFFFFFFFFFFULL) / (double)(1ULL << 52);
}

/* ---- mesh ------------------------------------------------------------ */
typedef struct { float *xyz; uint32_t n; } mesh;

static int load_obj(const char *path, mesh *m) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 65536, n = 0;
    float *v = malloc(cap * 3 * sizeof(float));
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'v' || (line[1] != ' ' && line[1] != '\t')) continue;
        double x, y, z;
        if (sscanf(line + 1, "%lf %lf %lf", &x, &y, &z) != 3) continue;
        if (n == cap) { cap *= 2; v = realloc(v, cap * 3 * sizeof(float)); }
        v[n*3] = (float)x; v[n*3+1] = (float)y; v[n*3+2] = (float)z; n++;
    }
    fclose(f);
    if (!n) { free(v); return -1; }
    m->xyz = v; m->n = (uint32_t)n;
    return 0;
}

static void morton_sort(mesh *m) {
    uint32_t *ord = malloc((size_t)m->n * sizeof(uint32_t));
    if (gn_morton_order(m->xyz, m->n, ord) != 0) { free(ord); return; }
    float *out = malloc((size_t)m->n * 3 * sizeof(float));
    for (uint32_t i = 0; i < m->n; i++)
        memcpy(out + i * 3, m->xyz + (size_t)ord[i] * 3, 3 * sizeof(float));
    free(m->xyz); free(ord);
    m->xyz = out;
}

/* ---- workloads: produce v2 from v1 ----------------------------------- */
static void wl_move(const mesh *a, mesh *b) {
    b->n = a->n;
    b->xyz = malloc((size_t)a->n * 3 * sizeof(float));
    memcpy(b->xyz, a->xyz, (size_t)a->n * 3 * sizeof(float));
    uint32_t lo = a->n / 2, hi = lo + a->n / 100;
    for (uint32_t v = lo; v < hi && v < a->n; v++)
        for (int k = 0; k < 3; k++) b->xyz[v*3+k] += 0.01f;
}
static void wl_insert(const mesh *a, mesh *b) {
    uint32_t add = a->n / 100;                 /* 1% more points */
    b->n = a->n + add;
    b->xyz = malloc((size_t)b->n * 3 * sizeof(float));
    uint32_t at = a->n / 2;
    memcpy(b->xyz, a->xyz, (size_t)at * 3 * sizeof(float));
    for (uint32_t i = 0; i < add; i++)
        for (int k = 0; k < 3; k++)
            b->xyz[(at + i) * 3 + k] = a->xyz[(at + i) * 3 + k] + 0.5f;
    memcpy(b->xyz + (size_t)(at + add) * 3, a->xyz + (size_t)at * 3,
           (size_t)(a->n - at) * 3 * sizeof(float));
}
static void wl_delete(const mesh *a, mesh *b) {
    uint32_t del = a->n / 100;
    b->n = a->n - del;
    b->xyz = malloc((size_t)b->n * 3 * sizeof(float));
    uint32_t at = a->n / 2;
    memcpy(b->xyz, a->xyz, (size_t)at * 3 * sizeof(float));
    memcpy(b->xyz + (size_t)at * 3, a->xyz + (size_t)(at + del) * 3,
           (size_t)(a->n - at - del) * 3 * sizeof(float));
}
static void wl_jitter(const mesh *a, mesh *b) {
    b->n = a->n;
    b->xyz = malloc((size_t)a->n * 3 * sizeof(float));
    for (size_t i = 0; i < (size_t)a->n * 3; i++)
        b->xyz[i] = (float)((double)a->xyz[i] + (rnd01() * 2 - 1) * 1e-5);
}

/* ---- measure --------------------------------------------------------- */
/* Ingest v1 then v2 into one engine; report what fraction of v2's chunks the
 * store already had. */
static double share_of_v2(const mesh *a, const mesh *b, int fixed,
                          uint32_t avg, uint64_t *v2_chunks_out,
                          uint64_t *dedup_out) {
    gn_engine *e = gn_engine_new();
    gn_bin_opts o; gn_bin_opts_default(&o);
    o.avg_chunk = avg; o.min_chunk = avg / 4; o.max_chunk = avg * 4;
    o.fixed = fixed;

    gn_object *oa = gn_create_binary(e, "a", (const uint8_t *)a->xyz,
                                     (size_t)a->n * 12, &o);
    uint64_t after_a = gn_store_chunks(gn_engine_store(e));
    gn_stats s1; gn_engine_stats(e, &s1);

    gn_object *ob = gn_create_binary(e, "b", (const uint8_t *)b->xyz,
                                     (size_t)b->n * 12, &o);
    gn_stats s2; gn_engine_stats(e, &s2);
    uint64_t new_chunks = gn_store_chunks(gn_engine_store(e)) - after_a;
    uint64_t dedup = s2.chunks_deduped - s1.chunks_deduped;

    uint32_t v2_leaves = gn_bin_chunk_count(ob, (uint32_t)-1);
    (void)oa;
    double share = (v2_leaves == 0) ? 0.0
                 : (double)(v2_leaves - new_chunks) / (double)v2_leaves;
    if (share < 0) share = 0;
    if (v2_chunks_out) *v2_chunks_out = v2_leaves;
    if (dedup_out) *dedup_out = dedup;

    gn_engine_free(e);
    gn_ext_arena_free();
    return share;
}

/* round-trip check: binary objects must read back exactly */
static void test_roundtrip(const mesh *m) {
    printf("\n-- binary ingest is byte-exact --\n");
    gn_engine *e = gn_engine_new();
    size_t nb = (size_t)m->n * 12;
    gn_object *o = gn_create_binary(e, "rt", (const uint8_t *)m->xyz, nb, NULL);
    CHECK(o != NULL, "gn_create_binary returned an object");
    uint8_t *buf = malloc(nb + 16);
    size_t got = gn_read(e, o, 0, nb, buf);
    CHECK(got == nb && memcmp(buf, m->xyz, nb) == 0,
          "%zu bytes read back byte-identical", nb);

    /* and a mid-range read */
    size_t off = nb / 3, len = 4096 < nb ? 4096 : nb;
    size_t g2 = gn_read(e, o, off, len, buf);
    CHECK(g2 == len && memcmp(buf, (const uint8_t *)m->xyz + off, len) == 0,
          "mid-range read at %zu matches", off);
    free(buf);
    gn_engine_free(e);
    gn_ext_arena_free();
}

typedef void (*wlfn)(const mesh *, mesh *);

static void run_mesh(const char *path) {
    mesh m;
    if (load_obj(path, &m) != 0) { printf("\n### SKIP %s\n", path); return; }
    printf("\n==================================================================\n");
    printf("### %s — %u vertices, %.2f MB\n", path, m.n, m.n * 12 / 1048576.0);
    printf("==================================================================\n");

    test_roundtrip(&m);

    /* Morton-order once: a "localized" edit is only localized in the byte
     * stream if the byte stream follows space. */
    morton_sort(&m);

    struct { const char *name; wlfn fn; } wls[] = {
        { "move 1% region",  wl_move   },
        { "insert 1% verts", wl_insert },
        { "delete 1% verts", wl_delete },
        { "jitter ALL",      wl_jitter },
    };

    printf("\n-- fixed chunking vs content-defined (share of v2 reused) --\n");
    printf("   %-18s %10s %10s %10s\n", "workload", "fixed", "CDC", "change");
    double fixed_ins = 0, cdc_ins = 0, cdc_mv = 0;
    double fixed_del = 0, cdc_del = 0, cdc_jit = 0;
    for (int i = 0; i < 4; i++) {
        mesh b;
        wls[i].fn(&m, &b);
        uint64_t c1, d1, c2, d2;
        double sf = share_of_v2(&m, &b, 1, 4096, &c1, &d1);
        double sc = share_of_v2(&m, &b, 0, 4096, &c2, &d2);
        printf("   %-18s %9.1f%% %9.1f%%   %+8.1f pts\n",
               wls[i].name, sf * 100, sc * 100, (sc - sf) * 100);
        if (i == 0) cdc_mv = sc;
        if (i == 1) { fixed_ins = sf; cdc_ins = sc; }
        if (i == 2) { fixed_del = sf; cdc_del = sc; }
        if (i == 3) cdc_jit = sc;
        free(b.xyz);
    }

    /* Stated as levels rather than as a delta: what matters is that fixed
     * chunking loses roughly half the file and CDC keeps nearly all of it. */
    CHECK(fixed_ins < 0.6 && cdc_ins > 0.9,
          "INSERT: fixed chunking keeps only %.1f%% (everything after the "
          "insertion point shifts); CDC keeps %.1f%%",
          fixed_ins * 100, cdc_ins * 100);
    CHECK(fixed_del < 0.6 && cdc_del > 0.9,
          "DELETE: fixed %.1f%% -> CDC %.1f%%", fixed_del * 100, cdc_del * 100);
    CHECK(cdc_mv > 0.9,
          "MOVE: a 1%% localized edit reuses %.1f%% of the mesh "
          "(the text path managed ~67%% because its shared dictionary "
          "retokenized identical bytes)", cdc_mv * 100);
    /* The honest negative, asserted so it cannot quietly change: */
    CHECK(cdc_jit < 0.05,
          "JITTER: still %.1f%% - every vertex genuinely changed, and no "
          "chunking scheme can share data that does not exist", cdc_jit * 100);

    /* chunk size sweep: the (1-a/2)^C rule says survival is exponential in
     * chunk size, so this is the knob that matters most. */
    printf("\n-- chunk size vs sharing (insert workload, CDC) --\n");
    printf("   %-12s %10s %10s\n", "avg chunk", "share", "chunks in v2");
    {
        mesh b; wl_insert(&m, &b);
        for (uint32_t avg = 1024; avg <= 16384; avg *= 2) {
            uint64_t c, d;
            double s = share_of_v2(&m, &b, 0, avg, &c, &d);
            printf("   %-12u %9.1f%% %10llu\n", avg, s * 100,
                   (unsigned long long)c);
        }
        free(b.xyz);
    }
    free(m.xyz);
}

int main(int argc, char **argv) {
    printf("=== GENNA binary path: content-defined chunking for geometry ===\n");
    if (argc > 1) {
        for (int i = 1; i < argc; i++) run_mesh(argv[i]);
    } else {
        run_mesh("corpora/3d/stanford-bunny.obj");
        run_mesh("corpora/3d/armadillo.obj");
    }
    printf("\n%s (%d failures)\n",
           fails ? "BINARY PATH: FAILURES" : "BINARY PATH: ALL PASS", fails);
    return fails != 0;
}
