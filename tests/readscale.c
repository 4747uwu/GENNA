/* readscale.c — is a small read O(log n) or O(n)?
 *
 * The central claim is that reading a byte range costs the range, not the
 * file. read_ver() passes the whole tree to gn_ext_walk(), which visits every
 * leaf; read_leaf() skips the ones before the offset, but the WALK still
 * descends them. If that is what happens, a 1 KB read from a 100 MB object
 * costs O(total leaves), and the cost of a fixed-size read will grow linearly
 * with object size.
 *
 * This measures exactly that: same tiny read, object size doubling.
 */
#include "../include/genna.h"
#include "../include/genna_bin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void) {
    printf("=== cost of a fixed 1 KB read as the object grows ===\n");
    printf("   if the read path is O(log n), the time should be flat\n\n");
    printf("   %10s %10s %12s %14s\n", "object", "leaves", "read 1KB", "ns/leaf");

    const int REPS = 200;
    double prev = 0;
    for (size_t mb = 1; mb <= 32; mb *= 2) {
        size_t n = mb << 20;
        uint8_t *data = malloc(n);
        for (size_t i = 0; i < n; i++) data[i] = (uint8_t)(i * 2654435761u >> 13);

        gn_engine *e = gn_engine_new();
        gn_bin_opts o; gn_bin_opts_default(&o);
        gn_object *ob = gn_create_binary(e, "big", data, n, &o);
        uint32_t leaves = gn_bin_chunk_count(ob, (uint32_t)-1);

        uint8_t buf[1024];
        /* read from the MIDDLE: the worst case for a left-to-right walk is
         * the end, the average case is the middle. */
        uint64_t at = n / 2;
        double t0 = ms();
        for (int r = 0; r < REPS; r++) gn_read(e, ob, at, sizeof buf, buf);
        double per = (ms() - t0) / REPS;

        printf("   %8zu MB %10u %10.4f ms %12.1f\n",
               mb, leaves, per, per * 1e6 / (leaves ? leaves : 1));
        if (prev > 0 && per > prev * 1.6)
            printf("        ^ grew %.1fx when the object doubled -> "
                   "this is O(n), not O(log n)\n", per / prev);
        prev = per;

        /* correctness: the bytes must be right regardless */
        if (memcmp(buf, data + at, sizeof buf) != 0) {
            printf("   *** read returned wrong bytes ***\n");
            return 1;
        }
        gn_engine_free(e);
        gn_ext_arena_free();
        free(data);
    }
    return 0;
}
