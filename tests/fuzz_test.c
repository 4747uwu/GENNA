/* fuzz_test.c — randomized differential testing against a dumb shadow buffer.
 *
 * The engine is clever: treaps, structural sharing, token seams, chunk reuse.
 * The shadow is a plain byte array spliced with memmove. After every single
 * operation the two must agree exactly. Any disagreement is an engine bug,
 * and the seed that produced it reproduces it.
 *
 * Four fuzzers, because the bugs this codebase has actually had lived in
 * four different places:
 *
 *   splice   2000 random inserts/deletes/replaces, full byte compare after
 *            each one. Boundary offsets (0, len, inside a multi-byte token)
 *            are drawn deliberately, not just uniformly.
 *   search   random needles taken from the live text, plus needles that
 *            cannot occur; hit counts compared against brute force.
 *   gc       edits interleaved with history trims, then a full refcount
 *            audit of the sharing DAG, then a check that every node is
 *            reclaimed once the last version is dropped.
 *   persist  save/reopen at random points mid-stream; the reloaded engine
 *            must still match the shadow AND every historical version hash.
 *
 * usage: fuzz_test [seed] [iterations]
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include "gn_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---- rng ------------------------------------------------------------- */
static uint64_t RS = 88172645463325252ULL;
static uint64_t rnd(void) {
    RS ^= RS << 13; RS ^= RS >> 7; RS ^= RS << 17; return RS;
}
static uint64_t rnd_n(uint64_t n) { return n ? rnd() % n : 0; }

/* ---- shadow: the obvious, slow, obviously-correct implementation ------ */
typedef struct { uint8_t *b; size_t n, cap; } shadow;

static void sh_init(shadow *s, const uint8_t *t, size_t n) {
    s->cap = n * 2 + 4096; s->b = malloc(s->cap);
    memcpy(s->b, t, n); s->n = n;
}
static void sh_splice(shadow *s, size_t off, size_t del,
                      const uint8_t *ins, size_t ilen) {
    if (off > s->n) return;
    if (off + del > s->n) del = s->n - off;
    size_t need = s->n - del + ilen;
    if (need + 1 > s->cap) {
        while (s->cap < need + 1) s->cap *= 2;
        s->b = realloc(s->b, s->cap);
    }
    memmove(s->b + off + ilen, s->b + off + del, s->n - off - del);
    if (ilen) memcpy(s->b + off, ins, ilen);
    s->n = need;
}
static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* a deterministic, tokenizer-hostile corpus: mixed word/punct/utf8/digits */
static size_t gen_corpus(uint8_t *out, size_t target) {
    static const char *W[] = {
        "the","engine","holds","data","below","layer","compression","token",
        "chunk","search","genna","system","memory","query","index","version",
        "structure","model","language","immutable","caf\xc3\xa9","na\xc3\xafve",
        "42","0xDEADBEEF","{\"k\":","}","...","\t","  ","\n\n"
    };
    size_t n = 0;
    while (n + 32 < target) {
        const char *w = W[rnd_n(28)];
        size_t l = strlen(w);
        memcpy(out + n, w, l); n += l;
        out[n++] = (rnd_n(11) == 0) ? '\n' : ' ';
    }
    return n;
}

/* materialize the object head and compare with the shadow */
static int agree(gn_engine *e, gn_object *o, shadow *s, const char *what, int it) {
    gn_version *v = &o->ver[o->n_ver - 1];
    if (v->total_bytes != s->n) {
        printf("  FAIL  %s it=%d: length %llu != shadow %zu\n",
               what, it, (unsigned long long)v->total_bytes, s->n);
        fails++; return 0;
    }
    uint8_t *b = malloc(s->n + 16);
    size_t got = gn_read(e, o, 0, s->n, b);
    int ok = (got == s->n) && (s->n == 0 || memcmp(b, s->b, s->n) == 0);
    if (!ok) {
        size_t k = 0; while (k < s->n && k < got && b[k] == s->b[k]) k++;
        printf("  FAIL  %s it=%d: bytes differ at %zu (read %zu of %zu)\n",
               what, it, k, got, s->n);
        fails++;
    }
    free(b);
    return ok;
}

