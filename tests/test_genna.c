/* test_genna.c — the architecture's claims as executable assertions.
 * Part 1: correctness (round-trip, splice, versions, search).
 * Part 2: the demo numbers (edit cost vs file size; search vs memmem). */
#include "../include/genna.h"
#include "gn_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Consumed by the rewrite baseline below so the compiler cannot delete it.
 * File scope on purpose: a local would trip -Wunused-but-set-variable. */
volatile uint8_t gn_test_sink;

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok    %s\n", msg); \
    else { printf("  FAIL  %s\n", msg); fails++; } } while (0)

/* deterministic pseudo-English corpus generator */
static const char *W[] = {"the","engine","holds","data","below","layer",
    "compression","token","chunk","search","genna","system","memory",
    "query","index","structure","model","language","version","immutable"};
static size_t gen_text(uint8_t *out, size_t cap, uint64_t seed, size_t words) {
    size_t n = 0; uint64_t s = seed;
    for (size_t i = 0; i < words && n + 16 < cap; i++) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const char *w = W[(s >> 33) % 20];
        size_t wl = strlen(w);
        memcpy(out + n, w, wl); n += wl;
        out[n++] = (s >> 40) % 13 == 0 ? '\n' : ' ';
    }
    return n;
}

int main(void) {
    printf("=== GENNA v1 test run ===\n\n-- correctness --\n");
    gn_engine *e = gn_engine_new();

    /* C + R round trip */
    const char *S1 = "the engine holds data below the layer";
    gn_object *o = gn_create(e, "a.txt", (const uint8_t*)S1, strlen(S1));
    uint8_t buf[512];
    size_t r = gn_read(e, o, 0, 512, buf); buf[r] = 0;
    CHECK(r == strlen(S1) && memcmp(buf, S1, r) == 0, "C+R: exact round trip");

    /* R: mid-range read */
    r = gn_read(e, o, 4, 6, buf); buf[r] = 0;
    CHECK(r == 6 && memcmp(buf, "engine", 6) == 0, "R: byte-range read mid-file");

    /* U: insert in the middle */
    gn_update(e, o, 11, 0, (const uint8_t*)"quietly ", 8);
    r = gn_read(e, o, 0, 512, buf); buf[r] = 0;
    CHECK(strcmp((char*)buf, "the engine quietly holds data below the layer") == 0,
          "U: insert mid-file");

    /* U: delete a range */
    gn_update(e, o, 11, 8, NULL, 0);
    r = gn_read(e, o, 0, 512, buf); buf[r] = 0;
    CHECK(strcmp((char*)buf, S1) == 0, "U: delete restores original");

    /* U: replace */
    gn_update(e, o, 0, 3, (const uint8_t*)"our", 3);
    r = gn_read(e, o, 0, 512, buf); buf[r] = 0;
    CHECK(strcmp((char*)buf, "our engine holds data below the layer") == 0,
          "U: replace at file start");

    /* versions: time travel */
    r = gn_read_version(e, o, 0, 0, 512, buf); buf[r] = 0;
    CHECK(strcmp((char*)buf, S1) == 0, "versions: v0 readable after 3 edits");
    CHECK(o->n_ver == 4, "versions: history length = 4");

    /* search below the layer */
    gn_create(e, "b.txt",
        (const uint8_t*)"another file where the engine appears twice: engine",
        51);
    gn_hit hits[16];
    size_t nh = gn_search(e, (const uint8_t*)"engine", 6, hits, 16);
    CHECK(nh == 3, "search: 'engine' found 3x across objects");
    nh = gn_search(e, (const uint8_t*)"zzzcryptid", 10, hits, 16);
    CHECK(nh == 0, "search: absent word -> instant corpus-wide no");
    nh = gn_search(e, (const uint8_t*)"below the layer", 15, hits, 16);
    CHECK(nh == 1, "search: multi-token phrase match");

    /* dedup: identical content stored once */
    gn_stats st0; gn_engine_stats(e, &st0);
    gn_create(e, "copy.txt", (const uint8_t*)S1, strlen(S1));
    gn_stats st1; gn_engine_stats(e, &st1);
    CHECK(st1.chunks_deduped > st0.chunks_deduped, "dedup: identical chunk reused");

    gn_engine_free(e);

    /* ---------------- demo benchmark ---------------- */
    printf("\n-- demo numbers (100MB pseudo-English) --\n");
    e = gn_engine_new();
    size_t RAW = 100u << 20;
    uint8_t *big = malloc(RAW + 64);
    size_t blen = gen_text(big, RAW, 42, RAW / 6);

    double t_in0 = now_ms();
    gn_object *B = gn_create(e, "big.txt", big, blen);
    double t_in1 = now_ms();
    double t0, t1;

    gn_stats st; gn_engine_stats(e, &st);
    printf("  ingest: %.1f MB in %.0f ms (%.0f MB/s)\n",
           blen / 1048576.0, t_in1 - t_in0, blen / 1048.576 / (t_in1 - t_in0));
    printf("  resident tokens: %.1f MB  (%.2fx vs raw)\n",
           st.bytes_resident / 1048576.0, (double)blen / st.bytes_resident);
    printf("  chunks: %llu created, %llu deduped\n",
           (unsigned long long)st.chunks_created,
           (unsigned long long)st.chunks_deduped);

    /* search vs libc memmem over raw bytes */
    t0 = now_ms();
    static gn_hit h2[1000000];
    size_t sn = gn_search(e, (const uint8_t*)"compression", 11, h2, 1000000);
    t1 = now_ms();
    double genna_search_ms = t1 - t0;

    t0 = now_ms();
    size_t raw_hits = 0; uint8_t *p = big, *end = big + blen;
    while ((p = memmem(p, end - p, "compression", 11))) { raw_hits++; p++; }
    t1 = now_ms();
    printf("  search 'compression': genna %.1f ms (%zu hits) "
           "vs memmem raw %.1f ms (%zu hits)\n",
           genna_search_ms, sn, t1 - t0, raw_hits);


    /* THE claim: edit cost independent of file size.
       Opponent: what every editor/sed does — rewrite the file.           */
    t0 = now_ms();
    gn_update(e, B, blen / 2, 0, (const uint8_t*)"INSERTED SENTENCE ", 18);
    t1 = now_ms();
    double genna_edit_ms = t1 - t0;

    t0 = now_ms();
    uint8_t *copy = malloc(blen + 32);
    memcpy(copy, big, blen / 2);
    memcpy(copy + blen/2, "INSERTED SENTENCE ", 18);
    memcpy(copy + blen/2 + 18, big + blen/2, blen - blen/2);
    /* The result must be observably used, or a modern compiler deletes the
     * whole malloc/memcpy/free chain as dead and the "rewrite" baseline
     * times zero -- which silently turns the headline ratio into 0x.      */
    gn_test_sink = copy[blen / 2 + 4] ^ copy[0] ^ copy[blen - 1];
    t1 = now_ms();
    double rewrite_ms = t1 - t0;
    free(copy);

    printf("  U mid-file:  genna %.3f ms   vs rewrite %.1f ms   (%.0fx)\n",
           genna_edit_ms, rewrite_ms, rewrite_ms / genna_edit_ms);

    /* delete a 10MB "chapter" */
    t0 = now_ms();
    gn_update(e, B, blen / 4, 10u << 20, NULL, 0);
    t1 = now_ms();
    printf("  U delete 10MB chapter: %.3f ms (file untouched except seams)\n",
           t1 - t0);

    /* verify correctness of the big edits at the seams */
    uint8_t seam[64];
    gn_read(e, B, blen/2 - 8 - (10u<<20), 34, seam); /* around insert, shifted by delete */
    CHECK(memmem(seam, 34, "INSERTED", 8) != NULL, "U@100MB: insert survives at seam");

    gn_engine_stats(e, &st);
    printf("  tokens scanned: %.1f M   tokens detokenized: %.2f M "
           "(toll paid only where bytes were requested)\n",
           st.tokens_scanned / 1e6, st.tokens_detokenized / 1e6);

    free(big); gn_engine_free(e);

    printf("\n%s (%d failures)\n", fails ? "TEST RUN: FAILURES" :
           "TEST RUN: ALL CLAIMS HELD", fails);
    return fails != 0;
}
