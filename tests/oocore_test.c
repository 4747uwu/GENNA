/* oocore_test.c — does mapping a store actually remove the RAM ceiling?
 *
 * Two things have to hold or the feature is worthless:
 *
 *   1. Reads through the mapping are byte-identical to reads from a heap
 *      store. Pointing gn_tok* at file bytes is exactly the sort of thing
 *      that silently corrupts on an alignment or endianness mistake.
 *
 *   2. Resident memory is actually lower. The whole point is that a store
 *      larger than RAM can be opened; if the pages are all faulted in
 *      anyway, nothing was gained.
 *
 * Both are measured against the normal compressed store on the same data.
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include "../include/genna_bin.h"
#include "../include/genna_agg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <psapi.h>
static size_t rss_bytes(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return (size_t)pmc.WorkingSetSize;
    return 0;
}
#elif defined(__APPLE__)
  /* macOS has no /proc. The old code fopen'd /proc/self/statm, got NULL, and
   * returned 0 -- so every resident measurement on this platform was zero,
   * silently. That is the house bug in miniature: a measurement that cannot
   * be taken returned a number that looks like one, and the mmap claim
   * ("0.7 MB resident vs 32.6 MB") has therefore never been measured on a
   * Mac at all. */
  #include <unistd.h>
  #include <mach/mach.h>
static size_t rss_bytes(void) {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS)
        return (size_t)info.resident_size;
    return 0;
}
#else
  #include <unistd.h>
static size_t rss_bytes(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0, res = 0;
    if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0;
    fclose(f);
    return (size_t)res * (size_t)sysconf(_SC_PAGESIZE);
}
#endif

/* rss_bytes() returning 0 always means "could not measure", never "measured
 * zero" -- a live process has resident pages by definition. The assertions
 * below check for it explicitly rather than comparing zeros, because an
 * unmeasurable platform producing "0.0 MB vs 0.0 MB" is how a broken probe
 * disguises itself as a broken engine. */

static int fails = 0;
#define CHECK(c, ...) do { \
    if (c) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

static double ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* Fresh-process probe: open a store and print its resident cost.
 *
 * This exists because an in-process RSS delta is not trustworthy after the
 * test has already opened and freed other engines -- the working set stays
 * high, so a later open appears to cost almost nothing. Measured that way an
 * aggregate-attached open looked LIGHTER than the plain one, which is
 * impossible. Each measurement that matters gets its own process. */
