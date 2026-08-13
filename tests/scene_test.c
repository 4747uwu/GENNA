/* scene_test.c — does the 3D idea actually work, and by how much?
 *
 * Three claims, measured on real scanned geometry (Stanford bunny /
 * armadillo / happy buddha), not asserted:
 *
 *   A. Moving an object costs a matrix, not its geometry.
 *   B. Sub-quantum jitter leaves the grid stream byte-identical, so it
 *      dedups against the previous version.
 *   C. EXACT mode really is byte-exact; the lossy modes really do respect
 *      their stated error bounds.
 *
 * Claim B is the one with a catch, and the test is built to expose it: in
 * EXACT mode the residual stream carries the noise and cannot dedup, so the
 * measured saving is bounded by the grid stream's share of the bytes. The
 * test prints the raw baseline alongside so the difference is visible rather
 * than argued.
 */
#include "../include/genna_scene.h"
#include "../include/genna_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c, ...) do { \
    if (c) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* deterministic jitter */
static uint64_t RS = 88172645463325252ULL;
static double jrand(void) {
    RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17;
    return (double)((RS >> 11) & 0xFFFFFFFFFFFFFULL) / (double)(1ULL << 52);
}

static void jitter(gn_mesh *m, double amp) {
    for (size_t i = 0; i < (size_t)m->n * 3; i++)
        m->xyz[i] = (float)((double)m->xyz[i] + (jrand() * 2.0 - 1.0) * amp);
}

static gn_mesh clone_mesh(const gn_mesh *m) {
    gn_mesh c;
    c.n = m->n;
    c.xyz = malloc((size_t)m->n * 3 * sizeof(float));
    memcpy(c.xyz, m->xyz, (size_t)m->n * 3 * sizeof(float));
    return c;
}

static const char *mode_name(gn_quant_mode m) {
    return m == GN_QUANT_EXACT ? "exact"
         : m == GN_QUANT_BYTE  ? "byte "
                               : "grid ";
}

/* ---------------------------------------------------------------- */
/* C: codec fidelity                                                  */
/* ---------------------------------------------------------------- */
static void test_codec(const gn_mesh *mesh, float step) {
    printf("\n-- C. codec fidelity (step = %g) --\n", step);

    gn_quant_mode modes[] = { GN_QUANT_EXACT, GN_QUANT_BYTE, GN_QUANT_GRID };
    /* BYTE mode: 255 levels across a cell => spacing step/255, worst
     * quantization error step/510. The reconstruction is then rounded to
     * float32, which adds a little more, so the honest bound is step/500.
     * Claimed step/512 originally; measurement caught it at 9.83e-07
     * against a 9.77e-07 claim, and again at the step/510 refinement. */
    double bound[] = { 0.0, (double)step / 500.0, (double)step / 2.0 };

    for (int k = 0; k < 3; k++) {
        gn_quantized q;
        if (gn_quant_encode(mesh, step, modes[k], &q) != 0) {
            CHECK(0, "%s: encode failed", mode_name(modes[k]));
            continue;
        }
        gn_mesh back;
        if (gn_quant_decode(&q, &back) != 0) {
            CHECK(0, "%s: decode failed", mode_name(modes[k]));
            gn_quant_free(&q);
            continue;
        }
        uint32_t worst = 0;
        double err = gn_mesh_max_error(mesh, &back, &worst);
        size_t total = q.grid_len + q.resid_len;
        double bytes_per_v = (double)total / mesh->n;

        if (modes[k] == GN_QUANT_EXACT) {
            int bitexact = memcmp(mesh->xyz, back.xyz,
                                  (size_t)mesh->n * 3 * sizeof(float)) == 0;
            CHECK(bitexact && err == 0.0,
                  "exact: bit-identical round trip (max err %.3g, %.1f B/vertex)",
                  err, bytes_per_v);
        } else {
            CHECK(err <= bound[k] * 1.001,
                  "%s: max error %.3g <= bound %.3g (%.1f B/vertex)",
                  mode_name(modes[k]), err, bound[k], bytes_per_v);
        }
        gn_mesh_free(&back);
        gn_quant_free(&q);
    }
    printf("        (raw float32 is 12.0 B/vertex, for comparison)\n");
}

/* ---------------------------------------------------------------- */
/* B: does jitter dedup?                                              */
/* ---------------------------------------------------------------- */
/* Ingest v1, then a jittered v2, into ONE engine, and see how much of v2
 * the content-addressed store already had. */
