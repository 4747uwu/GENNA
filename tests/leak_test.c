/* leak_test.c — exercise every path, then require the allocator balance to
 * return to zero. Fills the gap left by LeakSanitizer being unavailable here.
 *
 * Each phase is measured independently so a leak is attributed, not just
 * detected: "the engine leaks" is a bug report nobody can act on.
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void     gn_leak_reset(void);
void     gn_leak_report(const char *what);
uint64_t gn_leak_live_bytes(void);
uint64_t gn_leak_live_blocks(void);

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint64_t RS = 12345;
static uint64_t rnd(void){ RS^=RS<<13; RS^=RS>>7; RS^=RS<<17; return RS; }

static size_t gen(uint8_t *o, size_t target) {
    static const char *W[] = {"alpha","beta","gamma","delta","epsilon","zeta",
        "the","and","of","{\"x\":1}","\n","  ","1234567890"};
    size_t n = 0;
    while (n + 24 < target) {
        const char *w = W[rnd() % 13]; size_t l = strlen(w);
        memcpy(o + n, w, l); n += l; o[n++] = ' ';
    }
    return n;
}

/* Run one phase and report the allocator delta it leaves behind.
 *
 * gn_ext_arena_free() is called before measuring because the treap arena is
 * deliberately process-global: node_alloc() carves 4096-node blocks that are
 * kept and recycled through a free list rather than returned per engine, and
 * gn_ext_arena_free() is the documented way to hand them back. Without this
 * call a phase looks like it leaks exactly one 229384-byte block per arena
 * block it touched -- which is the arena, not a leak. It is only safe here
 * because each phase frees its engine first, so no node is live.          */
#define PHASE(name, body) do { \
    gn_leak_reset(); \
    { body } \
    gn_ext_arena_free(); \
    gn_leak_report(name); \
    CHECK(gn_leak_live_bytes() == 0, "%s: no bytes outstanding (%llu blocks)", \
          name, (unsigned long long)gn_leak_live_blocks()); \
} while (0)

int main(int argc, char **argv) {
    const char *store = (argc > 1) ? argv[1] : "leak_store.gn";
    printf("=== leak accounting (linker --wrap over malloc/free/calloc/realloc) ===\n");

    uint8_t *txt = malloc(200u << 10);
    size_t n = gen(txt, 150u << 10);

    PHASE("engine new/free", {
        gn_engine *e = gn_engine_new();
        gn_engine_free(e);
    });

    PHASE("dict train + create", {
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
        gn_create(e, "a", txt, n);
        gn_engine_free(e);
    });

    PHASE("500 splices + history", {
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
        gn_object *o = gn_create(e, "a", txt, n);
        for (int i = 0; i < 500; i++) {
            uint64_t tb = o->ver[o->n_ver-1].total_bytes;
            uint64_t at = rnd() % (tb ? tb : 1);
            uint8_t ins[32]; size_t il = rnd() % 30 + 1;
            for (size_t k = 0; k < il; k++) ins[k] = (uint8_t)('a' + rnd() % 26);
            gn_update(e, o, at, rnd() % 20, ins, il);
        }
        gn_engine_free(e);
    });

    PHASE("trim history", {
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
        gn_object *o = gn_create(e, "a", txt, n);
        for (int i = 0; i < 200; i++) gn_update(e, o, 10, 5, (const uint8_t*)"zz", 2);
        gn_trim_history(e, o, 3);
        gn_engine_free(e);
    });

    /* gn_delete used to free the object without releasing its version trees,
     * so every node the object owned stayed live. The arena is block-granular
     * so a byte count cannot see that; count nodes instead.               */
    {
        uint64_t before, after_create, after_delete;
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
        before = gn_ext_nodes_alloced();
        gn_object *o = gn_create(e, "a", txt, n);
        for (int i = 0; i < 50; i++) gn_update(e, o, 10, 5, (const uint8_t*)"qq", 2);
        after_create = gn_ext_nodes_alloced();
        gn_delete(e, "a");
        after_delete = gn_ext_nodes_alloced();
        gn_engine_free(e);
        /* This block is outside PHASE(), so it must hand the arena back
         * itself. Otherwise its arena block is freed inside the NEXT phase,
         * after that phase reset the counters -- and the accounting
         * underflows to "-1 blocks" instead of reporting zero. */
        gn_ext_arena_free();
        CHECK(after_create > before, "gn_delete: object held %llu tree nodes",
              (unsigned long long)(after_create - before));
        CHECK(after_delete == before,
              "gn_delete releases the whole version history: %llu nodes live "
              "after delete vs %llu before create",
              (unsigned long long)after_delete, (unsigned long long)before);
    }

    PHASE("gn_delete (drops an object mid-session)", {
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
        gn_object *o = gn_create(e, "a", txt, n);
        for (int i = 0; i < 50; i++) gn_update(e, o, 10, 5, (const uint8_t*)"qq", 2);
        gn_delete(e, "a");
        gn_engine_free(e);
    });

    PHASE("search", {
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
        gn_create(e, "a", txt, n);
        gn_hit *h = malloc(4096 * sizeof(gn_hit));
        gn_search(e, (const uint8_t*)"alpha", 5, h, 4096);
        gn_search(e, (const uint8_t*)"\xf0\xf1\xf2", 3, h, 4096);
        free(h);
        gn_engine_free(e);
    });

    PHASE("save + open + edit + close", {
        gn_engine *e = gn_engine_new();
        gn_dict_train(gn_engine_dict(e), txt, n, 4, 50000, 8);
        gn_object *o = gn_create(e, "a", txt, n);
        for (int i = 0; i < 60; i++) gn_update(e, o, 20, 4, (const uint8_t*)"pp", 2);
        gn_save(e, store);
        gn_close(e);
        gn_engine *e2 = gn_open(store);
        gn_object *o2 = gn_object_open(e2, "a");
        for (int i = 0; i < 20; i++) gn_update(e2, o2, 30, 3, (const uint8_t*)"rr", 2);
        gn_save(e2, store);
        gn_close(e2);
    });

    PHASE("open + WAL replay", {
        gn_engine *e = gn_open(store);
        gn_object *o = gn_object_open(e, "a");
        for (int i = 0; i < 40; i++) gn_update(e, o, 15, 2, (const uint8_t*)"ss", 2);
        gn_close(e);
        gn_engine *e2 = gn_open(store);   /* replays the 40 logged edits */
        gn_close(e2);
    });

    PHASE("failed open of a corrupt store", {
        /* the error paths free everything they built too */
        FILE *f = fopen("corrupt_probe.gn", "wb");
        uint8_t junk[400]; memset(junk, 0xAB, sizeof junk);
        memcpy(junk, "GENNAsnp", 8);
        fwrite(junk, 1, sizeof junk, f); fclose(f);
        gn_engine *e = gn_open("corrupt_probe.gn");
        if (e) { printf("  (note: corrupt store opened, unexpected)\n"); gn_close(e); }
        remove("corrupt_probe.gn");
    });

    free(txt);

    /* the treap arena is process-global and released explicitly */
    gn_ext_arena_free();
    CHECK(gn_ext_nodes_alloced() == 0, "treap: 0 nodes live at exit");

    { char w[512]; snprintf(w, sizeof w, "%s.wal", store); remove(w); remove(store); }

    printf("\n%s (%d failures)\n", fails ? "LEAK CHECK: FAILURES" : "LEAK CHECK: CLEAN", fails);
    return fails != 0;
}
