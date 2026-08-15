/* genna.h — Genna v1: CRUD below the compression layer.
 *
 * The contract, mapped to the five architecture views:
 *   View 2: gn_dict     — the learned language (word/phrase -> fixed-width id)
 *   View 2: gn_store    — immutable, content-addressed chunk sea
 *   View 3: gn_object   — a "file" = edit list of chunk refs; versions free
 *   View 4: CRUD verbs  — all metadata ops; no chunk ever rewritten
 *   View 5: read/search — materialize ranges on demand; scan tokens in place
 *
 * Invariants (the three laws):
 *   L1. Tokens are fixed-width (u32). Never entropy-coded on the hot path.
 *   L2. Chunks are immutable after seal. Updates create chunks, never edit them.
 *   L3. Every object version pins its dictionary. Dictionary is append-only.
 *
 * ---------------------------------------------------------------------------
 * API STABILITY
 * ---------------------------------------------------------------------------
 * Every public function here is either STABLE or UNSTABLE. There is no third
 * category and no unmarked function: if a name is not in the STABLE list
 * below, it is UNSTABLE, and that is a deliberate statement rather than an
 * omission.
 *
 *   STABLE    Covered by a test, and will not change shape within 0.x. If it
 *             has to change, the change ships with a converter or a version
 *             bump, not quietly.
 *   UNSTABLE  May change or disappear in any 0.x release without a major
 *             bump. Usable, but pin your version and expect to read a
 *             changelog. Most of these are internals that happen to be
 *             reachable, not an invitation.
 *
 * STABLE (the CRUD surface, persistence, and history):
 *
 *   gn_engine_new     gn_engine_free    gn_engine_stats
 *   gn_create         gn_delete         gn_object_open
 *   gn_read           gn_read_version   gn_update
 *   gn_cut            gn_graft          gn_search
 *   gn_trim_history
 *   gn_save           gn_open           gn_close        (genna_persist.h)
 *   gn_store_format   gn_format_version gn_verify_chunks
 *
 * UNSTABLE (everything else), notably:
 *
 *   gn_ext_*    the extent-tree internals. Reachable because the tests and
 *               the aggregate work need them; not a supported surface.
 *   gn_net_*    replication. Real, but the wire format is not settled.
 *   gn_dict_*   the learned dictionary. Shape is likely to change.
 *   gn_store_*  the chunk sea below the object layer (gn_store_format and
 *               gn_format_version are the exceptions and are STABLE).
 *   gn_tokenize gn_detokenize gn_detok_len  — the token layer.
 *
 * Unsure about a name? It is UNSTABLE. Unstable-and-honest beats
 * stable-and-regretted, and a promise is only worth what the tests behind it
 * are worth.
 */
#ifndef GENNA_H
#define GENNA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- tokens ---------------------------------------------------------- */

typedef uint32_t gn_tok;                 /* L1: fixed width, scannable    */

#define GN_TOK_INVALID   ((gn_tok)0)
/* ids 1..GN_BYTE_BASE-1 : dictionary entries (words/phrases)             */
/* ids GN_BYTE_BASE..GN_BYTE_BASE+255 : byte-escape (raw byte b)          */
#define GN_BYTE_BASE     ((gn_tok)0xFFFFFF00u)
#define GN_TOK_IS_BYTE(t)   ((t) >= GN_BYTE_BASE)
#define GN_TOK_BYTE_VAL(t)  ((uint8_t)((t) - GN_BYTE_BASE))

/* ---- dictionary (View 2): the language ------------------------------- */

typedef struct gn_dict gn_dict;

gn_dict *gn_dict_new(void);
void     gn_dict_free(gn_dict *d);

/* Learn from a sample: frequent words become single tokens.
 * Append-only (L3): learning twice only adds, never renumbers.           */
int      gn_dict_learn(gn_dict *d, const uint8_t *text, size_t len,
                       uint32_t max_new_entries);

/* Probe: exact word -> token id, or GN_TOK_INVALID.
 * This is the "instant corpus-wide no" of View 5.                        */
gn_tok   gn_dict_lookup(const gn_dict *d, const uint8_t *word, size_t len);