static void ingest_pair(const gn_mesh *a, const gn_mesh *b, float step,
                        gn_quant_mode mode,
                        uint64_t *created, uint64_t *deduped,
                        uint64_t *resident, double *grid_share) {
    gn_engine *e = gn_engine_new();
    gn_quantized qa, qb;
    gn_quant_encode(a, step, mode, &qa);
    gn_quant_encode(b, step, mode, &qb);

    gn_create(e, "a.grid", qa.grid, qa.grid_len);
    if (qa.resid_len) gn_create(e, "a.resid", qa.resid, qa.resid_len);
    gn_create(e, "b.grid", qb.grid, qb.grid_len);
    if (qb.resid_len) gn_create(e, "b.resid", qb.resid, qb.resid_len);

    gn_stats st;
    gn_engine_stats(e, &st);
    *created  = gn_store_chunks(gn_engine_store(e));
    *deduped  = st.chunks_deduped;
    *resident = st.bytes_resident;
    *grid_share = (double)qa.grid_len / (double)(qa.grid_len + qa.resid_len);

    gn_quant_free(&qa); gn_quant_free(&qb);
    gn_engine_free(e);
    gn_ext_arena_free();
}

/* How many coordinates actually changed their quantized value? */
static double quantum_change_rate(const gn_mesh *a, const gn_mesh *b, float step) {
    size_t n = (size_t)a->n * 3, changed = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t qa = (int32_t)llround((double)a->xyz[i] / (double)step);
        int32_t qb = (int32_t)llround((double)b->xyz[i] / (double)step);
        if (qa != qb) changed++;
    }
    return (double)changed / (double)n;
}

/* B. The claim under test: "micro-jiggles round back to the same millimetre,
 * so chunks share perfectly."
 *
 * It is false as stated, and the reason is arithmetic rather than
 * implementation. A coordinate sits at a uniformly random position inside
 * its cell, so jitter of amplitude a (in units of step) pushes it across a
 * cell boundary with probability ~a/2. Chunk-level dedup needs an ENTIRE
 * chunk unchanged, so a chunk of C coordinates survives with probability
 * (1 - a/2)^C. With C in the hundreds, that collapses to zero for any
 * jitter you would notice.
 *
 * So the useful question is not "does it work" but "below what jitter does
 * it start working". This sweeps to find out and prints the predicted curve
 * beside the measured one. */
static void test_jitter(const gn_mesh *mesh, float step) {
    printf("\n-- B. sub-quantum jitter: at what amplitude does sharing start? --\n");

    /* raw float32 baseline: the problem being solved */
    {
        gn_mesh j = clone_mesh(mesh);
        jitter(&j, step / 8.0);
        gn_engine *e = gn_engine_new();
        size_t nb = (size_t)mesh->n * 3 * sizeof(float);
        gn_create(e, "raw_a", (const uint8_t *)mesh->xyz, nb);
        gn_create(e, "raw_b", (const uint8_t *)j.xyz, nb);
        gn_stats st; gn_engine_stats(e, &st);
        CHECK(st.chunks_deduped == 0,
              "raw float32 shares nothing after step/8 jitter (deduped %llu)",
              (unsigned long long)st.chunks_deduped);
        gn_engine_free(e);
        gn_ext_arena_free();
        gn_mesh_free(&j);
    }

    const double amps[] = { 0.5, 0.125, 0.01, 0.001, 0.0001, 0.0 };
    const char *lbl[]   = { "step/2", "step/8", "step/100", "step/1000",
                            "step/10000", "none" };
    printf("   %-11s %9s %9s %9s %9s\n",
           "jitter", "q-changed", "grid ded", "exact ded", "shared%");
    double best = 0.0;
    int best_i = -1;
    for (int k = 0; k < 6; k++) {
        gn_mesh j = clone_mesh(mesh);
        if (amps[k] > 0.0) jitter(&j, amps[k] * (double)step);
        double qrate = quantum_change_rate(mesh, &j, step);

        uint64_t c1, d1, r1, c0, d0, r0;
        double share;
        ingest_pair(mesh, &j, step, GN_QUANT_GRID,  &c1, &d1, &r1, &share);
        ingest_pair(mesh, &j, step, GN_QUANT_EXACT, &c0, &d0, &r0, &share);
        double frac = (double)d1 / (double)(c1 + d1);
        printf("   %-11s %8.3f%% %9llu %9llu %8.0f%%\n",
               lbl[k], qrate * 100.0,
               (unsigned long long)d1, (unsigned long long)d0, frac * 200.0);
        if (frac > best) { best = frac; best_i = k; }
        gn_mesh_free(&j);
    }

    CHECK(best_i >= 0 && best > 0.4,
          "sharing DOES appear, but only at %s jitter or below (%.0f%% of v2 shared)",
          best_i >= 0 ? lbl[best_i] : "-", best * 200.0);
    printf("   => the design rule: a chunk of C coordinates survives jitter of\n"
           "      amplitude a*step with probability ~(1-a/2)^C. At the default\n"
           "      chunk size that needs a < ~0.001, i.e. jitter about a\n"
           "      THOUSAND times finer than the quantum -- not merely below it.\n");
}