/* ---------------------------------------------------------------- */
/* 1. splice fuzz                                                     */
/* ---------------------------------------------------------------- */
static void fuzz_splice(int iters) {
    printf("-- splice fuzz: %d random splices, full byte compare after each --\n", iters);
    uint8_t *txt = malloc(256u << 10);
    size_t n = gen_corpus(txt, 200u << 10);

    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
    gn_object *o = gn_create(e, "f", txt, n);
    shadow s; sh_init(&s, txt, n);

    uint64_t *vhash = malloc((size_t)(iters + 2) * 8);
    uint32_t nvh = 0;
    vhash[nvh++] = fnv(s.b, s.n);

    int bad = 0;
    for (int i = 0; i < iters && !bad; i++) {
        /* deliberately over-sample the boundaries, where the seam logic lives */
        size_t off;
        switch (rnd_n(6)) {
            case 0: off = 0; break;
            case 1: off = s.n; break;
            case 2: off = s.n ? s.n - 1 : 0; break;
            default: off = (size_t)rnd_n(s.n ? s.n + 1 : 1);
        }
        size_t del = 0, ilen = 0;
        uint8_t ins[128];
        int kind = (int)rnd_n(3);
        if (kind != 0) del = (size_t)rnd_n(64);
        if (off + del > s.n) del = s.n - off;
        if (kind != 1) {
            ilen = (size_t)rnd_n(60) + 1;
            for (size_t k = 0; k < ilen; k++) {
                /* mix of dictionary-ish and raw bytes, incl. high bytes */
                uint64_t r = rnd_n(10);
                ins[k] = (uint8_t)(r < 6 ? 'a' + rnd_n(26)
                                 : r < 8 ? ' ' + rnd_n(15)
                                         : rnd_n(256));
            }
        }
        if (gn_update(e, o, off, del, ilen ? ins : NULL, ilen) != 0) {
            printf("  FAIL  splice it=%d rejected (off=%zu del=%zu ilen=%zu len=%zu)\n",
                   i, off, del, ilen, s.n);
            fails++; bad = 1; break;
        }
        sh_splice(&s, off, del, ins, ilen);
        if (!agree(e, o, &s, "splice", i)) { bad = 1; break; }
        vhash[nvh++] = fnv(s.b, s.n);
    }
    CHECK(!bad, "%d splices, engine matched shadow byte-for-byte every time", iters);

    /* every historical version must still hash to what it did when created */
    int hbad = 0;
    for (uint32_t v = 0; v < o->n_ver && v < nvh; v++) {
        uint64_t len = o->ver[v].total_bytes;
        uint8_t *b = malloc((size_t)len + 16);
        size_t got = gn_read_version(e, o, v, 0, (size_t)len, b);
        if (got != len || fnv(b, got) != vhash[v]) hbad++;
        free(b);
    }
    CHECK(hbad == 0, "all %u historical versions still read back correctly", o->n_ver);

    free(vhash); free(s.b); free(txt);
    gn_engine_free(e);
}

/* ---------------------------------------------------------------- */
/* 2. search fuzz                                                     */
/* ---------------------------------------------------------------- */
static size_t brute_count(const uint8_t *hay, size_t hn,
                          const uint8_t *ndl, size_t nn) {
    if (nn == 0 || hn < nn) return 0;
    size_t c = 0;
    for (size_t i = 0; i + nn <= hn; i++) if (memcmp(hay + i, ndl, nn) == 0) c++;
    return c;
}

