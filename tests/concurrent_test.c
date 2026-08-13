/* concurrent_test.c — do two writers corrupt a store, or does one lose?
 *
 * Optimistic concurrency is only worth anything if it DETECTS the conflict.
 * A test that runs writers one after another proves nothing; these actually
 * race, N processes at once, all committing against the same base
 * generation.
 *
 * The invariants:
 *   1. Exactly one writer per generation wins. If two "succeed" against the
 *      same base, one silently overwrote the other -- the bug this exists to
 *      prevent.
 *   2. The losers are told (GN_CONFLICT), not silently discarded.
 *   3. The store is never corrupt afterwards: it opens, and it holds exactly
 *      the winner's data.
 *   4. A retry loop makes progress: with retries, all N writers' edits land.
 *
 * usage: concurrent_test writer <store> <id> <marker>
 *        concurrent_test parent <store> <corpus> <writers> <rounds>
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <unistd.h>
  #include <sys/wait.h>
#endif

static int fails = 0;
#define CHECK(c, ...) do { \
    if (c) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

/* ---- writer: load, edit, commit against the generation it loaded ------ */
static int run_writer(const char *store, int id, const char *marker) {
    uint64_t base = gn_store_generation(store);
    gn_engine *e = gn_open(store);
    if (!e) { fprintf(stderr, "w%d: open failed\n", id); return 3; }
    gn_object *o = gn_object_open(e, "ds");
    if (!o) { gn_close(e); return 3; }

    gn_update(e, o, 0, 0, (const uint8_t*)marker, strlen(marker));

    int rc = gn_commit(e, store, base);
    gn_close(e);
    /* 0 = won, GN_CONFLICT = lost the race (expected, not an error) */
    if (rc == 0) return 0;
    if (rc == GN_CONFLICT) return 10;
    if (rc == GN_LOCKED) return 11;
    return 3;
}

/* ---- spawn N writers simultaneously ----------------------------------- */
#if defined(_WIN32)
typedef PROCESS_INFORMATION proc;
static int spawn(proc *p, const char *exe, const char *store, int id,
                 const char *marker) {
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\" writer \"%s\" %d \"%s\"",
             exe, store, id, marker);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(p, 0, sizeof *p);
    return CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, p)
           ? 0 : -1;
}
static int reap(proc *p) {
    WaitForSingleObject(p->hProcess, INFINITE);
    DWORD code = 3;
    GetExitCodeProcess(p->hProcess, &code);
    CloseHandle(p->hProcess); CloseHandle(p->hThread);
    return (int)code;
}
#else
typedef pid_t proc;
static int spawn(proc *p, const char *exe, const char *store, int id,
                 const char *marker) {
    char sid[16]; snprintf(sid, sizeof sid, "%d", id);
    pid_t c = fork();
    if (c < 0) return -1;
    if (c == 0) { execl(exe, exe, "writer", store, sid, marker, (char*)NULL);
                  _exit(3); }
    *p = c; return 0;
}
static int reap(proc *p) {
    int st = 0; waitpid(*p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}
#endif

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s + 1);
    if (fread(b, 1, (size_t)s, f) != (size_t)s) exit(2);
    fclose(f); *n = (size_t)s; return b;
}

