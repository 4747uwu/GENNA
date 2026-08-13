/* crash_test.c — kill the process mid-edit; prove nothing is lost or broken.
 *
 * A child process edits a persistent store in a tight loop. The parent kills
 * it dead at an unpredictable moment -- TerminateProcess on Windows, SIGKILL
 * on POSIX; both are uncatchable, run no cleanup, and flush no buffers -- and
 * then reopens the store and checks two things:
 *
 *   NO CORRUPTION     gn_open() succeeds and every recovered version reads
 *                     back byte-for-byte equal to a reference engine that
 *                     replayed the same deterministic edits with no crash.
 *
 *   NO LOST COMMITS   the child fsyncs a witness line after each gn_update
 *                     RETURNS. Every edit the witness records must be present
 *                     after recovery. Recovery may contain at most one more
 *                     (the edit that was in flight when the axe fell) -- it
 *                     must never contain fewer, and never a torn one.
 *
 * The child also checkpoints (gn_save) periodically, so some kills land in
 * the middle of writing a snapshot. That is the case the temp-file + atomic
 * rename exists for.
 *
 * usage: crash_test child  <store> <witness> <corpus>
 *        crash_test parent <store> <witness> <corpus> <rounds> <ms>
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #define SYNCFD(fd) _commit(fd)
  #define FD_OF(f)   _fileno(f)
#else
  #include <unistd.h>
  #include <signal.h>
  #include <sys/wait.h>
  #include <sys/types.h>
  #define SYNCFD(fd) fsync(fd)
  #define FD_OF(f)   fileno(f)
#endif

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

#define CHECKPOINT_EVERY 25

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long s = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)s + 1);
    if (fread(b, 1, (size_t)s, f) != (size_t)s) exit(2);
    fclose(f); *n = (size_t)s; return b;
}

/* THE deterministic edit schedule. Both the crashing child and the clean
 * reference engine run exactly this, so their outputs must agree.          */
static void do_edit(gn_engine *e, gn_object *o, int i) {
    gn_version *cur = &o->ver[o->n_ver - 1];
    uint64_t tb = cur->total_bytes;
    if (tb < 128) return;
    uint64_t s = 0x9E3779B97F4A7C15ULL * (uint64_t)(i + 1);
    s ^= s >> 29; s *= 0xBF58476D1CE4E5B9ULL; s ^= s >> 32;
    uint64_t at = s % (tb - 64);
    char ins[64];
    int il = snprintf(ins, sizeof ins, "{crash-edit %d}", i);
    if (i % 3 == 0)      gn_update(e, o, at, 0, (const uint8_t*)ins, (size_t)il);
    else if (i % 3 == 1) gn_update(e, o, at, 13, NULL, 0);
    else                 gn_update(e, o, at, 7, (const uint8_t*)ins, (size_t)il);
}

/* ------------------------------------------------------------------ */
static int run_child(const char *store, const char *witness, const char *corpus) {
    gn_engine *e = gn_open(store);
    gn_object *o = NULL;
    if (!e) {
        /* first run: ingest and establish the snapshot the WAL extends */
        size_t n; uint8_t *txt = slurp(corpus, &n);
        e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 6, 200000, 16);
        o = gn_create(e, "ds", txt, n);
        free(txt);
        if (gn_save(e, store) != 0) { perror("gn_save"); return 2; }
    } else {
        o = gn_object_open(e, "ds");
        if (!o) { fprintf(stderr, "child: object missing\n"); return 2; }
    }

    FILE *wf = fopen(witness, "ab");
    if (!wf) { perror(witness); return 2; }

    /* v0 is the ingest, so edits already applied = n_ver-1 */
    int i = (int)o->n_ver - 1;
    for (;;) {
        do_edit(e, o, i);
        /* the edit has RETURNED -- from here on it must survive a kill */
        fprintf(wf, "%d\n", i);
        fflush(wf); SYNCFD(FD_OF(wf));
        i++;
        if (i % CHECKPOINT_EVERY == 0) {
            /* kills landing in here test the atomic-rename path */
            if (gn_save(e, store) != 0) { perror("checkpoint"); return 2; }
        }
    }
}