/* v2: phrase training. BPE-style merge rounds over the unit stream.
 * Returns final dictionary size. Append-only, so still L3-safe.         */
int      gn_dict_train(gn_dict *d, const uint8_t *text, size_t len,
                       uint32_t rounds, uint32_t merges_per_round,
                       uint32_t min_count);

/* Token id -> its text (byte-escape ids resolve to their single byte).   */
const uint8_t *gn_dict_text(const gn_dict *d, gn_tok t, size_t *len_out);

uint32_t gn_dict_size(const gn_dict *d);
uint64_t gn_dict_version(const gn_dict *d);   /* entry count = version    */

/* Tokenize text into out[] (cap tokens). Returns tokens written, or
 * (size_t)-1 if cap too small. Unknown words fall back to byte-escape,
 * so tokenization NEVER fails on content.                                */
size_t   gn_tokenize(const gn_dict *d, const uint8_t *text, size_t len,
                     gn_tok *out, size_t cap);

/* Exact byte length the token span materializes to (for offset math).   */
size_t   gn_detok_len(const gn_dict *d, const gn_tok *toks, size_t n);

/* Materialize tokens back to bytes. Returns bytes written.               */
size_t   gn_detokenize(const gn_dict *d, const gn_tok *toks, size_t n,
                       uint8_t *out, size_t cap);

/* ---- chunk store (Views 2+3): the immutable sea ---------------------- */

typedef struct gn_store gn_store;
typedef uint64_t gn_cid;                 /* chunk id (content hash)       */

#ifndef GN_CHUNK_TARGET_TOKENS
#define GN_CHUNK_TARGET_TOKENS 1024      /* ~4KB of tokens per chunk      */
#endif

gn_store *gn_store_new(void);
void      gn_store_free(gn_store *s);

/* Put: dedup by content hash (L2). Returns chunk id.                     */
gn_cid    gn_store_put(gn_store *s, const gn_tok *toks, size_t n);

/* Borrow a read-only view of a chunk's tokens (no copy).                 */
const gn_tok *gn_store_get(const gn_store *s, gn_cid id, size_t *n_out);

uint64_t  gn_store_chunks(const gn_store *s);
uint64_t  gn_store_bytes(const gn_store *s);   /* resident token bytes    */

/* ---- object (View 3): file = edit list over the sea ------------------ */

typedef struct {
    gn_cid  chunk;
    uint32_t off;        /* token offset within chunk                     */
    uint32_t len;        /* token count taken from chunk                  */
} gn_extent;

/* ---- extent tree (persistent treap; structural sharing across versions) */
typedef struct gn_enode gn_enode;

uint64_t  gn_ext_tokens(const gn_enode *n);
uint64_t  gn_ext_bytes (const gn_enode *n);
gn_enode *gn_ext_build(const gn_extent *ext, const uint64_t *bytes, uint32_t n);
gn_enode *gn_ext_join(gn_enode *a, gn_enode *b);
typedef uint64_t (*gn_ext_bytes_fn)(void *ctx, const gn_extent *x);
void      gn_ext_split_tok(gn_enode *n, uint64_t k,
                           gn_enode **lo, gn_enode **hi,
                           gn_ext_bytes_fn bytes_of, void *ctx);
bool      gn_ext_locate_byte(const gn_enode *n, uint64_t off,
                             gn_extent *ext_out, uint64_t *tok_before,
                             uint64_t *bytes_before);
void      gn_ext_walk(const gn_enode *n,
                      void (*fn)(void *ctx, const gn_extent *x, uint64_t bytes),
                      void *ctx);
/* Visit only the leaves covering [off, off+len), pruning on subtree byte
 * totals. The callback gets each leaf's absolute start offset. Use this for
 * range reads: gn_ext_walk visits every leaf and makes a small read cost
 * O(total leaves). */
void      gn_ext_walk_range(const gn_enode *n, uint64_t off, uint64_t len,
                            void (*fn)(void *ctx, const gn_extent *x,
                                       uint64_t bytes, uint64_t start),
                            void *ctx);