static void fuzz_search(int iters) {
    printf("-- search fuzz: %d needles vs brute force --\n", iters);
    uint8_t *txt = malloc(256u << 10);
    size_t n = gen_corpus(txt, 120u << 10);

    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
    gn_object *o = gn_create(e, "s", txt, n);
    shadow s; sh_init(&s, txt, n);

    /* a few edits first, so the searched text is not just the ingest */
    for (int i = 0; i < 20; i++) {
        size_t off = (size_t)rnd_n(s.n);
        const uint8_t *ins = (const uint8_t*)"needle-marker ";
        gn_update(e, o, off, 0, ins, 14);
        sh_splice(&s, off, 0, ins, 14);
    }
    agree(e, o, &s, "search-setup", 0);

    size_t cap = 1u << 20;
    gn_hit *hits = malloc(cap * sizeof(gn_hit));
    int mism = 0, absent_ok = 0, present = 0;

    for (int i = 0; i < iters && mism < 5; i++) {
        uint8_t ndl[32]; size_t nn;
        if (rnd_n(4) == 0) {
            /* a needle that cannot occur: high bytes the corpus never uses */
            nn = 3 + rnd_n(5);
            for (size_t k = 0; k < nn; k++) ndl[k] = (uint8_t)(0xF0 + rnd_n(8));
        } else {
            nn = 3 + rnd_n(18);
            if (nn >= s.n) nn = 3;
            size_t at = (size_t)rnd_n(s.n - nn);
            memcpy(ndl, s.b + at, nn);
        }
        size_t want = brute_count(s.b, s.n, ndl, nn);
        size_t got  = gn_search(e, ndl, nn, hits, cap);
        if (got != want) {
            if (mism < 5) {
                printf("  FAIL  needle '%.*s' (len %zu): engine %zu hits, brute %zu\n",
                       (int)(nn > 16 ? 16 : nn), ndl, nn, got, want);
            }
            mism++;
        }
        if (want == 0) absent_ok++; else present++;
        /* every reported offset must really be a match */
        for (size_t h = 0; h < got && h < 64; h++) {
            uint64_t bo = hits[h].byte_off;
            if (bo + nn > s.n || memcmp(s.b + bo, ndl, nn) != 0) {
                printf("  FAIL  reported hit at %llu is not a match\n",
                       (unsigned long long)bo);
                mism++; break;
            }
        }
    }
    CHECK(mism == 0, "%d needles: hit counts and offsets matched brute force "
                     "(%d present, %d absent)", iters, present, absent_ok);

    free(hits); free(s.b); free(txt);
    gn_engine_free(e);
}

/* ---------------------------------------------------------------- */
/* 3. GC / refcount fuzz                                              */
/* ---------------------------------------------------------------- */
static void fuzz_gc(int iters) {
    printf("-- gc/refcount fuzz: edits + history trims, then audit the DAG --\n");
    uint64_t base_live = gn_ext_nodes_alloced();

    uint8_t *txt = malloc(128u << 10);
    size_t n = gen_corpus(txt, 64u << 10);

    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
    gn_object *o = gn_create(e, "g", txt, n);
    shadow s; sh_init(&s, txt, n);

    int trims = 0;
    for (int i = 0; i < iters; i++) {
        size_t off = (size_t)rnd_n(s.n ? s.n + 1 : 1);
        size_t del = (size_t)rnd_n(48); if (off + del > s.n) del = s.n - off;
        uint8_t ins[64]; size_t ilen = (size_t)rnd_n(40);
        for (size_t k = 0; k < ilen; k++) ins[k] = (uint8_t)('a' + rnd_n(26));
        gn_update(e, o, off, del, ilen ? ins : NULL, ilen);
        sh_splice(&s, off, del, ins, ilen);

        /* drop history at random, which is what makes nodes reclaimable */
        if (rnd_n(8) == 0 && o->n_ver > 4) {
            uint32_t keep = 1 + (uint32_t)rnd_n(o->n_ver - 1);
            gn_trim_history(e, o, keep);
            trims++;
        }
    }
    CHECK(agree(e, o, &s, "gc", iters), "content still exact after %d trims", trims);

    /* the audit recomputes every refcount from scratch and compares */
    gn_enode **roots = malloc((size_t)o->n_ver * sizeof(gn_enode*));
    for (uint32_t v = 0; v < o->n_ver; v++) roots[v] = o->ver[v].root;
    int bad = gn_ext_audit(roots, o->n_ver, 1);
    CHECK(bad == 0, "refcount audit over %u live versions: %d mismatches",
          o->n_ver, bad);
    free(roots);

    uint64_t live_before = gn_ext_nodes_alloced();
    free(s.b); free(txt);
    gn_engine_free(e);
    uint64_t live_after = gn_ext_nodes_alloced();

    CHECK(live_after == base_live,
          "every node reclaimed on free: %llu live before, %llu after "
          "(baseline %llu)", (unsigned long long)live_before,
          (unsigned long long)live_after, (unsigned long long)base_live);
}