/* count of witnessed (returned) edits = highest index + 1 */
static int witness_count(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int last = -1, v;
    while (fscanf(f, "%d", &v) == 1) last = v;
    fclose(f);
    return last + 1;
}

/* ------------------------------------------------------------------ */
#if defined(_WIN32)
typedef PROCESS_INFORMATION childh;
static int spawn_child(childh *h, const char *exe, const char *store,
                       const char *witness, const char *corpus) {
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "\"%s\" child \"%s\" \"%s\" \"%s\"",
             exe, store, witness, corpus);
    STARTUPINFOA si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(h, 0, sizeof *h);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, h)) {
        fprintf(stderr, "CreateProcess failed: %lu\n", (unsigned long)GetLastError());
        return -1;
    }
    return 0;
}
/* TerminateProcess is the SIGKILL analogue: the process stops executing at
 * once, no handlers run, no CRT buffer is flushed. Anything already handed
 * to the OS survives; anything still in user-space buffers does not.       */
static void kill_child(childh *h, unsigned ms) {
    Sleep(ms);
    TerminateProcess(h->hProcess, 9);
    WaitForSingleObject(h->hProcess, INFINITE);
    CloseHandle(h->hProcess); CloseHandle(h->hThread);
}
#else
typedef pid_t childh;
static int spawn_child(childh *h, const char *exe, const char *store,
                       const char *witness, const char *corpus) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execl(exe, exe, "child", store, witness, corpus, (char*)NULL);
        _exit(127);
    }
    *h = p; return 0;
}
static void kill_child(childh *h, unsigned ms) {
    usleep(ms * 1000);
    kill(*h, SIGKILL);
    int st; waitpid(*h, &st, 0);
}
#endif

/* Build the crash-free reference: same corpus, same edits, no WAL.        */
static gn_engine *reference(const char *corpus, int edits, gn_object **out) {
    size_t n; uint8_t *txt = slurp(corpus, &n);
    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 6, 200000, 16);
    gn_object *o = gn_create(e, "ds", txt, n);
    free(txt);
    for (int i = 0; i < edits; i++) do_edit(e, o, i);
    *out = o;
    return e;
}

