/* genna_bin.h — binary objects with content-defined chunking.
 *
 * The text ingest path learns a dictionary and cuts chunks at fixed token
 * counts. Both are wrong for geometry: learning makes identical bytes in a
 * later object tokenize differently, and fixed cuts mean inserting one vertex
 * shifts every boundary after it so nothing dedups.
 *
 * This path tokenizes byte-for-byte with no learning, and cuts where the
 * CONTENT says to, so an insertion perturbs one chunk instead of all of them.
 *
 * Cost: one byte becomes one 4-byte token in the store, so a binary object
 * occupies 4x its size. Real, and measured in tests/bin_test.c.
 */
#ifndef GENNA_BIN_H
#define GENNA_BIN_H

#include "genna.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t avg_chunk;   /* target chunk size in bytes (default 4096)     */
    uint32_t min_chunk;   /* never cut below this (default avg/4)          */
    uint32_t max_chunk;   /* always cut by this (default avg*4)            */
    int      fixed;       /* 1 = fixed-size cuts, for A/B against CDC      */
} gn_bin_opts;

void gn_bin_opts_default(gn_bin_opts *o);

/* Ingest raw bytes as a new object. NULL opts uses the defaults. */
gn_object *gn_create_binary(gn_engine *e, const char *name,
                            const uint8_t *data, size_t len,
                            const gn_bin_opts *opts);

/* Leaf count of a version ((uint32_t)-1 for the latest). */
uint32_t gn_bin_chunk_count(const gn_object *o, uint32_t version);

/* Z-order (Morton) permutation of a vertex array, so that points near each
 * other in space are near each other in the byte stream. Writes n indices.
 * Without this a "localized" edit can touch bytes scattered through the whole
 * file, and no chunking scheme can recover the sharing. */
int gn_morton_order(const float *xyz, uint32_t n, uint32_t *order_out);

#ifdef __cplusplus
}
#endif
#endif /* GENNA_BIN_H */
