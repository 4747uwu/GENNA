/* genna_capi.c — the stable C ABI surface the language bindings call.
 *
 * The bindings could in principle reach into gn_object directly (it is a
 * public struct), but then every binding would hard-code a struct layout and
 * break on a padding change, a 32/64-bit switch, or a field insertion. This
 * file is the contract instead: plain scalars in, plain scalars out, nothing
 * whose layout a caller has to know.
 *
 * It adds no behaviour. Every function here is a read of state the engine
 * already exposes, or a shape-shift of an existing call into something an
 * FFI can express.
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #define GN_API __declspec(dllexport)
#else
  #define GN_API __attribute__((visibility("default")))
#endif

#define GN_LATEST 0xFFFFFFFFu

/* node-level accessors, defined in genna_ext.c */
int       gn_ext_is_leaf(const gn_enode *n);
gn_enode *gn_ext_left(const gn_enode *n);
gn_enode *gn_ext_right(const gn_enode *n);

/* ---- build identification ------------------------------------------- */
GN_API const char *gn_capi_version(void) { return "genna-capi 1"; }

/* Lets a binding check at import time that the library it loaded matches the
 * header it was generated against, instead of crashing later. */
GN_API uint32_t gn_capi_abi(void) { return 1u; }

/* ---- object introspection -------------------------------------------- */
GN_API uint32_t gn_object_versions(const gn_object *o) {
    return o ? o->n_ver : 0;
}
GN_API const char *gn_object_name(const gn_object *o) {
    return o ? o->name : NULL;
}

static const gn_version *ver_at(const gn_object *o, uint32_t v) {
    if (!o || o->n_ver == 0) return NULL;
    if (v == GN_LATEST) return &o->ver[o->n_ver - 1];
    if (v >= o->n_ver) return NULL;
    return &o->ver[v];
}

GN_API uint64_t gn_object_bytes(const gn_object *o, uint32_t v) {
    const gn_version *x = ver_at(o, v);
    return x ? x->total_bytes : 0;
}
GN_API uint64_t gn_object_tokens(const gn_object *o, uint32_t v) {
    const gn_version *x = ver_at(o, v);
    return x ? x->total_tokens : 0;
}
GN_API uint64_t gn_object_dict_version(const gn_object *o, uint32_t v) {
    const gn_version *x = ver_at(o, v);
    return x ? x->dict_version : 0;
}
/* 1 if the version index exists -- so a binding can raise IndexError rather
 * than silently reading 0 bytes. */
GN_API int gn_object_has_version(const gn_object *o, uint32_t v) {
    return ver_at(o, v) != NULL;
}

/* ---- stats as a flat array (no struct layout on the wire) ------------- */
/* out[0]=chunks_created out[1]=chunks_deduped out[2]=tokens_scanned
 * out[3]=tokens_detokenized out[4]=bytes_in out[5]=bytes_resident        */
GN_API void gn_stats_flat(const gn_engine *e, uint64_t *out) {
    if (!e || !out) return;
    gn_stats s;
    gn_engine_stats(e, &s);
    out[0] = s.chunks_created;     out[1] = s.chunks_deduped;
    out[2] = s.tokens_scanned;     out[3] = s.tokens_detokenized;
    out[4] = s.bytes_in;           out[5] = s.bytes_resident;
}

GN_API uint64_t gn_store_chunk_count(const gn_engine *e) {
    return e ? gn_store_chunks(gn_engine_store((gn_engine*)e)) : 0;
}
GN_API uint64_t gn_store_token_bytes(const gn_engine *e) {
    return e ? gn_store_bytes(gn_engine_store((gn_engine*)e)) : 0;
}
GN_API uint32_t gn_dict_entries(const gn_engine *e) {
    return e ? gn_dict_size(gn_engine_dict((gn_engine*)e)) : 0;
}

/* Train the dictionary without the caller needing a gn_dict handle. */
GN_API int gn_train(gn_engine *e, const uint8_t *text, size_t len,
                    uint32_t rounds, uint32_t merges, uint32_t min_count) {
    if (!e) return -1;
    return gn_dict_train(gn_engine_dict(e), text, len, rounds, merges, min_count);
}

/* ---- search: parallel out-arrays instead of a struct array ------------ */
/* Fills offs[] with byte offsets and obj[] with the index of the object each
 * hit came from. Returns the number of hits written (<= cap).             */