static int count_markers(const char *store, int nw) {
    gn_engine *e = gn_open(store);
    if (!e) return -1;
    gn_object *o = gn_object_open(e, "ds");
    uint64_t len = o->ver[o->n_ver - 1].total_bytes;
    uint8_t *buf = malloc((size_t)len + 1);
    gn_read(e, o, 0, (size_t)len, buf);
    buf[len] = 0;
    int found = 0;
    for (int i = 0; i < nw; i++) {
        char m[32]; snprintf(m, sizeof m, "<W%d>", i);
        if (strstr((char*)buf, m)) found++;
    }
    free(buf);
    gn_close(e);
    return found;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "writer") == 0)
        return run_writer(argv[2], atoi(argv[3]), argv[4]);

    if (argc != 6 || strcmp(argv[1], "parent") != 0) {
        fprintf(stderr, "usage: %s parent <store> <corpus> <writers> <rounds>\n",
                argv[0]);
        return 2;
    }
    const char *store = argv[2], *corpus = argv[3];
    int NW = atoi(argv[4]), ROUNDS = atoi(argv[5]);

    printf("=== optimistic concurrency: %d writers racing, %d rounds ===\n",
           NW, ROUNDS);

    /* fresh store */
    { char t[1024];
      remove(store);
      snprintf(t, sizeof t, "%s.wal", store);  remove(t);
      snprintf(t, sizeof t, "%s.lock", store); remove(t);
      size_t n; uint8_t *txt = slurp(corpus, &n);
      gn_engine *e = gn_engine_new();
      gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
      gn_create(e, "ds", txt, n);
      free(txt);
      if (gn_save(e, store) != 0) { perror("setup"); return 2; }
      gn_close(e);
    }

    int total_won = 0, total_conflict = 0, total_locked = 0, total_err = 0;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t base = gn_store_generation(store);
        proc ps[64];
        char marks[64][32];
        int nw = NW > 64 ? 64 : NW;
        for (int i = 0; i < nw; i++) {
            snprintf(marks[i], sizeof marks[i], "<W%d>", r * nw + i);
            if (spawn(&ps[i], argv[0], store, r * nw + i, marks[i]) != 0) {
                printf("  spawn failed\n"); return 2;
            }
        }
        int won = 0, conflict = 0, locked = 0, err = 0;
        for (int i = 0; i < nw; i++) {
            int rc = reap(&ps[i]);
            if (rc == 0) won++;
            else if (rc == 10) conflict++;
            else if (rc == 11) locked++;
            else err++;
        }
        uint64_t after = gn_store_generation(store);
        printf("  round %d: base gen %llu -> %llu | won %d, conflict %d, "
               "locked %d, error %d\n", r,
               (unsigned long long)base, (unsigned long long)after,
               won, conflict, locked, err);

        CHECK(err == 0, "    no writer failed unexpectedly");
        CHECK(won <= 1, "    at most ONE writer committed against gen %llu "
                        "(got %d) - more would mean a silent overwrite",
              (unsigned long long)base, won);
        CHECK(won + conflict + locked == nw,
              "    every writer got a definite answer (%d+%d+%d of %d)",
              won, conflict, locked, nw);
        if (won == 1)
            CHECK(after == base + 1, "    generation advanced exactly once");

        total_won += won; total_conflict += conflict;
        total_locked += locked; total_err += err;

        gn_engine *e = gn_open(store);
        CHECK(e != NULL, "    store still opens after the race");
        if (e) gn_close(e);
    }

    printf("\n  totals: %d committed, %d conflicted, %d lock-timeout, %d errors\n",
           total_won, total_conflict, total_locked, total_err);
    int found = count_markers(store, NW * ROUNDS);
    CHECK(found == total_won,
          "    the store contains exactly the %d winning edits (found %d) "
          "- no lost update, no phantom", total_won, found);

    /* ---- retry loop: does the protocol make progress? ------------------ */
    printf("\n  -- with a retry loop, every writer should eventually land --\n");
    int landed = 0;
    for (int i = 0; i < NW; i++) {
        char m[32]; snprintf(m, sizeof m, "<R%d>", i);
        for (int attempt = 0; attempt < 50; attempt++) {
            uint64_t base = gn_store_generation(store);
            gn_engine *e = gn_open(store);
            if (!e) break;
            gn_object *o = gn_object_open(e, "ds");
            gn_update(e, o, 0, 0, (const uint8_t*)m, strlen(m));
            int rc = gn_commit(e, store, base);
            gn_close(e);
            if (rc == 0) { landed++; break; }
            if (rc != GN_CONFLICT && rc != GN_LOCKED) break;
        }
    }
    CHECK(landed == NW, "    all %d writers landed via retry (%d)", NW, landed);

    printf("\n%s (%d failures)\n",
           fails ? "CONCURRENCY: FAILURES" : "CONCURRENCY: ALL PASS", fails);
    return fails != 0;
}