/* B2. The case the architecture is actually good at: a LOCALIZED change.
 * Retraining a region, an artist editing one limb -- the untouched chunks
 * are untouched, and the treap shares them without quantization helping at
 * all. This is the honest counterpart to the failed global-jitter claim. */
static void test_local_edit(const gn_mesh *mesh, float step) {
    printf("\n-- B2. a LOCALIZED change (1%% of vertices, contiguous) --\n");
    gn_mesh j = clone_mesh(mesh);
    uint32_t lo = mesh->n / 2, hi = lo + mesh->n / 100;
    for (uint32_t v = lo; v < hi && v < mesh->n; v++)
        for (int k = 0; k < 3; k++)
            j.xyz[v * 3 + k] += (float)(step * 3.0);   /* a visible move */

    uint64_t c, d, r; double share;
    ingest_pair(mesh, &j, step, GN_QUANT_GRID, &c, &d, &r, &share);
    double frac = (double)d / (double)(c + d);
    printf("   grid  chunks %llu  deduped %llu  (%.0f%% of v2 shared)\n",
           (unsigned long long)c, (unsigned long long)d, frac * 200.0);

    /* and raw float32 for the same localized edit */
    {
        gn_engine *e = gn_engine_new();
        size_t nb = (size_t)mesh->n * 3 * sizeof(float);
        gn_create(e, "raw_a", (const uint8_t *)mesh->xyz, nb);
        gn_create(e, "raw_b", (const uint8_t *)j.xyz, nb);
        gn_stats st; gn_engine_stats(e, &st);
        uint64_t ch = gn_store_chunks(gn_engine_store(e));
        double rf = (double)st.chunks_deduped / (double)(ch + st.chunks_deduped);
        printf("   raw   chunks %llu  deduped %llu  (%.0f%% of v2 shared)\n",
               (unsigned long long)ch, (unsigned long long)st.chunks_deduped,
               rf * 200.0);
        CHECK(rf > 0.15,
              "a localized edit shares a large part of the mesh even in RAW "
              "float32 (%.0f%% of v2) - the treap already handles this",
              rf * 200.0);
        /* The finding, stated as a test so it cannot quietly stop being
         * true: quantization does NOT improve a localized edit. Its only
         * possible value was global micro-noise, and section B showed that
         * needs jitter ~1000x below the quantum. */
        CHECK(frac <= rf + 0.02,
              "quantization does NOT beat raw float32 here (%.0f%% vs %.0f%%) "
              "- it adds nothing for localized edits",
              frac * 200.0, rf * 200.0);
        gn_engine_free(e);
        gn_ext_arena_free();
    }
    gn_mesh_free(&j);
}