/* ---------------------------------------------------------------- */
/* 4. persistence fuzz                                                */
/* ---------------------------------------------------------------- */
static void fuzz_persist(int iters, const char *store) {
    printf("-- persistence fuzz: save/reopen at random points mid-stream --\n");
    uint8_t *txt = malloc(128u << 10);
    size_t n = gen_corpus(txt, 96u << 10);

    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
    gn_object *o = gn_create(e, "p", txt, n);
    shadow s; sh_init(&s, txt, n);

    uint64_t *vhash = malloc((size_t)(iters + 2) * 8);
    uint32_t nvh = 0;
    vhash[nvh++] = fnv(s.b, s.n);

    int cycles = 0, bad = 0;
    for (int i = 0; i < iters && !bad; i++) {
        size_t off = (size_t)rnd_n(s.n ? s.n + 1 : 1);
        size_t del = (size_t)rnd_n(50); if (off + del > s.n) del = s.n - off;
        uint8_t ins[64]; size_t ilen = (size_t)rnd_n(45);
        for (size_t k = 0; k < ilen; k++)
            ins[k] = (uint8_t)(rnd_n(8) ? 'a' + rnd_n(26) : rnd_n(256));
        gn_update(e, o, off, del, ilen ? ins : NULL, ilen);
        sh_splice(&s, off, del, ins, ilen);
        vhash[nvh++] = fnv(s.b, s.n);

        if (rnd_n(20) == 0) {
            /* round-trip the whole store through disk, mid-stream */
            if (gn_save(e, store) != 0) { printf("  FAIL  save failed\n"); fails++; bad = 1; break; }
            gn_close(e);
            e = gn_open(store);
            if (!e) { printf("  FAIL  reopen failed\n"); fails++; bad = 1; break; }
            o = gn_object_open(e, "p");
            if (!o) { printf("  FAIL  object lost across reopen\n"); fails++; bad = 1; break; }
            cycles++;
            if (!agree(e, o, &s, "persist-head", i)) { bad = 1; break; }
            /* and the whole history came back too */
            for (uint32_t v = 0; v < o->n_ver && v < nvh; v++) {
                uint64_t len = o->ver[v].total_bytes;
                uint8_t *b = malloc((size_t)len + 16);
                size_t got = gn_read_version(e, o, v, 0, (size_t)len, b);
                if (got != len || fnv(b, got) != vhash[v]) {
                    printf("  FAIL  version %u wrong after reopen #%d\n", v, cycles);
                    fails++; bad = 1; free(b); break;
                }
                free(b);
            }
        }
        if (!bad && !agree(e, o, &s, "persist", i)) { bad = 1; break; }
    }
    CHECK(!bad, "%d edits across %d save/reopen cycles, all versions exact",
          iters, cycles);

    free(vhash); free(s.b); free(txt);
    if (e) gn_close(e);
    { char w[1024]; snprintf(w, sizeof w, "%s.wal", store); remove(w); remove(store); }
}

/* ---------------------------------------------------------------- */
/* 5. corrupt-store fuzz: gn_open must never crash on a bad file      */
/* ---------------------------------------------------------------- */
static uint32_t ctab[256]; static int cready;
static uint32_t crc32c(uint32_t c, const void *b, size_t n) {
    if (!cready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t x = i;
            for (int k = 0; k < 8; k++) x = (x & 1) ? (0xEDB88320u ^ (x >> 1)) : (x >> 1);
            ctab[i] = x;
        }
        cready = 1;
    }
    const uint8_t *p = (const uint8_t*)b;
    c = ~c;
    for (size_t i = 0; i < n; i++) c = ctab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
static void wr32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);
                                          p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* Same idea for a MAPPABLE store, which loads through an entirely different
 * path: chunk token arrays are pointers into the file rather than copies, so
 * a bad n_tokens or extent is a read straight off the end of a mapping.
 * Mutations are confined to the metadata prefix, because that is the region
 * gn_open still validates -- bulk corruption is accepted by design (see
 * PERSISTENCE.md 2.5) and is covered by gn_verify_chunks in oocore_test. */
