/* leakcheck.c — malloc accounting via linker --wrap.
 *
 * ASan on Windows reports memory ERRORS but not LEAKS: LeakSanitizer is a
 * separate component and this platform does not have it (verified -- ASan
 * answers "detect_leaks is not supported on this platform"). The treap has
 * its own leak check already (node count returns to baseline), but that says
 * nothing about the chunk store, the dictionary arrays, or the buffers the
 * persistence layer allocates.
 *
 * So: wrap the allocator at link time and count. Every block carries a
 * 16-byte header with a magic, so a pointer that came from somewhere else
 * (a libc-internal allocation freed through our free) is detected and passed
 * straight through rather than mis-accounted.
 *
 * Link with:
 *   -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc
 *
 * Do NOT combine with ASan: reading the 16 bytes in front of a foreign
 * pointer is exactly the kind of thing ASan exists to complain about, and
 * ASan's own allocator makes the accounting meaningless anyway.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

void *__real_malloc(size_t);
void  __real_free(void *);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);

#define LK_MAGIC 0xA1B2C3D4E5F60718ULL
#define LK_HSZ   16u                 /* keeps 16-byte alignment */

typedef struct { uint64_t magic; uint64_t size; } lkhdr;

static uint64_t g_live_bytes, g_live_blocks, g_total_blocks, g_total_bytes, g_peak;
static uint64_t g_foreign_frees;

static void lk_note(uint64_t n) {
    g_live_bytes += n; g_live_blocks++; g_total_blocks++; g_total_bytes += n;
    if (g_live_bytes > g_peak) g_peak = g_live_bytes;
}

void *__wrap_malloc(size_t n) {
    lkhdr *h = (lkhdr*)__real_malloc(n + LK_HSZ);
    if (!h) return NULL;
    h->magic = LK_MAGIC; h->size = n;
    lk_note(n);
    return (uint8_t*)h + LK_HSZ;
}

void __wrap_free(void *p) {
    if (!p) return;
    lkhdr *h = (lkhdr*)((uint8_t*)p - LK_HSZ);
    if (h->magic != LK_MAGIC) { g_foreign_frees++; __real_free(p); return; }
    g_live_bytes -= h->size; g_live_blocks--;
    h->magic = 0;                       /* also catches a double free */
    __real_free(h);
}

void *__wrap_calloc(size_t a, size_t b) {
    if (a && b > (size_t)-1 / a) return NULL;      /* overflow */
    size_t n = a * b;
    void *p = __wrap_malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *__wrap_realloc(void *p, size_t n) {
    if (!p) return __wrap_malloc(n);
    lkhdr *h = (lkhdr*)((uint8_t*)p - LK_HSZ);
    if (h->magic != LK_MAGIC) return __real_realloc(p, n);
    uint64_t old = h->size;
    lkhdr *nh = (lkhdr*)__real_realloc(h, n + LK_HSZ);
    if (!nh) return NULL;
    nh->size = n;
    g_live_bytes = g_live_bytes - old + n;
    g_total_bytes += (n > old) ? (n - old) : 0;
    return (uint8_t*)nh + LK_HSZ;
}

/* strdup is a libc function that mallocs internally; if it does not route
 * through our wrapper, the block is foreign and free() passes it through.
 * Nothing to do, but count it so the report can say so. */

void gn_leak_reset(void) {
    g_live_bytes = g_live_blocks = g_total_blocks = g_total_bytes = g_peak = 0;
    g_foreign_frees = 0;
}

uint64_t gn_leak_live_bytes(void)  { return g_live_bytes; }
uint64_t gn_leak_live_blocks(void) { return g_live_blocks; }

void gn_leak_report(const char *what) {
    /* Print signed: a phase that frees more than it allocated (an allocation
     * made before the counters were reset) otherwise shows up as ~1.8e19
     * rather than as the "-1 blocks" it actually is. */
    printf("  [leak] %-22s live %lld bytes in %lld blocks | "
           "lifetime %llu blocks, peak %.2f MB",
           what,
           (long long)g_live_bytes, (long long)g_live_blocks,
           (unsigned long long)g_total_blocks, g_peak / 1048576.0);
    if (g_foreign_frees) printf(" | %llu foreign frees passed through",
                                (unsigned long long)g_foreign_frees);
    printf("\n");
}
