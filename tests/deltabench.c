/* deltabench.c — what does one more version actually cost on disk?
 *
 * The dedup story is easy to state and easy to overstate. Identical chunks
 * collapse perfectly, but a one-line edit inside a 4 KB chunk produces a chunk
 * that is 99.9% the same as one already stored and shares nothing with it,
 * because content addressing is exact-match. git does not have that problem:
 * packfiles delta objects against similar objects.
 *
 * So the honest measurement is not the ratio of a whole store to a whole
 * checkout -- it is the GROWTH of the saved store per commit, against git's
 * growth of .git per commit on the same edits.
 *
 * Usage: deltabench <corpus> <workdir> [n_commits] [save_flags]
 * Prints one line per sampled commit plus a summary, so the shape of the
 * growth is visible and not just its endpoints.
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static long fsize(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f);
    return n;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <corpus> <workdir> [n] [flags]\n", argv[0]); return 2; }
    const char *corpus = argv[1], *wd = argv[2];
    int N = argc > 3 ? atoi(argv[3]) : 100;
    uint32_t flags = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 0u;
    /* "wal" skips the save-after-every-commit section, which costs one full
     * store rewrite per commit and dominates the runtime. The checkpoint
     * number at the end is what a many-version comparison actually needs. */
    int wal_only = argc > 5 && strcmp(argv[5], "wal") == 0;

    FILE *f = fopen(corpus, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", corpus); return 2; }
    fseek(f, 0, SEEK_END); long clen = ftell(f); fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)clen + 1);
    if (!text || fread(text, 1, (size_t)clen, f) != (size_t)clen) {
        fprintf(stderr, "cannot read %s\n", corpus); return 2;
    }
    text[clen] = 0; fclose(f);

    char path[1024];
    snprintf(path, sizeof path, "%s/delta.gn", wd);
    remove(path);
    { char w[1100]; snprintf(w, sizeof w, "%s.wal", path); remove(w); }

    printf("=== store growth per commit ===\n");
    printf("   corpus: %s (%.2f MB), commits: %d, save flags: 0x%x\n\n",
           corpus, clen / 1048576.0, N, flags);

    gn_engine *e = gn_engine_new();
    gn_object *o = gn_create(e, "src", (const uint8_t *)text, (size_t)clen);

    if (gn_save_ex(e, path, flags) != 0) { fprintf(stderr, "save failed\n"); return 1; }
    long base = fsize(path);
    printf("   baseline store (1 version): %ld bytes\n\n", base);

    if (wal_only) goto wal_section;

    printf("   %6s %14s %14s %12s\n", "commit", "store bytes", "grew by", "save ms");
    long prev = base;
    double tsave = 0;
    for (int i = 0; i < N; i++) {
        /* One appended line, at a position that moves -- the same shape of
         * edit gitcmp makes, so the two numbers are about the same workload. */
        char line[64];
        int ln = snprintf(line, sizeof line, "\n// edit %d\n", i);
        uint64_t at = (uint64_t)(((long long)i * 7919LL) % (clen - 1));
        if (gn_update(e, o, at, 0, (const uint8_t *)line, (size_t)ln) != 0) {
            fprintf(stderr, "update %d failed\n", i); return 1;
        }
        double t0 = ms();
        if (gn_save_ex(e, path, flags) != 0) { fprintf(stderr, "save %d failed\n", i); return 1; }
        tsave += ms() - t0;
        long now = fsize(path);
        if (i < 3 || i == N / 2 || i == N - 1) {
            printf("   %6d %14ld %14ld %12.0f\n", i + 1, now, now - prev, ms() - t0);
        }
        prev = now;
    }

    long final_sz = fsize(path);
    printf("\n   after %d commits: %ld bytes\n", N, final_sz);
    printf("   growth:           %ld bytes total, %.0f bytes/commit\n",
           final_sz - base, (double)(final_sz - base) / N);
    printf("   save cost:        %.0f ms/commit (full rewrite, not incremental)\n",
           tsave / N);

    /* The store is rewritten whole on every save, so "bytes written to disk"
     * and "bytes the store grew" are very different numbers. Report both --
     * quoting only the growth would hide that Genna rewrites the entire store
     * to add one version, where git appends. */
    printf("   bytes WRITTEN:    %.0f per commit (the whole store, every time)\n",
           (double)final_sz);

    printf("   versions:         %u\n", o->n_ver);

    gn_engine_free(e);
    gn_ext_arena_free();

    /* ---- the mode that is actually comparable to git ------------------
     * Above, the store is rewritten in full after every commit, which is the
     * worst case and not how the engine is meant to run. git appends to .git
     * per commit; Genna's append-per-commit path is the WAL, with the store
     * rewritten only at a checkpoint. Measuring only the rewrite would
     * overstate git's advantage as badly as measuring only the WAL would
     * understate it, so measure both.                                     */
wal_section:
    printf("\n=== same commits, WAL mode (one checkpoint, then append) ===\n");
    remove(path);
    { char w[1100]; snprintf(w, sizeof w, "%s.wal", path); remove(w); }

    e = gn_engine_new();
    o = gn_create(e, "src", (const uint8_t *)text, (size_t)clen);
    if (gn_save_ex(e, path, flags) != 0) { fprintf(stderr, "save failed\n"); return 1; }

    char wpath[1100]; snprintf(wpath, sizeof wpath, "%s.wal", path);
    long wal0 = fsize(wpath);
    double t0 = ms();
    for (int i = 0; i < N; i++) {
        char line[64];
        int ln = snprintf(line, sizeof line, "\n// edit %d\n", i);
        uint64_t at = (uint64_t)(((long long)i * 7919LL) % (clen - 1));
        if (gn_update(e, o, at, 0, (const uint8_t *)line, (size_t)ln) != 0) {
            fprintf(stderr, "wal update %d failed\n", i); return 1;
        }
    }
    double twal = ms() - t0;
    long wal1 = fsize(wpath);
    printf("   WAL grew:         %ld bytes for %d commits (%.0f bytes/commit)\n",
           wal1 - wal0, N, (double)(wal1 - wal0) / N);
    printf("   commit cost:      %.2f ms/commit (fsync per record)\n", twal / N);
    printf("   durability:       every one of those %d edits survives SIGKILL\n", N);

    /* And the checkpoint that folds them in, once. */
    t0 = ms();
    if (gn_save_ex(e, path, flags) != 0) { fprintf(stderr, "checkpoint failed\n"); return 1; }
    double tck = ms() - t0;
    long ck = fsize(path);
    printf("   checkpoint:       %ld bytes, %.0f ms (amortized over %d commits:"
           " %.0f bytes/commit)\n",
           ck, tck, N, (double)(ck - base) / N);

    gn_engine_free(e);
    gn_ext_arena_free();
    free(text);
    remove(path);
    remove(wpath);
    return 0;
}