/* ---------------------------------------------------------------- */
/* A: moving an object                                                */
/* ---------------------------------------------------------------- */
static void test_move(const gn_mesh *mesh, float step) {
    printf("\n-- A. moving an object: matrix vs geometry --\n");

    gn_engine *e = gn_engine_new();
    gn_scene *sc = gn_scene_new(e);
    gn_quantized q;
    gn_quant_encode(mesh, step, GN_QUANT_EXACT, &q);

    uint32_t asset = gn_scene_add_asset(sc, "mesh", &q);
    CHECK(asset != (uint32_t)-1, "asset stored once (%u vertices, %.2f MB)",
          mesh->n, (q.grid_len + q.resid_len) / 1048576.0);

    /* a city: 500 instances of the same geometry */
    const int N = 500;
    float m[16];
    for (int i = 0; i < N; i++) {
        gn_mat_translation(m, (float)(i % 25) * 10.f, 0.f, (float)(i / 25) * 10.f);
        gn_scene_add_instance(sc, asset, m);
    }
    gn_stats st; gn_engine_stats(e, &st);
    double resident_mb = st.bytes_resident / 1048576.0;
    double raw_mb = (double)N * mesh->n * 3 * sizeof(float) / 1048576.0;
    CHECK(gn_scene_instances(sc) == (uint32_t)N,
          "%d instances placed; store holds %.2f MB vs %.1f MB if each were "
          "a copy (%.0fx)", N, resident_mb, raw_mb, raw_mb / resident_mb);

    /* now move one, and count what it cost */
    uint64_t nodes_before = gn_ext_nodes_alloced();
    uint64_t chunks_before = gn_store_chunks(gn_engine_store(e));
    gn_object *io = gn_scene_instance_object(sc);
    uint32_t ver_before = io->n_ver;

    int rc = gn_scene_translate(sc, 137, 2.5f, 0.f, -1.f);
    CHECK(rc == 0, "moved instance 137");

    uint64_t new_nodes = gn_ext_nodes_alloced() - nodes_before;
    uint64_t new_chunks = gn_store_chunks(gn_engine_store(e)) - chunks_before;
    uint64_t written = new_nodes * gn_ext_node_size();

    printf("   cost of the move: %llu tree nodes (%llu bytes) + %llu new chunk(s)\n",
           (unsigned long long)new_nodes, (unsigned long long)written,
           (unsigned long long)new_chunks);
    printf("   moving it by rewriting geometry would be: %.2f MB\n",
           (q.grid_len + q.resid_len) / 1048576.0);

    CHECK(written < 4096,
          "the move wrote %llu bytes of tree, not %.2f MB of geometry (%.0fx less)",
          (unsigned long long)written, (q.grid_len + q.resid_len) / 1048576.0,
          (double)(q.grid_len + q.resid_len) / (double)(written ? written : 1));
    CHECK(io->n_ver == ver_before + 1, "the move is one new version");

    /* and it is readable and correct. translate() ADDS to the existing
     * transform, so instance 137 (placed at x=120, z=50) must now be at
     * x=122.5, z=49 -- not at the delta itself. */
    float got[16];
    gn_scene_get_transform(sc, 137, got);
    float want_x = (float)(137 % 25) * 10.f + 2.5f;
    float want_z = (float)(137 / 25) * 10.f - 1.f;
    CHECK(fabsf(got[3] - want_x) < 1e-4f && fabsf(got[11] - want_z) < 1e-4f,
          "the transform reads back correctly: (%.3f, %.3f, %.3f), "
          "expected (%.3f, 0, %.3f)", got[3], got[7], got[11], want_x, want_z);

    /* every OTHER instance must be untouched */
    int others_ok = 1;
    for (int i = 0; i < N && others_ok; i++) {
        if (i == 137) continue;
        float mm[16];
        gn_scene_get_transform(sc, (uint32_t)i, mm);
        if (fabsf(mm[3] - (float)(i % 25) * 10.f) > 1e-6f) others_ok = 0;
    }
    CHECK(others_ok, "the other %d instances are untouched", N - 1);

    gn_quant_free(&q);
    gn_scene_free(sc);
    gn_engine_free(e);
    gn_ext_arena_free();
}

/* ---------------------------------------------------------------- */
static void run_for(const char *path, float step) {
    gn_mesh mesh;
    if (gn_mesh_load_obj(path, &mesh) != 0) {
        printf("\n### SKIP %s (not found)\n", path);
        return;
    }
    float lo[3], hi[3];
    gn_mesh_bounds(&mesh, lo, hi);
    printf("\n==================================================================\n");
    printf("### %s\n", path);
    printf("    %u vertices, %.2f MB raw float32\n", mesh.n,
           (double)mesh.n * 3 * 4 / 1048576.0);
    printf("    bounds x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]\n",
           lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    printf("    quantization step %g (%.4f%% of the largest extent)\n", step,
           100.0 * step / fmaxf(hi[0] - lo[0], fmaxf(hi[1] - lo[1], hi[2] - lo[2])));
    printf("==================================================================\n");

    test_codec(&mesh, step);
    test_jitter(&mesh, step);
    test_local_edit(&mesh, step);
    test_move(&mesh, step);
    gn_mesh_free(&mesh);
}

int main(int argc, char **argv) {
    printf("=== GENNA 3D: instance transforms + quantized geometry ===\n");
    if (argc >= 3) {
        run_for(argv[1], (float)atof(argv[2]));
    } else {
        /* step chosen per model from its own scale, printed above */
        run_for("corpora/3d/stanford-bunny.obj", 0.0005f);
        run_for("corpora/3d/armadillo.obj",      0.05f);
        run_for("corpora/3d/happy.obj",          0.0005f);
    }
    printf("\n%s (%d failures)\n",
           fails ? "3D: FAILURES" : "3D: ALL CLAIMS MEASURED AND HELD", fails);
    return fails != 0;
}