/* ---- monoid-annotated tree (aggregates in the node) -------------------
 * The treap already carries two aggregates -- subtree token and byte counts --
 * which is why a range read can prune. Those are just the sum monoid over
 * leaf sizes. Generalising the slot lets any associative operation ride the
 * same structure: min/max for predicate pushdown, sum for analytics, a
 * register-max HLL for distinct counts, a transition vector for automaton
 * state.
 *
 * What it buys: an aggregate over a byte range in O(log n) rather than
 * O(range), because a subtree entirely inside the range answers in O(1) from
 * its own annotation. What it costs: 8 bytes on every node, and -- once a
 * monoid is registered -- a leaf aggregate computed per leaf at construction,
 * which reads chunk tokens. Both are measured in AGGREGATES.md.
 *
 * The monoid is per-process, not per-object, and deliberately so: nodes are
 * SHARED between objects and versions, so a per-object monoid would let one
 * object read an annotation another object computed under different rules.
 *
 * `leaf` must be a pure function of the tokens it is given, and `combine`
 * must be associative with `identity`, or range results will disagree with
 * a brute-force scan depending on where the tree happens to split.
 */
typedef struct {
    uint64_t identity;
    uint64_t (*leaf)(const gn_tok *t, uint32_t n, void *ud);
    uint64_t (*combine)(uint64_t a, uint64_t b, void *ud);
    /* how to reach a chunk's tokens; the engine supplies its store lookup */
    const gn_tok *(*resolve)(gn_cid chunk, uint32_t *n_out, void *ud);
    void *ud;
} gn_monoid;

/* Register (or clear, with NULL) the monoid. Trees built BEFORE this call
 * carry annotations computed under the previous rules, so set it before
 * ingesting, or rebuild. */
void      gn_ext_set_monoid(const gn_monoid *m);
int       gn_ext_monoid_set(void);
/* Repoint `resolve`'s context, keeping the operation. gn_open() calls this
 * with the engine it is loading, because leaves are built before the caller
 * can see that engine. */
void      gn_ext_monoid_bind(void *ud);
/* Aggregate over [off, off+len) of the tree rooted at n, in O(log n). */
uint64_t  gn_ext_range_agg(const gn_enode *n, uint64_t off, uint64_t len);
/* The cost of the annotation is gn_ext_node_size(), below -- 8 bytes wider
 * when built with -DGN_NODE_AGG. Without that define these calls compile to
 * no-ops and the node is unchanged, so the feature can be A/B'd. */

uint32_t  gn_ext_depth(const gn_enode *n);
uint32_t  gn_ext_leaves(const gn_enode *n);
gn_enode *gn_ext_retain(gn_enode *n);
void      gn_ext_release(gn_enode *n);
uint64_t  gn_ext_nodes_alloced(void);
uint64_t  gn_ext_nodes_total(void);
uint64_t  gn_ext_nodes_freed(void);
uint64_t  gn_ext_arena_bytes(void);
int       gn_ext_audit(gn_enode **roots, uint32_t nroots, int verbose);
uint64_t  gn_ext_node_bytes(void);
uint64_t  gn_ext_node_size(void);   /* sizeof one node; do NOT hardcode it */
void      gn_ext_arena_free(void);

typedef struct {
    /* v3 (genna_engine3.c): the persistent extent tree */
    gn_enode  *root;
    /* v1/v2 (genna_engine.c, genna_engine2.c): the flat array they were
     * measured with. Kept so the control arms still build and the
     * array-vs-tree comparison in RESULTS.md is reproducible.            */
    gn_extent *ext;
    uint32_t   n_ext;

    uint64_t   total_tokens;
    uint64_t   total_bytes;
    uint64_t   dict_version;   /* L3: pinned                              */
} gn_version;

typedef struct {
    gn_version *ver;           /* ver[0..n_ver-1]; last = current         */
    uint32_t    n_ver;
    char        name[64];
} gn_object;

/* ---- engine: the boundary (View 1) ----------------------------------- */

typedef struct gn_engine gn_engine;

gn_engine *gn_engine_new(void);
void       gn_engine_free(gn_engine *e);
gn_dict   *gn_engine_dict(gn_engine *e);
gn_store  *gn_engine_store(gn_engine *e);