static int probe_main(const char *path, int with_agg) {
    size_t base = rss_bytes();
    if (with_agg) {
        gn_engine *tmp = gn_engine_new();
        gn_agg_attach(tmp, GN_AGG_MAX);
        gn_engine_free(tmp);
        gn_ext_arena_free();
        base = rss_bytes();
    }
    double t0 = ms();
    gn_engine *e = gn_open(path);
    double t = ms() - t0;
    size_t rss = rss_bytes() - base;
    if (!e) { printf("PROBE fail\n"); return 1; }
    printf("PROBE %zu %.0f\n", rss, t);
    gn_engine_free(e);
    gn_ext_arena_free();
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* so a crash does not eat the log */
    if (argc > 3 && strcmp(argv[1], "probe") == 0)
        return probe_main(argv[2], strcmp(argv[3], "agg") == 0);
    const char *dir = argc > 1 ? argv[1] : ".";
    size_t MB = argc > 2 ? (size_t)atoi(argv[2]) : 192;
    int keep = argc > 3 && strcmp(argv[3], "keep") == 0;
    size_t n = MB << 20;

    char p_map[1024], p_cmp[1024];
    snprintf(p_map, sizeof p_map, "%s/oo_mapped.gn", dir);
    snprintf(p_cmp, sizeof p_cmp, "%s/oo_compressed.gn", dir);

    printf("=== out-of-core: mapped vs heap-resident store ===\n");
    printf("   payload: %zu MB of binary data\n\n", MB);

    /* ---- build the two stores from identical data ------------------- */
    uint8_t *data = malloc(n);
    if (!data) { printf("  cannot allocate %zu MB\n", MB); return 2; }
    /* Genuinely non-repeating. An earlier version used (i*2654435761)>>13,
     * which repeats often enough that CDC deduplicated 192 MB down to 8 MB
     * of unique chunks -- so there was no memory pressure to relieve and the
     * measurement showed nothing. Incompressible data is the honest case for
     * an out-of-core claim. */
    uint64_t st = 0x243F6A8885A308D3ULL;
    for (size_t i = 0; i < n; i++) {
        st ^= st << 13; st ^= st >> 7; st ^= st << 17;
        data[i] = (uint8_t)(st >> 24);
    }

    {
        gn_engine *e = gn_engine_new();
        gn_bin_opts o; gn_bin_opts_default(&o); o.avg_chunk = 4096;
        gn_create_binary(e, "big", data, n, &o);
        double t0 = ms();
        errno = 0;
        int rc = gn_save_ex(e, p_map, GN_SAVE_MAPPABLE);
        int se = errno;
        double t_map = ms() - t0;
        CHECK(rc == 0, "mappable store written (%.0f ms)%s%s", t_map,
              rc == 0 ? "" : " errno=", rc == 0 ? "" : strerror(se));
        t0 = ms();
        rc = gn_save(e, p_cmp);
        double t_cmp = ms() - t0;
        CHECK(rc == 0, "compressed store written (%.0f ms)", t_cmp);
        gn_engine_free(e);
        gn_ext_arena_free();
    }

    FILE *f;
    long sz_map = 0, sz_cmp = 0;
    if ((f = fopen(p_map, "rb"))) { fseek(f, 0, SEEK_END); sz_map = ftell(f); fclose(f); }
    if ((f = fopen(p_cmp, "rb"))) { fseek(f, 0, SEEK_END); sz_cmp = ftell(f); fclose(f); }
    printf("   on disk: mappable %.1f MB, compressed %.1f MB "
           "(compression forfeited: %.2fx bigger)\n",
           sz_map / 1048576.0, sz_cmp / 1048576.0,
           sz_cmp ? (double)sz_map / sz_cmp : 0.0);

    /* ---- open the compressed one, measure resident ------------------- */
    size_t base = rss_bytes();
    double t0 = ms();
    gn_engine *ec = gn_open(p_cmp);
    double t_open_c = ms() - t0;
    CHECK(ec != NULL, "compressed store opens");
    if (!ec) { printf("\nOUT-OF-CORE: FAILURES (%d)\n", fails); return 1; }
    size_t rss_c = rss_bytes() - base;
    CHECK(gn_store_is_mapped(ec) == 0, "compressed store is NOT mapped");

    gn_object *oc = gn_object_open(ec, "big");
    uint8_t *buf1 = malloc(1 << 20), *buf2 = malloc(1 << 20);
    size_t g1 = gn_read(ec, oc, n / 3, 1 << 20, buf1);
    CHECK(g1 == (1u << 20) && memcmp(buf1, data + n / 3, g1) == 0,
          "compressed store reads correctly");
    gn_engine_free(ec);
    gn_ext_arena_free();

    /* ---- open the mapped one, measure resident ----------------------- */
    size_t base2 = rss_bytes();
    t0 = ms();
    gn_engine *em = gn_open(p_map);
    double t_open_m = ms() - t0;
    CHECK(em != NULL, "mappable store opens");
    /* Bail rather than dereference NULL. An earlier version carried on and
     * segfaulted, which reported a crash where the real news was one clear
     * failure line above it. */
    if (!em) { printf("\nOUT-OF-CORE: FAILURES (%d)\n", fails); return 1; }
    size_t rss_m = rss_bytes() - base2;
    CHECK(gn_store_is_mapped(em) == 1,
          "mapped store reports mapped, %.1f MB of chunk data borrowed",
          gn_store_mapped_bytes(em) / 1048576.0);

    gn_object *om = gn_object_open(em, "big");

    /* the claim that matters: identical bytes through the mapping */
    size_t g2 = gn_read(em, om, n / 3, 1 << 20, buf2);
    CHECK(g2 == g1 && memcmp(buf1, buf2, g1) == 0,
          "1 MB read through the mapping is byte-identical to the heap store");

    int bad = 0;
    for (int i = 0; i < 40; i++) {
        uint64_t off = ((uint64_t)i * 7919u * 4096u) % (n - 65536);
        size_t got = gn_read(em, om, off, 65536, buf2);
        if (got != 65536 || memcmp(buf2, data + off, 65536) != 0) bad++;
    }
    CHECK(bad == 0, "40 scattered 64 KB reads all byte-exact through the map");

    /* editing a mapped store must still work: new chunks are heap-owned
     * while the untouched ones stay borrowed */
    int rc = gn_update(em, om, n / 2, 16, (const uint8_t *)"MAPPED-EDIT-OK!!", 16);
    CHECK(rc == 0, "a mapped store is still writable");
    size_t got = gn_read(em, om, n / 2, 16, buf2);
    CHECK(got == 16 && memcmp(buf2, "MAPPED-EDIT-OK!!", 16) == 0,
          "the edit reads back from the mapped store");
    CHECK(gn_read(em, om, 0, 4096, buf2) == 4096 &&
          memcmp(buf2, data, 4096) == 0,
          "untouched data is still exact after editing a mapped store");

    printf("\n   resident after open:\n");
    printf("     compressed store : %8.1f MB   (open %.0f ms)\n",
           rss_c / 1048576.0, t_open_c);
    printf("     mapped store     : %8.1f MB   (open %.0f ms)\n",
           rss_m / 1048576.0, t_open_m);
    printf("     payload          : %8.1f MB\n", n / 1048576.0);

    CHECK(rss_c > 0 && rss_m > 0,
          "resident memory is actually measurable here (compressed %zu B, "
          "mapped %zu B) - a 0 means the probe could not read RSS, and "
          "comparing zeros would be meaningless either way", rss_c, rss_m);
    CHECK(rss_m * 2 < rss_c,
          "mapped open is at least 2x lighter (%.1f MB vs %.1f MB, %.1fx)",
          rss_m / 1048576.0, rss_c / 1048576.0,
          rss_c ? (double)rss_c / (rss_m ? rss_m : 1) : 0.0);
    CHECK(t_open_m < t_open_c,
          "mapped open is faster (%.0f ms vs %.0f ms) - no decompress, no copy",
          t_open_m, t_open_c);

    gn_engine_free(em);
    gn_ext_arena_free();

    /* --- what a node aggregate costs out-of-core -----------------------
     * These two features pull against each other and it is worth a number
     * rather than a warning. With a monoid registered, every leaf built at
     * load time computes an aggregate, which READS that leaf's chunk tokens
     * -- and on a mapped store, reading is faulting. Measure the same open
     * with a monoid attached and report how much of the mapping win is left.
     */
    /* Launched by tools/run_oocore.sh as two `oo.exe probe ...` runs, which
     * compares them. Doing it from here would mean system() -> cmd.exe, which
     * cannot parse the MSYS paths this test runs under. */

    /* --- the integrity that open deliberately does NOT check -----------
     * Skipping the bulk CRC at open is what buys the RAM win. It is only an
     * honest trade if the check still exists and actually catches damage, so
     * verify both directions: intact passes, one flipped bit fails. */
    CHECK(gn_verify_chunks(p_map) == 0, "gn_verify_chunks accepts an intact mapped store");
    CHECK(gn_verify_chunks(p_cmp) == 1, "a compressed store reports no separate bulk (checked at open)");

    {
        /* flip one bit deep in the bulk region */
        FILE *fx = fopen(p_map, "r+b");
        int flipped = 0;
        if (fx) {
            long at = (long)(sz_map / 2);
            if (fseek(fx, at, SEEK_SET) == 0) {
                int c = fgetc(fx);
                if (c != EOF && fseek(fx, at, SEEK_SET) == 0) {
                    fputc(c ^ 0x01, fx); flipped = 1;
                }
            }
            fclose(fx);
        }
        CHECK(flipped && gn_verify_chunks(p_map) == -1,
              "gn_verify_chunks catches a single flipped bit in the bulk");
        /* ... and gn_open still succeeds on it, which is exactly the gap
         * being documented: mapped opens trade this check for the RAM. */
        gn_engine *eb = gn_open(p_map);
        CHECK(eb != NULL,
              "a mapped store with corrupt bulk still OPENS (the documented gap)");
        if (eb) { gn_engine_free(eb); gn_ext_arena_free(); }
    }
    free(buf1); free(buf2); free(data);
    if (!keep) { remove(p_map); remove(p_cmp); }
    if (!keep) { char w[1100];
      snprintf(w, sizeof w, "%s.wal", p_map); remove(w);
      snprintf(w, sizeof w, "%s.wal", p_cmp); remove(w); }

    printf("\n%s (%d failures)\n",
           fails ? "OUT-OF-CORE: FAILURES" : "OUT-OF-CORE: ALL PASS", fails);
    return fails != 0;
}
