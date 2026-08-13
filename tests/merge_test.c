/* merge_test.c — does the merge produce the right bytes, and does it refuse
 * when it should?
 *
 * A merge that is wrong is worse than a merge that is missing, so the checks
 * are in that order:
 *
 *   1. A clean merge must equal, byte for byte, the document you get by
 *      applying both edits by hand. Not "looks right" -- memcmp.
 *   2. Overlapping edits must CONFLICT. A merger that silently picks a side
 *      passes every happy-path test and loses data in production.
 *   3. Merging must not disturb the versions it merged. Structural sharing
 *      means head and other are still there, unchanged, afterwards.
 */
#include "../include/genna.h"
#include "../include/genna_merge.h"
#include "../include/genna_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c, ...) do { \
    if (c) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* Build the expected document by splicing directly, independent of Genna. */
static uint8_t *splice(const uint8_t *s, size_t n, size_t at, size_t del,
                       const char *ins, size_t inl, size_t *out_n) {
    size_t m = n - del + inl;
    uint8_t *b = malloc(m + 1);
    memcpy(b, s, at);
    memcpy(b + at, ins, inl);
    memcpy(b + at + inl, s + at + del, n - at - del);
    *out_n = m; b[m] = 0;
    return b;
}

static uint8_t *read_all(gn_engine *e, gn_object *o, uint32_t v, size_t *n) {
    uint64_t len = o->ver[v].total_bytes;
    uint8_t *b = malloc((size_t)len + 1);
    size_t got = gn_read_version(e, o, v, 0, (size_t)len, b);
    *n = got; b[got] = 0;
    return b;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== three-way merge ===\n\n");

    /* A document with enough structure that edits land in different chunks. */
    size_t BASE_N = 200000;
    uint8_t *base = malloc(BASE_N + 1);
    for (size_t i = 0; i < BASE_N; i++)
        base[i] = (uint8_t)('a' + (i * 7 + i / 64) % 26);
    base[BASE_N] = 0;

    /* ---- 1. disjoint edits merge, byte-exact ------------------------- */
    {
        gn_engine *e = gn_engine_new();
        gn_object *o = gn_create(e, "doc", base, BASE_N);
        uint32_t v_base = o->n_ver - 1;

        /* head edits near the front */
        const char *HE = "[[HEAD-EDIT]]";
        gn_update(e, o, 1000, 20, (const uint8_t *)HE, strlen(HE));
        uint32_t v_head = o->n_ver - 1;

        /* other edits near the back, branched from the same base:
         * reconstruct it as a separate version derived from base by
         * splicing base directly, which is what a second writer would have
         * produced independently. */
        const char *OE = "<<OTHER-EDIT>>";
        size_t exp_o_n;
        uint8_t *exp_o = splice(base, BASE_N, 150000, 30, OE, strlen(OE), &exp_o_n);
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, exp_o, exp_o_n);
        uint32_t v_other = o->n_ver - 1;

        /* head is no longer latest, so put it back on top to merge INTO it */
        size_t hn; uint8_t *hbytes = read_all(e, o, v_head, &hn);
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, hbytes, hn);

        gn_merge_info info;
        int rc = gn_merge(e, o, v_base, v_other, &info);
        CHECK(rc == GN_MERGE_OK,
              "disjoint edits merge (rc=%d, head span [%llu,%llu), other span "
              "[%llu,%llu))", rc,
              (unsigned long long)info.head_lo, (unsigned long long)info.head_hi,
              (unsigned long long)info.other_lo, (unsigned long long)info.other_hi);

        /* what it should be: base with BOTH splices applied */
        size_t t1n; uint8_t *t1 = splice(base, BASE_N, 1000, 20, HE, strlen(HE), &t1n);
        /* the second splice shifts by the first's length delta */
        long d1 = (long)strlen(HE) - 20;
        size_t t2n; uint8_t *t2 = splice(t1, t1n, (size_t)(150000 + d1), 30,
                                         OE, strlen(OE), &t2n);
        size_t mn; uint8_t *merged = read_all(e, o, info.merged_version, &mn);
        CHECK(mn == t2n && memcmp(merged, t2, mn) == 0,
              "merged document is byte-identical to applying both edits by "
              "hand (%zu bytes)", mn);

        /* both inputs survive untouched */
        size_t hn2; uint8_t *h2 = read_all(e, o, v_head, &hn2);
        CHECK(hn2 == hn && memcmp(h2, hbytes, hn) == 0,
              "the head version it merged into is unchanged");
        size_t on2; uint8_t *o2 = read_all(e, o, v_other, &on2);
        CHECK(on2 == exp_o_n && memcmp(o2, exp_o, exp_o_n) == 0,
              "the other version is unchanged");
        size_t bn2; uint8_t *b2 = read_all(e, o, v_base, &bn2);
        CHECK(bn2 == BASE_N && memcmp(b2, base, BASE_N) == 0,
              "the common ancestor is unchanged");

        free(t1); free(t2); free(merged); free(hbytes); free(h2);
        free(exp_o); free(o2); free(b2);
        gn_engine_free(e); gn_ext_arena_free();
    }

    /* ---- 2. overlapping edits MUST conflict --------------------------- */
    {
        gn_engine *e = gn_engine_new();
        gn_object *o = gn_create(e, "doc", base, BASE_N);
        uint32_t v_base = o->n_ver - 1;

        size_t an; uint8_t *a = splice(base, BASE_N, 5000, 100, "AAAA", 4, &an);
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, a, an);
        uint32_t v_other = o->n_ver - 1;

        size_t hn; uint8_t *h = splice(base, BASE_N, 5050, 100, "BBBB", 4, &hn);
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, h, hn);

        uint32_t before = o->n_ver;
        gn_merge_info info;
        int rc = gn_merge(e, o, v_base, v_other, &info);
        CHECK(rc == GN_MERGE_CONFLICT,
              "overlapping edits are REFUSED, not silently resolved (rc=%d)", rc);
        CHECK(info.conflict == 1, "the conflict is reported in the info struct");
        CHECK(o->n_ver == before,
              "a conflicting merge appends NO version (%u == %u)",
              o->n_ver, before);
        printf("        head span [%llu,%llu), other span [%llu,%llu)\n",
               (unsigned long long)info.head_lo, (unsigned long long)info.head_hi,
               (unsigned long long)info.other_lo, (unsigned long long)info.other_hi);

        free(a); free(h);
        gn_engine_free(e); gn_ext_arena_free();
    }

    /* ---- 3. one side unchanged is a fast-forward ---------------------- */
    {
        gn_engine *e = gn_engine_new();
        gn_object *o = gn_create(e, "doc", base, BASE_N);
        uint32_t v_base = o->n_ver - 1;

        size_t an; uint8_t *a = splice(base, BASE_N, 77000, 10, "ZZZ", 3, &an);
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, a, an);
        uint32_t v_other = o->n_ver - 1;

        /* head goes back to being exactly base */
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, base, BASE_N);

        gn_merge_info info;
        int rc = gn_merge(e, o, v_base, v_other, &info);
        CHECK(rc == GN_MERGE_OK, "merging into an unchanged head succeeds");
        size_t mn; uint8_t *m = read_all(e, o, info.merged_version, &mn);
        CHECK(mn == an && memcmp(m, a, an) == 0,
              "and yields exactly the other side (fast-forward, %zu bytes)", mn);
        free(a); free(m);
        gn_engine_free(e); gn_ext_arena_free();
    }

    /* ---- 4. merging a version into itself changes nothing ------------- */
    {
        gn_engine *e = gn_engine_new();
        gn_object *o = gn_create(e, "doc", base, BASE_N);
        uint32_t v_base = o->n_ver - 1;
        const char *E = "###";
        gn_update(e, o, 4000, 10, (const uint8_t *)E, strlen(E));
        uint32_t v_head = o->n_ver - 1;

        size_t hn; uint8_t *h = read_all(e, o, v_head, &hn);
        gn_merge_info info;
        int rc = gn_merge(e, o, v_base, v_head, &info);
        CHECK(rc == GN_MERGE_OK,
              "merging head into itself succeeds rather than conflicting "
              "(identical=%d)", info.identical);
        CHECK(info.identical == 1,
              "and is recognised as convergence, not a clash");
        {
            size_t mn; uint8_t *m = read_all(e, o, info.merged_version, &mn);
            CHECK(mn == hn && memcmp(m, h, hn) == 0,
                  "the document is unchanged (idempotent, %zu bytes)", mn);
            free(m);
        }
        free(h);
        gn_engine_free(e); gn_ext_arena_free();
    }

    /* ---- 5. a merged store round-trips through disk ------------------- */
    {
        const char *sp = getenv("GN_MERGE_DIR");
        char path[1024];
        snprintf(path, sizeof path, "%s/merge_rt.gn", sp ? sp : ".");
        gn_engine *e = gn_engine_new();
        gn_object *o = gn_create(e, "doc", base, BASE_N);
        uint32_t v_base = o->n_ver - 1;

        size_t an; uint8_t *a = splice(base, BASE_N, 120000, 40, "OTHER", 5, &an);
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, a, an);
        uint32_t v_other = o->n_ver - 1;
        size_t hn; uint8_t *h = splice(base, BASE_N, 2000, 40, "HEAD", 4, &hn);
        gn_update(e, o, 0, o->ver[o->n_ver - 1].total_bytes, h, hn);

        gn_merge_info info;
        int rc = gn_merge(e, o, v_base, v_other, &info);
        CHECK(rc == GN_MERGE_OK, "merge before saving");
        size_t mn; uint8_t *m = read_all(e, o, info.merged_version, &mn);

        CHECK(gn_save(e, path) == 0, "merged store saves");
        gn_engine_free(e); gn_ext_arena_free();

        gn_engine *e2 = gn_open(path);
        CHECK(e2 != NULL, "merged store reopens");
        if (e2) {
            gn_object *o2 = gn_object_open(e2, "doc");
            size_t mn2; uint8_t *m2 = read_all(e2, o2, o2->n_ver - 1, &mn2);
            CHECK(mn2 == mn && memcmp(m2, m, mn) == 0,
                  "the merged version is byte-exact after a save/open cycle "
                  "(%zu bytes)", mn2);
            free(m2);
            gn_engine_free(e2); gn_ext_arena_free();
        }
        remove(path);
        { char w[1100]; snprintf(w, sizeof w, "%s.wal", path); remove(w); }
        free(a); free(h); free(m);
    }

    free(base);
    printf("\n%s (%d failures)\n",
           fails ? "MERGE: FAILURES" : "MERGE: ALL PASS", fails);
    return fails != 0;
}