static void fuzz_corrupt_mappable(int iters, const char *store) {
    char mp[1024]; snprintf(mp, sizeof mp, "%s.map", store);
    uint8_t *txt = malloc(64u << 10);
    size_t n = gen_corpus(txt, 40u << 10);
    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
    gn_object *o = gn_create(e, "c", txt, n);
    for (int i = 0; i < 40; i++) {
        uint64_t tb = o->ver[o->n_ver-1].total_bytes;
        gn_update(e, o, rnd_n(tb), rnd_n(30), (const uint8_t*)"mutate", 6);
    }
    if (gn_save_ex(e, mp, GN_SAVE_MAPPABLE) != 0) {
        printf("  FAIL  mappable save\n"); fails++; gn_close(e); free(txt); return;
    }
    gn_close(e); free(txt);

    FILE *f = fopen(mp, "rb");
    if (!f) { printf("  FAIL  mappable reopen\n"); fails++; return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *orig = malloc((size_t)sz);
    if (fread(orig, 1, (size_t)sz, f) != (size_t)sz) { printf("  FAIL  read\n"); fails++; }
    fclose(f);

    size_t crclen = (size_t)rd64(orig + 44);
    if (crclen < 8 || crclen > (size_t)sz - 64) {
        printf("  FAIL  mappable crc_len looks wrong (%zu)\n", crclen); fails++;
        free(orig); remove(mp); return;
    }

    char mut[1024]; snprintf(mut, sizeof mut, "%s.mut", mp);
    int opened = 0, rejected = 0;
    uint8_t *b = malloc((size_t)sz);
    for (int i = 0; i < iters; i++) {
        memcpy(b, orig, (size_t)sz);
        int nmut = 1 + (int)rnd_n(3);
        for (int k = 0; k < nmut; k++) {
            size_t at = 64 + (size_t)rnd_n((uint64_t)crclen);
            b[at] ^= (uint8_t)(1 + rnd_n(255));
        }
        wr32(b + 32, crc32c(0, b + 64, crclen));   /* metadata crc only */
        wr32(b + 56, crc32c(0, b, 56));
        FILE *g = fopen(mut, "wb"); fwrite(b, 1, (size_t)sz, g); fclose(g);

        gn_engine *m = gn_open(mut);
        if (!m) { rejected++; continue; }
        opened++;
        for (uint32_t oi = 0; oi < gn_engine_objects(m); oi++) {
            gn_object *mo = gn_engine_object(m, oi);
            for (uint32_t v = 0; v < mo->n_ver; v++) {
                uint64_t len = mo->ver[v].total_bytes;
                if (len > (64u << 20)) continue;
                uint8_t *rb = malloc((size_t)len + 16);
                if (rb) { gn_read_version(m, mo, v, 0, (size_t)len, rb); free(rb); }
            }
        }
        gn_close(m);
    }
    free(b); free(orig);
    { char w[1100]; snprintf(w, sizeof w, "%s.wal", mut); remove(w); remove(mut); }
    { char w[1100]; snprintf(w, sizeof w, "%s.wal", mp);  remove(w); remove(mp);  }
    CHECK(opened > 0,
          "%d corrupted MAPPABLE stores: %d rejected, %d loaded and read "
          "through the mapping without crashing%s", iters, rejected, opened,
          opened ? "" : "  <-- NOTHING loaded: checksum repair is broken");
}

static void fuzz_corrupt(int iters, const char *store) {
    printf("-- corrupt-store fuzz: %d mutated stores, checksums REPAIRED --\n", iters);
    /* Repairing the CRC after each mutation is the point: otherwise every
     * mutant is rejected by the checksum and the structural validators --
     * index bounds, name lengths, subtree totals -- are never reached.   */
    uint8_t *txt = malloc(64u << 10);
    size_t n = gen_corpus(txt, 40u << 10);
    gn_engine *e = gn_engine_new();
    gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
    gn_object *o = gn_create(e, "c", txt, n);
    for (int i = 0; i < 40; i++) {
        uint64_t tb = o->ver[o->n_ver-1].total_bytes;
        gn_update(e, o, rnd_n(tb), rnd_n(30), (const uint8_t*)"mutate", 6);
    }
    /* RAW, so the payload on disk IS the payload the CRC covers. With
     * compression on, repairing the checksum over the compressed bytes can
     * never match the loader's check over the decompressed ones, so every
     * mutant was rejected and no structural validator ever ran -- in every
     * build that had zlib or zstd, which is every build except the sanitized
     * one. That is why this campaign passed for so long while testing only
     * that CRC32 works. */
    if (gn_save_ex(e, store, GN_SAVE_RAW) != 0) {
        printf("  FAIL  save\n"); fails++; return;
    }
    gn_close(e); free(txt);

    FILE *f = fopen(store, "rb");
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *orig = malloc((size_t)sz);
    if (fread(orig, 1, (size_t)sz, f) != (size_t)sz) { printf("  FAIL  read\n"); fails++; }
    fclose(f);

    char mut[1024]; snprintf(mut, sizeof mut, "%s.mut", store);
    int opened = 0, rejected = 0;
    uint8_t *b = malloc((size_t)sz);
    for (int i = 0; i < iters; i++) {
        memcpy(b, orig, (size_t)sz);
        /* mutate 1-3 bytes somewhere in the payload */
        int nmut = 1 + (int)rnd_n(3);
        for (int k = 0; k < nmut; k++) {
            size_t at = 64 + (size_t)rnd_n((uint64_t)sz - 64);
            b[at] ^= (uint8_t)(1 + rnd_n(255));
        }
        /* Format v3 header offsets. These MUST track genna_persist.c: when
         * the header grew for mappable stores, the header CRC moved from 36
         * to 56 and this repair silently began writing into `raw_len` and
         * leaving the real CRC stale -- so every mutant was rejected at the
         * checksum and no structural validator ran. The campaign still
         * printed green. The `opened` assertion below now catches that. */
        wr32(b + 32, crc32c(0, b + 64, (size_t)sz - 64));   /* payload crc  */
        wr32(b + 56, crc32c(0, b, 56));                     /* header crc   */
        FILE *g = fopen(mut, "wb"); fwrite(b, 1, (size_t)sz, g); fclose(g);

        gn_engine *m = gn_open(mut);
        if (!m) { rejected++; continue; }
        opened++;
        /* If it loaded, every version must be readable without crashing. */
        for (uint32_t oi = 0; oi < gn_engine_objects(m); oi++) {
            gn_object *mo = gn_engine_object(m, oi);
            for (uint32_t v = 0; v < mo->n_ver; v++) {
                uint64_t len = mo->ver[v].total_bytes;
                if (len > (64u << 20)) continue;      /* absurd, skip the read */
                uint8_t *rb = malloc((size_t)len + 16);
                if (rb) { gn_read_version(m, mo, v, 0, (size_t)len, rb); free(rb); }
            }
        }
        gn_close(m);
    }
    free(b); free(orig);
    { char w[1100]; snprintf(w, sizeof w, "%s.wal", mut); remove(w); remove(mut); }
    { char w[1100]; snprintf(w, sizeof w, "%s.wal", store); remove(w); remove(store); }
    fuzz_corrupt_mappable(iters, store);
    /* The point of repairing the checksum is to get PAST it, so that bad
     * indices and lengths reach the structural validators. If nothing loads,
     * this test proves only that CRC32 works -- which is not what it is for.
     * Fail loudly rather than report a green vacuous run. */
    CHECK(opened > 0,
          "%d corrupted stores: %d rejected, %d loaded and read without "
          "crashing%s", iters, rejected, opened,
          opened ? "" : "  <-- NOTHING loaded: checksum repair is broken, "
                        "the structural validators were never reached");
}

int main(int argc, char **argv) {
    uint64_t seed = (argc > 1) ? strtoull(argv[1], NULL, 0) : 20260811ULL;
    int iters     = (argc > 2) ? atoi(argv[2]) : 2000;
    const char *store = (argc > 3) ? argv[3] : "fuzz_store.gn";
    RS = seed ? seed : 1;

    printf("=== GENNA fuzz suite (seed %llu) ===\n\n", (unsigned long long)seed);
    fuzz_splice(iters);
    printf("\n");  RS = seed * 7919 + 1;
    fuzz_search(400);
    printf("\n");  RS = seed * 104729 + 3;
    fuzz_gc(iters / 2);
    printf("\n");  RS = seed * 15485863 + 5;
    fuzz_persist(iters / 4, store);
    printf("\n");  RS = seed * 32452843 + 7;
    fuzz_corrupt(iters / 8 < 50 ? 50 : iters / 8, store);

    printf("\n%s (%d failures)  [reproduce with: fuzz_test %llu %d]\n",
           fails ? "FUZZ: FAILURES" : "FUZZ: ALL PASS", fails,
           (unsigned long long)seed, iters);
    return fails != 0;
}
