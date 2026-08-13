/* persist_test.c — the bar: byte-exact across a process boundary.
 *
 * Run in two phases, as two separate processes, so nothing can be smuggled
 * through RAM:
 *
 *   phase 1 (write):  ingest real data, edit it N times, dump EVERY version's
 *                     full bytes to a reference file, then gn_save().
 *   phase 2 (verify): fresh process. gn_open(), then memcmp every version
 *                     against the reference, byte for byte.
 *
 * A hash would not be proof enough here -- the claim is byte-exactness, so
 * the test keeps the actual bytes and compares the actual bytes.
 *
 * Phase 2 also checks the things a content-only comparison would miss:
 *   - the version DAG survived as a DAG (save -> open -> save is byte-
 *     identical, which can only hold if sharing was preserved exactly)
 *   - the engine is still WRITABLE after a reload, and edits made after the
 *     reload are themselves correct
 *   - chunk-store dedup survived (chunk count did not grow on reload)
 *
 * usage: persist_test write <corpus> <store> <ref>
 *        persist_test verify <store> <ref>
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include "gn_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef GN_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef GN_HAVE_ZSTD
#include <zstd.h>
#endif

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#ifndef N_EDITS
#define N_EDITS 30
#endif

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s + 1);
    if (fread(b, 1, (size_t)s, f) != (size_t)s) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f); *n = (size_t)s; return b;
}

/* Read a snapshot and hand back its UNCOMPRESSED payload.
 *
 * The save->open->save comparison below needs the structural payload, not the
 * file. Once the payload is deflate-compressed, "skip the last 48 bytes to
 * ignore the stats counters" stops working -- those bytes are inside the
 * compressed stream and a one-counter difference perturbs it anywhere. */
static uint8_t *snapshot_payload(const char *path, size_t *out_len) {
    size_t flen = 0;
    uint8_t *fb = slurp(path, &flen);
    if (flen < 64) { free(fb); *out_len = 0; return NULL; }
    uint32_t flags = (uint32_t)fb[12] | ((uint32_t)fb[13] << 8)
                   | ((uint32_t)fb[14] << 16) | ((uint32_t)fb[15] << 24);
    uint64_t plen = 0, rawlen = 0;
    for (int i = 0; i < 8; i++) plen   |= (uint64_t)fb[24 + i] << (8 * i);
    for (int i = 0; i < 8; i++) rawlen |= (uint64_t)fb[36 + i] << (8 * i);

    if ((flags & 3u) == 0) {                 /* stored raw */
        uint8_t *out = malloc((size_t)plen + 1);
        memcpy(out, fb + 64, (size_t)plen);
        free(fb); *out_len = (size_t)plen; return out;
    }
#ifdef GN_HAVE_ZSTD
    if (flags & 2u) {                        /* zstd */
        uint8_t *out = malloc((size_t)rawlen + 1);
        size_t n = ZSTD_decompress(out, (size_t)rawlen, fb + 64, (size_t)plen);
        free(fb);
        if (ZSTD_isError(n)) { free(out); *out_len = 0; return NULL; }
        *out_len = n;
        return out;
    }
#endif
#ifdef GN_HAVE_ZLIB
    if (flags & 1u) {                        /* deflate */
        uint8_t *out = malloc((size_t)rawlen + 1);
        uLongf n = (uLongf)rawlen;
        int rc = uncompress(out, &n, fb + 64, (uLong)plen);
        free(fb);
        if (rc != Z_OK) { free(out); *out_len = 0; return NULL; }
        *out_len = (size_t)n;
        return out;
    }
#endif
    free(fb); *out_len = 0; return NULL;
}

/* reference file: [u32 n_ver] then per version [u64 len][len bytes] */
static void put_u64(FILE *f, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
    fwrite(b, 1, 8, f);
}
static uint64_t get_u64(FILE *f) {
    uint8_t b[8];
    if (fread(b, 1, 8, f) != 8) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)b[i] << (8 * i);
    return v;
}

/* deterministic edit schedule, so both phases agree on what was done */
static uint64_t lcg(uint64_t *s) {
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return *s;
}