/* ---- Genna-Net sync ---- */
uint32_t  gn_net_manifest(gn_engine*e, gn_object*o, gn_cid**out);
uint32_t  gn_net_diff(const gn_cid*sender,uint32_t ns,const gn_cid*receiver,uint32_t nr,gn_cid**missing);
size_t    gn_net_serialize(gn_engine*e,gn_object*o,const gn_cid*missing,uint32_t nm,uint8_t**out);
size_t    gn_net_extent_bytes(gn_engine*e,gn_object*o);
uint32_t  gn_net_extent_count(gn_engine*e,gn_object*o);
gn_object*gn_net_apply(gn_engine*e,const char*name,const uint8_t*buf,size_t len);
gn_object*gn_net__rebuild(gn_engine*e,const char*name,gn_extent*ex,uint32_t ne);
void      gn_net__install_entry(gn_dict *d, uint32_t id, const uint8_t *txt, uint32_t len);

/* ---- CRUD (View 4). All O(edit-list), never O(file). ----------------- */

/* C: ingest raw text -> tokenize -> chunk -> object v1.                  */
gn_object *gn_create(gn_engine *e, const char *name,
                     const uint8_t *text, size_t len);

/* Look up an object by name. Renamed from gn_open() in the persistence
 * stage: gn_open() is now "open a store from disk" (see genna_persist.h),
 * and C has no overloading. No caller in the tree used the old name.       */
gn_object *gn_object_open(gn_engine *e, const char *name);

/* R: materialize byte range [off, off+len) of latest version into out.
 * Only covering chunks are detokenized (View 5 left path).
 * Returns bytes written.                                                 */
size_t     gn_read(gn_engine *e, gn_object *o,
                   uint64_t off, size_t len, uint8_t *out);

/* U: splice — replace byte range [off, off+del_len) with new text.
 * insert: del_len=0. pure delete: text=NULL.
 * Touches at most 2 boundary chunks + tokenizes the insert.
 * Appends a NEW version (L2, L3). Returns 0 on success.                  */
int        gn_update(gn_engine *e, gn_object *o,
                     uint64_t off, uint64_t del_len,
                     const uint8_t *text, size_t len);

/* drop all but the newest `keep` versions; returns how many were dropped */
uint32_t   gn_trim_history(gn_engine *e, gn_object *o, uint32_t keep);

/* graft: splice a byte range of one object into another at frame
 * boundaries. O(log n) -- no frame is re-parsed or copied. Returns 0.    */
int        gn_graft(gn_engine *e, gn_object *dst, uint64_t dst_off,
                    gn_object *src, uint64_t src_off, uint64_t src_len);

/* cut a frame-aligned byte range. O(log n), no seam re-tokenization.     */
int        gn_cut(gn_engine *e, gn_object *o, uint64_t off, uint64_t len);

/* D: whole-object delete = drop the name. Chunks GC'd when unreferenced. */
int        gn_delete(gn_engine *e, const char *name);

/* time travel: read an older version (0-based). Same cost as gn_read.   */
size_t     gn_read_version(gn_engine *e, gn_object *o, uint32_t version,
                           uint64_t off, size_t len, uint8_t *out);

/* ---- search (View 5 right path): below the layer --------------------- */

typedef struct {
    const char *object;
    uint64_t    byte_off;      /* materialized byte offset of hit         */
} gn_hit;

/* Exact word/phrase search across ALL objects, latest versions.
 * Path: dictionary probe (absent => 0 immediately, nothing scanned)
 *       else fixed-width token scan over chunks, in place (L1).
 * No detokenization on this path. Returns hit count.                     */
size_t     gn_search(gn_engine *e, const uint8_t *needle, size_t len,
                     gn_hit *hits, size_t cap);

/* ---- introspection: proving the claims ------------------------------- */

typedef struct {
    uint64_t chunks_created;
    uint64_t chunks_deduped;
    uint64_t tokens_scanned;       /* search work                         */
    uint64_t tokens_detokenized;   /* read/update toll actually paid      */
    uint64_t bytes_in;             /* raw text ingested                   */
    uint64_t bytes_resident;       /* store + dict, actual RAM            */
} gn_stats;

void gn_engine_stats(const gn_engine *e, gn_stats *out);

#endif /* GENNA_H */