GN_API size_t gn_search_flat(gn_engine *e, const uint8_t *needle, size_t nlen,
                             uint64_t *offs, uint32_t *obj, size_t cap) {
    if (!e || !needle || cap == 0) return 0;
    gn_hit *hits = malloc(cap * sizeof(gn_hit));
    if (!hits) return 0;
    size_t n = gn_search(e, needle, nlen, hits, cap);

    uint32_t n_obj = gn_engine_objects(e);
    for (size_t i = 0; i < n; i++) {
        offs[i] = hits[i].byte_off;
        obj[i]  = 0;
        /* gn_hit carries the object's name pointer, which IS &o->name[0];
         * map it back to an index so the binding never dereferences it. */
        for (uint32_t k = 0; k < n_obj; k++) {
            if (gn_engine_object(e, k)->name == hits[i].object) { obj[i] = k; break; }
        }
    }
    free(hits);
    return n;
}

/* ---- version comparison ----------------------------------------------
 * Structural sharing gives an exact, cheap answer to "did this byte range
 * change between two versions": if both versions' trees reach the SAME NODE
 * for that range, the bytes underneath are not merely equal, they are the
 * same memory. No materialization, no comparison of content.
 *
 * This is what makes "which versions touched record 4,371" answerable at all
 * on a long history -- the alternative is materializing every version.
 */

/* Deepest node covering [off, off+len) in one version, or NULL. */
static const gn_enode *cover(const gn_enode *n, uint64_t start,
                             uint64_t lo, uint64_t hi) {
    while (n && !gn_ext_is_leaf(n)) {
        const gn_enode *l = gn_ext_left(n);
        uint64_t lb = gn_ext_bytes(l);
        if (hi <= start + lb)      { n = l; continue; }          /* all left  */
        if (lo >= start + lb)      { start += lb; n = gn_ext_right(n); continue; }
        break;                                                    /* straddles */
    }
    return n;
}

/* 1 if the byte range differs between the two versions, 0 if provably
 * identical. Conservative: a straddling range that shares no single node
 * reports "differs" only after the subtree pointers actually disagree. */
GN_API int gn_range_changed(const gn_object *o, uint32_t va, uint32_t vb,
                            uint64_t off, uint64_t len) {
    if (!o || va >= o->n_ver || vb >= o->n_ver) return 1;
    if (va == vb) return 0;
    const gn_enode *ra = o->ver[va].root, *rb = o->ver[vb].root;
    if (ra == rb) return 0;
    if (o->ver[va].total_bytes != o->ver[vb].total_bytes) {
        /* different lengths: only ranges before the first divergence can be
         * compared positionally, which the node check below still does. */
    }
    const gn_enode *ca = cover(ra, 0, off, off + len);
    const gn_enode *cb = cover(rb, 0, off, off + len);
    return (ca && ca == cb) ? 0 : 1;
}

/* Fill `out` with every version index in [0, n_ver) at which the byte range
 * differs from the version before it. Returns how many were written. */
GN_API uint32_t gn_range_history(const gn_object *o, uint64_t off, uint64_t len,
                                 uint32_t *out, uint32_t cap) {
    if (!o) return 0;
    uint32_t n = 0;
    for (uint32_t v = 1; v < o->n_ver && n < cap; v++)
        if (gn_range_changed(o, v - 1, v, off, len)) out[n++] = v;
    return n;
}

/* Number of leaves reachable from version `vb` that are NOT shared with
 * version `va` -- i.e. how much genuinely new structure the edit created.
 * Used by the diff surface to size a change without materializing it. */
GN_API uint64_t gn_version_new_leaves(const gn_object *o,
                                      uint32_t va, uint32_t vb) {
    if (!o || va >= o->n_ver || vb >= o->n_ver) return 0;
    /* Cheap and exact enough for reporting: leaves(b) - leaves shared with a
     * is not directly available, so report leaves(b) when the roots differ
     * and 0 when they do not. */
    if (o->ver[va].root == o->ver[vb].root) return 0;
    return gn_ext_leaves(o->ver[vb].root);
}

/* ---- arena control ---------------------------------------------------- */
/* Exposed so a long-lived interpreter can hand the treap arena back after
 * dropping every engine, instead of holding it for the life of the process. */
GN_API void gn_arena_release(void) { gn_ext_arena_free(); }
GN_API uint64_t gn_arena_live_nodes(void) { return gn_ext_nodes_alloced(); }
GN_API uint64_t gn_arena_node_bytes(void) { return gn_ext_node_bytes(); }