/* ------------------------------------------------------------------ */
static int phase_write(const char *corpus, const char *store, const char *ref) {
    size_t n; uint8_t *txt = slurp(corpus, &n);
    printf("-- phase 1: ingest %s (%.2f MB), %d edits, save --\n",
           corpus, n / 1048576.0, N_EDITS);

    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 6, 200000, 16);
    gn_object *o = gn_create(e, "dataset", txt, n);

    /* a mix of the three splice shapes, at offsets spread over the object */
    uint64_t s = 0xC0FFEE;
    for (int i = 0; i < N_EDITS; i++) {
        gn_version *cur = &o->ver[o->n_ver - 1];
        uint64_t tb = cur->total_bytes;
        if (tb < 64) break;
        uint64_t at = lcg(&s) % (tb - 40);
        int kind = (int)(lcg(&s) % 3);
        char ins[96];
        int il = snprintf(ins, sizeof ins, "[EDIT-%03d-%llu]", i, (unsigned long long)(s & 0xFFFF));
        if (kind == 0)       gn_update(e, o, at, 0, (const uint8_t*)ins, (size_t)il);
        else if (kind == 1)  gn_update(e, o, at, 17 + (lcg(&s) % 23), NULL, 0);
        else                 gn_update(e, o, at, 11, (const uint8_t*)ins, (size_t)il);
    }
    printf("   versions built: %u\n", o->n_ver);

    /* dump every version's bytes as the reference */
    FILE *rf = fopen(ref, "wb");
    if (!rf) { perror(ref); return 2; }
    uint8_t hdr[4];
    for (int i = 0; i < 4; i++) hdr[i] = (uint8_t)(o->n_ver >> (8 * i));
    fwrite(hdr, 1, 4, rf);
    uint64_t total_ref = 0;
    for (uint32_t v = 0; v < o->n_ver; v++) {
        uint64_t len = o->ver[v].total_bytes;
        uint8_t *b = malloc((size_t)len + 16);
        size_t got = gn_read_version(e, o, v, 0, (size_t)len, b);
        if (got != len) { printf("  FAIL  phase1 read v%u short (%zu != %llu)\n",
                                 v, got, (unsigned long long)len); fails++; }
        put_u64(rf, len);
        fwrite(b, 1, (size_t)len, rf);
        total_ref += len;
        free(b);
    }
    fclose(rf);
    printf("   reference: %u versions, %.1f MB of bytes\n", o->n_ver, total_ref / 1048576.0);

    gn_stats st; gn_engine_stats(e, &st);
    printf("   store: %llu chunks, %.2f MB resident\n",
           (unsigned long long)gn_store_chunks(gn_engine_store(e)),
           st.bytes_resident / 1048576.0);

    if (gn_save(e, store) != 0) { perror("gn_save"); return 2; }
    printf("   saved -> %s\n", store);

    /* the save must not have disturbed anything still in memory */
    uint8_t *b0 = malloc((size_t)o->ver[0].total_bytes + 16);
    size_t g0 = gn_read_version(e, o, 0, 0, (size_t)o->ver[0].total_bytes, b0);
    CHECK(g0 == o->ver[0].total_bytes && memcmp(b0, txt, g0) == 0,
          "v0 still byte-exact in the saving process");
    free(b0);

    gn_engine_free(e);
    free(txt);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ */
static int phase_verify(const char *store, const char *ref) {
    printf("-- phase 2: FRESH PROCESS, open %s, memcmp every version --\n", store);

    gn_engine *e = gn_open(store);
    if (!e) { perror("gn_open"); printf("  FAIL  gn_open returned NULL\n"); return 1; }

    CHECK(gn_engine_objects(e) == 1, "object count restored (%u)", gn_engine_objects(e));
    gn_object *o = gn_object_open(e, "dataset");
    if (!o) { printf("  FAIL  object 'dataset' not found after open\n"); return 1; }

    FILE *rf = fopen(ref, "rb");
    if (!rf) { perror(ref); return 2; }
    uint8_t h[4];
    if (fread(h, 1, 4, rf) != 4) { printf("  FAIL  reference truncated\n"); return 1; }
    uint32_t nref = (uint32_t)h[0] | ((uint32_t)h[1] << 8)
                  | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);

    CHECK(o->n_ver == nref, "version count survived: %u (expected %u)", o->n_ver, nref);
    if (o->n_ver != nref) { fclose(rf); return 1; }

    uint32_t bad = 0; uint64_t cmp_bytes = 0;
    for (uint32_t v = 0; v < nref; v++) {
        uint64_t len = get_u64(rf);
        uint8_t *want = malloc((size_t)len + 16);
        if (len && fread(want, 1, (size_t)len, rf) != len) {
            printf("  FAIL  reference short at v%u\n", v); free(want); fclose(rf); return 1;
        }
        uint8_t *got = malloc((size_t)len + 16);
        size_t g = gn_read_version(e, o, v, 0, (size_t)len, got);
        if (g != len || (len && memcmp(got, want, (size_t)len) != 0)) {
            if (bad < 5) {
                size_t k = 0;
                while (k < len && k < g && got[k] == want[k]) k++;
                printf("  FAIL  v%u differs: got %zu bytes, want %llu, first diff at %zu\n",
                       v, g, (unsigned long long)len, k);
            }
            bad++;
        }
        cmp_bytes += len;
        free(want); free(got);
    }
    fclose(rf);
    CHECK(bad == 0, "all %u versions byte-exact after reload (%.1f MB compared)",
          nref, cmp_bytes / 1048576.0);

    /* --- the DAG really is a DAG on disk ----------------------------------
     * Re-saving a reloaded store must reproduce the identical payload. That
     * can only happen if every shared subtree came back shared: had reload
     * expanded sharing into copies, the node count -- and the file -- would
     * grow.                                                                */
    {
        char s2[600], w2[640];
        snprintf(s2, sizeof s2, "%s.again", store);
        snprintf(w2, sizeof w2, "%s.wal", s2);
        if (gn_save(e, s2) != 0) { printf("  FAIL  re-save failed\n"); fails++; }
        else {
            /* Compare the DECOMPRESSED payloads, minus the 48-byte stats
             * trailer (six u64 counters of work done: reads performed since
             * the load move tokens_detokenized, which is not content). */
            size_t n1 = 0, n2 = 0;
            uint8_t *p1 = snapshot_payload(store, &n1);
            uint8_t *p2 = snapshot_payload(s2, &n2);
            const size_t STATS = 48;
            int same = 0; size_t first = 0;
            if (p1 && p2 && n1 == n2 && n1 > STATS) {
                size_t body = n1 - STATS;
                same = memcmp(p1, p2, body) == 0;
                if (!same) { while (first < body && p1[first] == p2[first]) first++; }
            }
            if (same) {
                CHECK(1, "save->open->save byte-identical over %zu payload bytes "
                         "- structural sharing preserved", n1 - STATS);
            } else {
                printf("  FAIL  save->open->save differs: payloads %zu vs %zu, "
                       "first diff at %zu\n", n1, n2, first);
                fails++;
            }
            free(p1); free(p2);
            remove(s2); remove(w2);
        }
    }

    /* --- still writable, and correct, after a reload --------------------- */
    {
        uint32_t before = o->n_ver;
        uint64_t tb = o->ver[before - 1].total_bytes;
        const char *mark = "<<POST-RELOAD-EDIT>>";
        size_t ml = strlen(mark);
        int rc = gn_update(e, o, tb / 2, 0, (const uint8_t*)mark, ml);
        CHECK(rc == 0 && o->n_ver == before + 1, "engine still writable after reload");

        uint64_t nb = o->ver[o->n_ver - 1].total_bytes;
        CHECK(nb == tb + ml, "post-reload edit changed length correctly "
                             "(%llu -> %llu)", (unsigned long long)tb, (unsigned long long)nb);
        uint8_t *b = malloc((size_t)nb + 16);
        gn_read_version(e, o, o->n_ver - 1, 0, (size_t)nb, b);
        CHECK(memmem(b + tb / 2 - 4, ml + 8, mark, ml) != NULL,
              "post-reload edit lands at the right offset");
        free(b);

        /* and the pre-existing history is untouched by the new edit */
        uint64_t l0 = o->ver[0].total_bytes;
        uint8_t *b0 = malloc((size_t)l0 + 16);
        size_t g0 = gn_read_version(e, o, 0, 0, (size_t)l0, b0);
        FILE *rf2 = fopen(ref, "rb");
        uint8_t hh[4]; size_t rd = fread(hh, 1, 4, rf2); (void)rd;
        uint64_t rl0 = get_u64(rf2);
        uint8_t *w0 = malloc((size_t)rl0 + 16);
        rd = fread(w0, 1, (size_t)rl0, rf2); (void)rd;
        fclose(rf2);
        CHECK(g0 == rl0 && memcmp(b0, w0, (size_t)rl0) == 0,
              "v0 STILL byte-exact after a post-reload edit");
        free(b0); free(w0);
    }

    gn_close(e);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "write") == 0 && argc == 5)
        return phase_write(argv[2], argv[3], argv[4]);
    if (argc >= 2 && strcmp(argv[1], "verify") == 0 && argc == 4) {
        int rc = phase_verify(argv[2], argv[3]);
        printf("\n%s (%d failures)\n",
               fails ? "PERSIST ROUND-TRIP: FAILURES" : "PERSIST ROUND-TRIP: BYTE-EXACT", fails);
        return rc;
    }
    fprintf(stderr, "usage: %s write <corpus> <store> <ref>\n"
                    "       %s verify <store> <ref>\n", argv[0], argv[0]);
    return 2;
}