static int run_parent(const char *exe, const char *store, const char *witness,
                      const char *corpus, int rounds, int base_ms) {
    remove(store); remove(witness);
    { char w[1024]; snprintf(w, sizeof w, "%s.wal", store); remove(w); }

    /* Establish the initial snapshot here, so every kill below lands inside
     * the edit loop rather than inside the first ingest (which takes longer
     * than the kill delay and would just test "file does not exist yet").
     * Kills during a snapshot write are still covered: the child checkpoints
     * every CHECKPOINT_EVERY edits.                                        */
    {
        size_t n; uint8_t *txt = slurp(corpus, &n);
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 6, 200000, 16);
        gn_create(e, "ds", txt, n);
        free(txt);
        if (gn_save(e, store) != 0) { perror("setup gn_save"); return 2; }
        gn_close(e);
        printf("  setup: ingested %s and saved the initial snapshot\n", corpus);
    }

    for (int r = 0; r < rounds; r++) {
        unsigned ms = (unsigned)(base_ms + (r * 137) % 400);
        childh h;
        if (spawn_child(&h, exe, store, witness, corpus) != 0) return 2;
        kill_child(&h, ms);

        int w = witness_count(witness);

        gn_engine *e = gn_open(store);
        if (!e) {
            printf("  FAIL  round %d: gn_open failed after kill: %s\n", r, strerror(errno));
            fails++; return 1;
        }
        gn_object *o = gn_object_open(e, "ds");
        if (!o) { printf("  FAIL  round %d: object lost after kill\n", r); fails++; gn_close(e); return 1; }

        int recovered = (int)o->n_ver - 1;
        uint64_t replayed = gn_wal_replayed(e);

        /* NO LOST COMMITS: everything the child said it finished is here. */
        int ok_commits = (recovered >= w) && (recovered <= w + 1);

        /* NO CORRUPTION: every version equals the crash-free reference.   */
        gn_object *ro = NULL;
        gn_engine *ref = reference(corpus, recovered, &ro);
        int bad = 0; uint64_t cmp = 0;
        if ((int)ro->n_ver != recovered + 1) {
            printf("  FAIL  round %d: reference has %u versions, recovered %d\n",
                   r, ro->n_ver, recovered + 1);
            bad = -1;
        } else {
            for (uint32_t v = 0; v < ro->n_ver; v++) {
                uint64_t la = o->ver[v].total_bytes, lb = ro->ver[v].total_bytes;
                uint8_t *A = malloc((size_t)la + 16), *B = malloc((size_t)lb + 16);
                size_t ga = gn_read_version(e, o, v, 0, (size_t)la, A);
                size_t gb = gn_read_version(ref, ro, v, 0, (size_t)lb, B);
                if (ga != gb || memcmp(A, B, ga) != 0) bad++;
                cmp += ga;
                free(A); free(B);
            }
        }

        printf("  round %d: killed after %u ms | witnessed %d, recovered %d, "
               "replayed %llu WAL records\n", r, ms, w, recovered,
               (unsigned long long)replayed);
        CHECK(ok_commits, "    no lost commits (recovered %d >= witnessed %d)", recovered, w);
        CHECK(bad == 0, "    no corruption: %u/%u versions byte-exact vs crash-free "
                        "reference (%.1f MB)", ro->n_ver - (bad > 0 ? bad : 0),
                        ro->n_ver, cmp / 1048576.0);

        gn_close(ref);
        gn_close(e);
        if (fails) return 1;
    }

    /* ---- forensics: prove the WAL is what is doing the work -------------
     * Everything above would also pass if the snapshot alone happened to
     * hold all the edits. Two negative controls settle it.                */
    char wpath[1024]; snprintf(wpath, sizeof wpath, "%s.wal", store);
    printf("\n  -- WAL forensics --\n");

    int with_wal;
    { gn_engine *e = gn_open(store); with_wal = (int)gn_object_open(e, "ds")->n_ver - 1; gn_close(e); }

    /* (1) a torn tail must be discarded, not misread as an edit */
    {
        FILE *f = fopen(wpath, "ab");
        /* a plausible-looking but incomplete record: length prefix, no body */
        uint8_t junk[12] = { 0xFF,0x00,0x00,0x00, 0xDE,0xAD,0xBE,0xEF, 1,2,3,4 };
        fwrite(junk, 1, sizeof junk, f); fclose(f);
        gn_engine *e = gn_open(store);
        CHECK(e != NULL, "    torn tail appended: store still opens");
        if (e) {
            gn_object *o = gn_object_open(e, "ds");
            int n = (int)o->n_ver - 1;
            CHECK(n == with_wal, "    torn tail ignored, not misread "
                                 "(%d edits, same as before)", n);
            /* And edits made AFTER recovering from a tear must themselves
             * survive. If the log were appended to past the torn bytes, the
             * next open would stop at the tear and lose all of these. */
            for (int k = 0; k < 6; k++) do_edit(e, o, n + k);
            gn_close(e);

            gn_engine *e2 = gn_open(store);
            int n2 = e2 ? (int)gn_object_open(e2, "ds")->n_ver - 1 : -1;
            CHECK(n2 == n + 6, "    6 edits made after recovering from a torn "
                               "tail survive the next open (%d -> %d)", n, n2);
            if (e2) gn_close(e2);
            with_wal = n2;
        }
    }

    /* (2) remove the log entirely: the un-checkpointed edits must vanish,
     *     landing us exactly on the last snapshot. If this number did NOT
     *     drop, the WAL was never carrying anything.                      */
    {
        char save[1100]; snprintf(save, sizeof save, "%s.bak", wpath);
        remove(save); rename(wpath, save);
        gn_engine *e = gn_open(store);
        CHECK(e != NULL, "    store opens with no WAL at all");
        if (e) {
            int n = (int)gn_object_open(e, "ds")->n_ver - 1;
            CHECK(n < with_wal, "    without the WAL we fall back to the last "
                                "checkpoint: %d edits vs %d with the log "
                                "(the log was carrying %d)", n, with_wal, with_wal - n);
            gn_close(e);
        }
        remove(wpath); rename(save, wpath);
    }

    /* (3) Opening a store whose log is MISSING must rebuild a usable log,
     *     not create a headerless one. Appending to a headerless (or
     *     stale-generation) log loses every edit written to it, silently,
     *     at the next open -- which looks exactly like working software
     *     until the day it matters. */
    {
        remove(wpath);
        gn_engine *e = gn_open(store);
        int n0 = e ? (int)gn_object_open(e, "ds")->n_ver - 1 : -1;
        CHECK(e && gn_wal_active(e), "    log missing: reopened with logging re-armed");
        if (e) {
            gn_object *o = gn_object_open(e, "ds");
            for (int k = 0; k < 5; k++) do_edit(e, o, n0 + k);
            gn_close(e);

            gn_engine *e2 = gn_open(store);
            int n1 = e2 ? (int)gn_object_open(e2, "ds")->n_ver - 1 : -1;
            CHECK(n1 == n0 + 5, "    5 edits made after the log was rebuilt "
                                "survive the next open (%d -> %d)", n0, n1);
            if (e2) gn_close(e2);
        }
    }

    /* (4) Same trap via a STALE generation: a log left over from an older
     *     snapshot must be replaced, not appended to. */
    {
        /* forge a log header carrying a generation that cannot match */
        FILE *f = fopen(wpath, "wb");
        uint8_t h[32]; memset(h, 0, sizeof h);
        memcpy(h, "GENNAwal", 8);
        h[8] = 1;                                  /* format version 1 */
        h[16] = 0xEE; h[17] = 0xEE;                /* absurd generation */
        /* the header CRC is wrong too: either way it must be unusable */
        fwrite(h, 1, sizeof h, f); fclose(f);

        gn_engine *e2 = gn_open(store);
        CHECK(e2 && gn_wal_active(e2), "    stale log: reopened with logging re-armed");
        if (e2) {
            /* Baseline is taken AFTER this open, not before: discarding an
             * unusable log correctly drops whatever it was carrying, so the
             * question is only whether edits made from here survive.      */
            gn_object *o = gn_object_open(e2, "ds");
            int base = (int)o->n_ver - 1;
            for (int k = 0; k < 4; k++) do_edit(e2, o, base + k);
            gn_close(e2);
            gn_engine *e3 = gn_open(store);
            int n1 = e3 ? (int)gn_object_open(e3, "ds")->n_ver - 1 : -1;
            CHECK(n1 == base + 4, "    4 edits after a stale log survive the "
                                  "next open (%d -> %d)", base, n1);
            if (e3) gn_close(e3);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "child") == 0)
        return run_child(argv[2], argv[3], argv[4]);
    if (argc == 7 && strcmp(argv[1], "parent") == 0) {
        printf("=== crash recovery: SIGKILL mid-edit ===\n");
        int rc = run_parent(argv[0], argv[2], argv[3], argv[4],
                            atoi(argv[5]), atoi(argv[6]));
        printf("\n%s (%d failures)\n",
               fails ? "CRASH RECOVERY: FAILURES" : "CRASH RECOVERY: SURVIVED", fails);
        return rc || fails;
    }
    fprintf(stderr, "usage: %s parent <store> <witness> <corpus> <rounds> <ms>\n", argv[0]);
    return 2;
}
