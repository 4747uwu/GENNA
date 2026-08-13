/* genna_persist.c — the on-disk layer: snapshot + write-ahead log.
 *
 * Two files make a store:
 *
 *   <path>       the snapshot. Chunk table + dictionary + the version DAG.
 *   <path>.wal   the log. Every edit since the snapshot, in order.
 *
 * THE VERSION DAG IS THE WHOLE POINT.  In RAM, N versions of an object cost
 * O(N log n) nodes, not O(N n), because path copying makes every untouched
 * subtree shared. If persistence walked each version and wrote out its
 * extents, that sharing would be destroyed on disk: 500 versions of a 10MB
 * object would write 500 full extent lists. So the node graph is serialized
 * AS a graph -- each distinct node emitted exactly once, in post-order, with
 * children referred to by index. Reload rebuilds the same graph with the same
 * sharing. The file is therefore the same size as the RAM structure, and a
 * version still costs O(log n).
 *
 * Chunks are content-addressed and immutable, so the chunk table is written
 * once, keyed by content hash: dedup on disk is free, inherited rather than
 * implemented.
 *
 * Everything is little-endian and fixed-width on the wire, so a store written
 * by a 32-bit build reads on a 64-bit one.
 */
#include "../include/genna.h"
#include "../include/genna_persist.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#if defined(_WIN32)
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
  #define GN_FSYNC(fd)  _commit(fd)
  #define GN_FILENO(f)  _fileno(f)
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/mman.h>     /* map_file()/gn_store_unmap() on POSIX */
  #define GN_FSYNC(fd)  fsync(fd)
  #define GN_FILENO(f)  fileno(f)
#endif

/* Cut a file down to `len` bytes and flush that change.
 * Win32 API rather than _chsize_s: the CRT's secure variants are not
 * declared under -std=c11 (__STRICT_ANSI__), and this needs no feature test. */
static int file_truncate(const char *path, uint64_t len) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)len;
    int ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) && SetEndOfFile(h);
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    return ok ? 0 : -1;
#else
    if (truncate(path, (off_t)len) != 0) return -1;
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
    return 0;
#endif
}

/* ---- engine-internal accessors (defined in genna_engine3.c / _ext / _dict) */
uint32_t      gn_store__count(const gn_store *s);
const gn_tok *gn_store__at(const gn_store *s, uint32_t i,
                           uint32_t *n_out, gn_cid *id_out);
uint64_t      gn_store__deduped(const gn_store *s);
void          gn_store__set_deduped(gn_store *s, uint64_t v);

uint32_t   gn_engine__n_obj(const gn_engine *e);
gn_object *gn_engine__obj(const gn_engine *e, uint32_t i);
void       gn_engine__attach(gn_engine *e, gn_object *o);
void       gn_engine__stats_get(const gn_engine *e, gn_stats *out);
void       gn_engine__stats_set(gn_engine *e, const gn_stats *in);
void      *gn_engine__wal(const gn_engine *e);
void       gn_engine__set_wal(gn_engine *e, void *w);

int             gn_dict__serializable(const gn_dict *d);
uint32_t        gn_dict__entries(const gn_dict *d);
const uint8_t  *gn_dict__texts(const gn_dict *d, uint64_t *len_out);
const uint32_t *gn_dict__offs(const gn_dict *d);
const uint32_t *gn_dict__lens(const gn_dict *d);
int             gn_dict__restore(gn_dict *d, const uint8_t *texts,
                                 uint64_t texts_len, const uint32_t *offs,
                                 const uint32_t *lens, uint32_t n);

int       gn_ext_is_leaf(const gn_enode *n);
gn_enode *gn_ext_left(const gn_enode *n);
gn_enode *gn_ext_right(const gn_enode *n);
void      gn_ext_leaf_info(const gn_enode *n, gn_extent *ext_out, uint64_t *bytes_out);
uint32_t  gn_ext_prio(const gn_enode *n);
gn_enode *gn_ext_mk_leaf_p(gn_extent e, uint64_t bytes, uint32_t prio);
gn_enode *gn_ext_mk_inner_p(gn_enode *l, gn_enode *r, uint32_t prio);

/* ====================================================================== */
/* CRC32 (IEEE 802.3, reflected) — integrity, not security.               */
/* ====================================================================== */
static uint32_t g_crc_tab[256];
static int      g_crc_ready = 0;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}
static uint32_t crc32_up(uint32_t crc, const void *buf, size_t len) {
    if (!g_crc_ready) crc_init();
    const uint8_t *p = (const uint8_t*)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) crc = g_crc_tab[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ====================================================================== */
/* little-endian growable write buffer                                    */
/* ====================================================================== */
typedef struct { uint8_t *p; size_t n, cap; int oom; } buf;

static int bgrow(buf *b, size_t need) {
    if (b->oom) return -1;
    if (b->n + need <= b->cap) return 0;
    size_t c = b->cap ? b->cap : 4096;
    while (c < b->n + need) {
        if (c > (size_t)-1 / 2) { b->oom = 1; return -1; }
        c *= 2;
    }
    uint8_t *np = realloc(b->p, c);
    if (!np) { b->oom = 1; return -1; }
    b->p = np; b->cap = c;
    return 0;
}
static void braw(buf *b, const void *d, size_t n) {
    if (n == 0) return;
    if (bgrow(b, n)) return;
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bu8 (buf *b, uint8_t v)  { if (bgrow(b,1)) return; b->p[b->n++] = v; }
static void bu32(buf *b, uint32_t v) {
    if (bgrow(b,4)) return;
    b->p[b->n++] = (uint8_t)(v);       b->p[b->n++] = (uint8_t)(v >> 8);
    b->p[b->n++] = (uint8_t)(v >> 16); b->p[b->n++] = (uint8_t)(v >> 24);
}
static void bu64(buf *b, uint64_t v) { bu32(b, (uint32_t)v); bu32(b, (uint32_t)(v >> 32)); }

/* ---- bounds-checked reader ------------------------------------------- */
typedef struct { const uint8_t *p; size_t n, at; int err; } rdr;

static const uint8_t *rraw(rdr *r, size_t n) {
    if (r->err || r->at + n > r->n || r->at + n < r->at) { r->err = 1; return NULL; }
    const uint8_t *q = r->p + r->at; r->at += n; return q;
}
static uint8_t ru8(rdr *r) {
    const uint8_t *q = rraw(r, 1); return q ? *q : 0;
}
static uint32_t ru32(rdr *r) {
    const uint8_t *q = rraw(r, 4); if (!q) return 0;
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8)
         | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}
static uint64_t ru64(rdr *r) {
    uint32_t lo = ru32(r), hi = ru32(r);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Is a declared count plausible, given how many bytes are actually left?
 *
 * Every count in the payload is followed by that many records, so a count can
 * never exceed remaining_bytes / bytes_per_record. Checking this BEFORE
 * allocating is the difference between rejecting a corrupt file and trying to
 * malloc 12.9 GB from a flipped byte -- which is exactly what the corrupt-store
 * fuzzer did to the dictionary and node counts once it could get past the CRC.
 *
 * `per` is the MINIMUM on-disk size of one record, so this never rejects a
 * file that could legitimately be read. It is a sanity bound, not a parser:
 * the real structural checks still follow. */
static int rfits(const rdr *r, uint64_t count, size_t per) {
    if (count == 0) return 1;
    size_t left = r->n > r->at ? r->n - r->at : 0;
    return count <= (uint64_t)(left / (per ? per : 1));
}

/* ====================================================================== */
/* the WAL handle, hung off the engine                                    */
/* ====================================================================== */
#define WOP_CREATE 1
#define WOP_UPDATE 2
#define WOP_DELETE 3
#define WOP_GRAFT  4
#define WOP_CUT    5
#define WOP_TRIM   6

typedef struct gn_wal {
    char    *path;        /* store path                                    */
    char    *wpath;       /* store path + ".wal"                           */
    FILE    *f;           /* append handle, NULL if logging is off         */
    uint64_t gen;         /* generation of the snapshot this log extends   */
    int      sync;        /* fsync after every record                      */
    int      replaying;   /* suppress logging while replaying              */
    int      broken;      /* a write or fsync failed: guarantee is void    */
    uint64_t records, wbytes, replayed;
    gn_engine *e;
} gn_wal;

#define SNAP_HDR_BYTES 64u
#define WAL_HDR_BYTES  32u

/* ---- snapshot payload compression ------------------------------------
 * The store keeps chunks as gn_tok (u32). For binary data every byte becomes
 * GN_BYTE_BASE+b, so three bytes in four are 0xFF -- the on-disk payload is
 * extremely compressible, and leaving it raw was costing ~4x for nothing.
 * Compression is applied to the whole payload at save time only: the read
 * path, the chunk store and the version DAG are untouched.
 *
 * Optional at compile time so a build without zlib still works; such a build
 * writes uncompressed payloads and can still read compressed ones only if
 * zlib is present, which is stated in PERSISTENCE.md.                      */
#define GN_SNAP_FLAG_DEFLATE 1u
#define GN_SNAP_FLAG_ZSTD    2u
/* payload is uncompressed and chunk token arrays are 4-byte aligned, so the
 * file can be mapped and the chunks used in place */
#define GN_SNAP_FLAG_MAPPABLE 4u

/* engine-internal, for the mapped path */
gn_cid   gn_store__put_borrowed(gn_store *s, gn_cid id, gn_tok *toks, uint32_t n);
void     gn_store__set_map(gn_store *s, void *map, size_t len, void *h);
void     gn_store__get_map(const gn_store *s, void **map, size_t *len, void **h);

static int host_is_little_endian(void) {
    const uint32_t x = 1;
    return *(const uint8_t *)&x == 1;
}

#ifdef GN_HAVE_ZLIB
  #include <zlib.h>
#endif
#ifdef GN_HAVE_ZSTD
  #include <zstd.h>
#endif

/* ====================================================================== */
/* portable file helpers                                                  */
/* ====================================================================== */
static int file_sync(FILE *f) {
    if (fflush(f) != 0) return -1;
    return GN_FSYNC(GN_FILENO(f));
}

/* Replace `dst` with `src` atomically. POSIX rename() already is; Windows
 * needs MoveFileEx with REPLACE_EXISTING, and WRITE_THROUGH so the directory
 * entry itself is on the platter before we return.                        */
static int atomic_replace(const char *src, const char *dst) {
#if defined(_WIN32)
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    errno = EACCES;
    return -1;
#else
    if (rename(src, dst) != 0) return -1;
    /* durability of the rename itself needs the containing directory synced */
    char *d = strdup(dst);
    if (d) {
        char *slash = strrchr(d, '/');
        const char *dir = ".";
        if (slash) { *slash = 0; dir = d; }
        int fd = open(dir, O_RDONLY);
        if (fd >= 0) { fsync(fd); close(fd); }
        free(d);
    }
    return 0;
#endif
}

/* ---- exclusive store lock --------------------------------------------
 * Optimistic concurrency needs the read-generation / write-snapshot pair to
 * be atomic against other writers, or two processes both read generation N,
 * both see "no conflict", and one silently overwrites the other.
 *
 * O_EXCL / CREATE_NEW on a lock file is the portable primitive. It is
 * advisory -- a writer that ignores gn_commit() is not stopped -- and a
 * process killed while holding it leaves the file behind, so the lock records
 * its pid and age and gn_lock_break() exists for that case. Stated plainly in
 * PERSISTENCE.md rather than implied to be more than it is.                */
#if defined(_WIN32)
static int lock_acquire(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%lu\n", (unsigned long)GetCurrentProcessId());
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    return 0;
}
#else
static int lock_acquire(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%ld\n", (long)getpid());
    ssize_t rc = write(fd, buf, (size_t)n); (void)rc;
    fsync(fd);
    close(fd);
    return 0;
}
#endif

static void lock_release(const char *path) { remove(path); }

static char *cat2(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + lb + 1);
    if (!s) return NULL;
    memcpy(s, a, la); memcpy(s + la, b, lb + 1);
    return s;
}

/* Read a whole file. Returns NULL and sets *len=0 if absent/unreadable.   */
static uint8_t *slurp_file(const char *path, size_t *len) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    size_t got = sz ? fread(b, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (got != (size_t)sz) { free(b); return NULL; }
    b[got] = 0;
    *len = got;
    return b;
}

/* ====================================================================== */
/* node graph: pointer -> emitted index                                   */
/* ====================================================================== */
typedef struct { const gn_enode *k; uint32_t v; } pent;
typedef struct { pent *t; uint32_t cap, n; } pmap;

static int pmap_init(pmap *m, uint32_t hint) {
    uint32_t c = 1024; while (c < hint * 2u && c < (1u << 31)) c <<= 1;
    m->t = calloc(c, sizeof(pent)); m->cap = c; m->n = 0;
    return m->t ? 0 : -1;
}
static void pmap_free(pmap *m) { free(m->t); m->t = NULL; }

static uint32_t ptr_hash(const void *p) {
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (uint32_t)h;
}
static int pmap_grow(pmap *m) {
    uint32_t oc = m->cap; pent *ot = m->t;
    pent *nt = calloc((size_t)oc * 2, sizeof(pent));
    if (!nt) return -1;
    m->t = nt; m->cap = oc * 2;
    for (uint32_t i = 0; i < oc; i++) {
        if (!ot[i].k) continue;
        uint32_t j = ptr_hash(ot[i].k) & (m->cap - 1);
        while (m->t[j].k) j = (j + 1) & (m->cap - 1);
        m->t[j] = ot[i];
    }
    free(ot);
    return 0;
}
/* returns 1 and sets *out if present; 0 if absent */
static int pmap_get(const pmap *m, const gn_enode *k, uint32_t *out) {
    uint32_t j = ptr_hash(k) & (m->cap - 1);
    while (m->t[j].k) {
        if (m->t[j].k == k) { *out = m->t[j].v; return 1; }
        j = (j + 1) & (m->cap - 1);
    }
    return 0;
}
static int pmap_put(pmap *m, const gn_enode *k, uint32_t v) {
    if ((uint64_t)m->n * 10 > (uint64_t)m->cap * 7) { if (pmap_grow(m)) return -1; }
    uint32_t j = ptr_hash(k) & (m->cap - 1);
    while (m->t[j].k) {
        if (m->t[j].k == k) { m->t[j].v = v; return 0; }
        j = (j + 1) & (m->cap - 1);
    }
    m->t[j].k = k; m->t[j].v = v; m->n++;
    return 0;
}

#define NODE_NIL 0xFFFFFFFFu

/* Emit every node reachable from `root` in post-order (children before
 * parents), skipping nodes already emitted. Explicit stack: version trees
 * after a long editing session can be deeper than a comfortable recursion.  */
typedef struct { const gn_enode *n; int phase; } frame;

typedef struct {
    buf   *out;         /* node records                                    */
    pmap   idx;         /* node -> index                                   */
    uint32_t count;
    frame *st; size_t sp, scap;
    int    err;
} nodewr;

static int nw_push(nodewr *w, const gn_enode *n) {
    if (w->sp == w->scap) {
        size_t nc = w->scap ? w->scap * 2 : 256;
        frame *nf = realloc(w->st, nc * sizeof(frame));
        if (!nf) { w->err = 1; return -1; }
        w->st = nf; w->scap = nc;
    }
    w->st[w->sp].n = n; w->st[w->sp].phase = 0; w->sp++;
    return 0;
}

static void nw_emit(nodewr *w, const gn_enode *n) {
    uint32_t li = NODE_NIL, ri = NODE_NIL;
    if (gn_ext_is_leaf(n)) {
        gn_extent x; uint64_t by;
        gn_ext_leaf_info(n, &x, &by);
        bu8 (w->out, 0);
        bu64(w->out, x.chunk);
        bu32(w->out, x.off);
        bu32(w->out, x.len);
        bu64(w->out, by);
        bu32(w->out, gn_ext_prio(n));
    } else {
        gn_enode *l = gn_ext_left(n), *r = gn_ext_right(n);
        if (l && !pmap_get(&w->idx, l, &li)) { w->err = 1; return; }
        if (r && !pmap_get(&w->idx, r, &ri)) { w->err = 1; return; }
        bu8 (w->out, 1);
        bu32(w->out, li);
        bu32(w->out, ri);
        bu32(w->out, gn_ext_prio(n));
    }
    if (pmap_put(&w->idx, n, w->count)) { w->err = 1; return; }
    w->count++;
}

/* Walk `root`, emitting anything not yet seen. Returns root's index, or
 * NODE_NIL for a NULL root.                                               */
static uint32_t nw_walk(nodewr *w, const gn_enode *root) {
    if (!root) return NODE_NIL;
    uint32_t got;
    if (pmap_get(&w->idx, root, &got)) return got;
    if (nw_push(w, root)) return NODE_NIL;

    while (w->sp > 0 && !w->err) {
        frame *f = &w->st[w->sp - 1];
        uint32_t dummy;
        if (pmap_get(&w->idx, f->n, &dummy)) { w->sp--; continue; }
        if (f->phase == 0) {
            f->phase = 1;
            gn_enode *l = gn_ext_left(f->n);
            if (l) { nw_push(w, l); continue; }
        }
        if (f->phase == 1) {
            f->phase = 2;
            gn_enode *r = gn_ext_right(f->n);
            if (r) { nw_push(w, r); continue; }
        }
        nw_emit(w, f->n);
        w->sp--;
    }
    if (w->err) return NODE_NIL;
    if (!pmap_get(&w->idx, root, &got)) { w->err = 1; return NODE_NIL; }
    return got;
}

/* ====================================================================== */
/* snapshot: write                                                        */
/* ====================================================================== */

/* For a mappable store the payload is split in two:
 *
 *   [ metadata ][ pad ][ chunk token data ]
 *   ^-------- CRC'd ---^                  ^-- mapped, verified on demand
 *
 * The split exists because verifying a CRC over the whole payload reads every
 * byte, which faults in the entire mapping and destroys the only thing
 * out-of-core was for. Measured: opening an 8 MB store cost 33.3 MB resident
 * with a whole-payload CRC and 7.3 MB without it.
 *
 * What is still checked eagerly is everything that drives pointer arithmetic
 * -- dictionary offsets, node indices, extent ranges, version totals. Chunk
 * *content* corruption yields wrong bytes, not unsafe memory, and
 * gn_verify_chunks() checks it on demand.
 */
static int snap_build(gn_engine *e, buf *payload, uint32_t *n_nodes_out,
                      int mappable) {
    gn_dict  *d = gn_engine_dict(e);
    gn_store *s = gn_engine_store(e);

    /* --- dictionary ------------------------------------------------- */
    uint64_t tlen = 0;
    const uint8_t  *texts = gn_dict__texts(d, &tlen);
    const uint32_t *offs  = gn_dict__offs(d);
    const uint32_t *lens  = gn_dict__lens(d);
    uint32_t nent = gn_dict__entries(d);
    bu32(payload, nent);
    bu64(payload, tlen);
    braw(payload, texts, (size_t)tlen);
    for (uint32_t i = 0; i < nent; i++) { bu32(payload, offs[i]); bu32(payload, lens[i]); }

    /* --- chunk store: content-addressed, so exactly once each ---------
     * For a mappable store only (cid, n) goes here; the tokens go to
     * `tokdata`, which is appended after the metadata is padded to a 4-byte
     * file offset (the loader points a gn_tok* straight at those bytes). */
    uint32_t nch = gn_store__count(s);
    bu32(payload, nch);
    bu64(payload, gn_store__deduped(s));
    for (uint32_t i = 0; i < nch; i++) {
        uint32_t n; gn_cid id;
        const gn_tok *t = gn_store__at(s, i, &n, &id);
        bu64(payload, id);
        bu32(payload, n);
        /* For a mappable store the tokens are NOT written here: they are
         * streamed to the file after the metadata, in this same chunk order,
         * so the reader walks their offsets cumulatively. */
        if (!mappable)
            for (uint32_t k = 0; k < n; k++) bu32(payload, t[k]);
    }

    /* --- the version DAG, shared subtrees written once ---------------- */
    buf nodes = {0};
    nodewr w; memset(&w, 0, sizeof w);
    w.out = &nodes;
    if (pmap_init(&w.idx, 4096)) { free(nodes.p); return -1; }

    uint32_t n_obj = gn_engine__n_obj(e);
    /* root index per (object, version), collected while walking */
    uint32_t *roots = NULL; size_t nroots = 0, croots = 0;
    for (uint32_t oi = 0; oi < n_obj; oi++) {
        gn_object *o = gn_engine__obj(e, oi);
        for (uint32_t v = 0; v < o->n_ver; v++) {
            if (nroots == croots) {
                size_t nc = croots ? croots * 2 : 64;
                uint32_t *nr = realloc(roots, nc * sizeof(uint32_t));
                if (!nr) { w.err = 1; goto done_nodes; }
                roots = nr; croots = nc;
            }
            roots[nroots++] = nw_walk(&w, o->ver[v].root);
            if (w.err) goto done_nodes;
        }
    }
done_nodes:
    free(w.st);
    pmap_free(&w.idx);
    if (w.err || nodes.oom) { free(nodes.p); free(roots); return -1; }

    bu32(payload, w.count);
    braw(payload, nodes.p, nodes.n);
    free(nodes.p);
    *n_nodes_out = w.count;

    /* --- objects and their version lists ------------------------------ */
    bu32(payload, n_obj);
    size_t ri = 0;
    for (uint32_t oi = 0; oi < n_obj; oi++) {
        gn_object *o = gn_engine__obj(e, oi);
        size_t nl = strlen(o->name);
        if (nl > 63) nl = 63;
        bu8(payload, (uint8_t)nl);
        braw(payload, o->name, nl);
        bu32(payload, o->n_ver);
        for (uint32_t v = 0; v < o->n_ver; v++) {
            bu32(payload, roots[ri++]);
            bu64(payload, o->ver[v].total_tokens);
            bu64(payload, o->ver[v].total_bytes);
            bu64(payload, o->ver[v].dict_version);
        }
    }
    free(roots);

    /* --- stats -------------------------------------------------------- */
    gn_stats st; gn_engine__stats_get(e, &st);
    bu64(payload, st.chunks_created);
    bu64(payload, st.chunks_deduped);
    bu64(payload, st.tokens_scanned);
    bu64(payload, st.tokens_detokenized);
    bu64(payload, st.bytes_in);
    bu64(payload, st.bytes_resident);

    return payload->oom ? -1 : 0;
}

/* forward decls for WAL lifecycle */
static gn_wal *wal_start(gn_engine *e, const char *path, uint64_t gen);
static void    wal_free(gn_wal *w);

int gn_save(gn_engine *e, const char *path) {
    return gn_save_ex(e, path, 0);
}

int gn_save_ex(gn_engine *e, const char *path, uint32_t save_flags) {
    if (!e || !path) { errno = EINVAL; return -1; }
    const int mappable = (save_flags & GN_SAVE_MAPPABLE) != 0;
    const int nocomp   = mappable || (save_flags & GN_SAVE_RAW) != 0;

    /* An alternative language module (e.g. the video dictionary) may hold
     * state this format cannot express. Refuse loudly instead of writing a
     * store that would reload wrong. */
    if (!gn_dict__serializable(gn_engine_dict(e))) {
#ifdef ENOTSUP
        errno = ENOTSUP;
#else
        errno = EINVAL;
#endif
        return -1;
    }

    gn_wal *old = (gn_wal*)gn_engine__wal(e);
    uint64_t gen = old ? old->gen + 1 : 1;

    buf payload = {0};
    uint32_t n_nodes = 0;
    if (snap_build(e, &payload, &n_nodes, mappable) != 0) {
        free(payload.p); errno = ENOMEM; return -1;
    }

    /* CRC covers the UNCOMPRESSED payload, so integrity is checked on the
     * real data rather than on one particular encoding of it.
     *
     * For a mappable store it covers only the metadata prefix. Checking the
     * bulk here would read every byte of the file at open, faulting in the
     * whole mapping -- measured at 33.3 MB resident instead of 7.3 MB on an
     * 8 MB payload, i.e. the entire benefit. The bulk gets its own CRC in the
     * header, checked by gn_verify_chunks() when a caller asks for it. */
    uint32_t bulk_crc = 0;
    if (mappable) {
        while ((SNAP_HDR_BYTES + payload.n) % 4u) bu8(&payload, 0);
    }
    uint64_t crc_len = (uint64_t)payload.n;

    uint32_t pcrc = crc32_up(0, payload.p, (size_t)crc_len);
    uint64_t raw_len = (uint64_t)payload.n;
    uint32_t flags = 0;
    uint8_t *on_disk = payload.p;
    size_t   on_disk_n = payload.n;
    uint8_t *zbuf = NULL;

    /* zstd is preferred over deflate here for one specific reason: zlib's
     * window is 32 KB, so it cannot see that a chunk near the end of the
     * payload is nearly identical to one near the start. On a store holding
     * many versions of the same data that is exactly the redundancy worth
     * finding, and zstd's window covers it. Measured on evolving geometry,
     * this is the difference between 894 KB and the numbers in SPATIAL.md. */
    /* A mappable store cannot be compressed: the loader points gn_tok* into
     * these exact bytes. This is the whole trade -- bigger on disk, not
     * bounded by RAM. */
    if (mappable) flags |= GN_SNAP_FLAG_MAPPABLE;

#ifdef GN_HAVE_ZSTD
    if (payload.n > 0 && !nocomp) {
        size_t zcap = ZSTD_compressBound(payload.n);
        zbuf = malloc(zcap ? zcap : 1);
        if (zbuf) {
            /* Plain level 19, deliberately.
             *
             * This compressor IS the delta encoder: a one-line edit makes a
             * chunk 99.9% identical to one already stored, content addressing
             * being exact-match shares nothing between them, and it is the
             * payload pass that codes one against the other. Since level 19's
             * window is 8 MB, enabling long-distance matching with a 128 MB
             * window looked like an obvious improvement.
             *
             * It was measured and it is not. On the 8.18 MB redis corpus at
             * 2000 versions, LDM produced a store 64 bytes LARGER (1,999,724
             * vs 1,999,660) and took 7% longer. The window was never the
             * binding constraint, so the parameter is not set. See
             * PERSISTENCE.md 2.6. */
            size_t zn = ZSTD_compress(zbuf, zcap, payload.p, payload.n, 19);
            if (!ZSTD_isError(zn) && zn < payload.n) {
                on_disk = zbuf; on_disk_n = zn;
                flags |= GN_SNAP_FLAG_ZSTD;
            } else { free(zbuf); zbuf = NULL; }
        }
    }
#endif
#ifdef GN_HAVE_ZLIB
    if (payload.n > 0 && !nocomp && !(flags & (GN_SNAP_FLAG_DEFLATE | GN_SNAP_FLAG_ZSTD))) {
        uLongf zcap = compressBound((uLong)payload.n);
        zbuf = malloc(zcap ? zcap : 1);
        if (zbuf) {
            uLongf zn = zcap;
            if (compress2(zbuf, &zn, payload.p, (uLong)payload.n, 6) == Z_OK
                && (size_t)zn < payload.n) {
                on_disk = zbuf; on_disk_n = (size_t)zn;
                flags |= GN_SNAP_FLAG_DEFLATE;
            } else { free(zbuf); zbuf = NULL; }
        }
    }
#endif

    uint8_t hdr[SNAP_HDR_BYTES]; memset(hdr, 0, sizeof hdr);
    buf hb = { hdr, 0, sizeof hdr, 0 };
    braw(&hb, GN_SNAP_MAGIC, 8);
    bu32(&hb, GN_FORMAT_VER);
    bu32(&hb, flags);
    bu64(&hb, gen);
    bu64(&hb, (uint64_t)on_disk_n);      /* bytes that follow the header    */
    bu32(&hb, pcrc);                     /* CRC of payload[0, crc_len)      */
    bu64(&hb, raw_len);                  /* uncompressed length             */
    bu64(&hb, crc_len);                  /* how much of it pcrc covers      */
    bu32(&hb, bulk_crc);                 /* CRC of payload[crc_len, raw_len)*/
    bu32(&hb, crc32_up(0, hdr, 56));     /* header checksum over bytes 0..55 */

    char *tmp = cat2(path, ".tmp");
    if (!tmp) { free(payload.p); errno = ENOMEM; return -1; }

    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); free(payload.p); free(zbuf); return -1; }
    int ok = (fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr)
          && (on_disk_n == 0 || fwrite(on_disk, 1, on_disk_n, f) == on_disk_n);

    /* The bulk is streamed straight from the live store rather than staged
     * into a buffer. Staging it cost three simultaneous copies of the token
     * data and made gn_save_ex fail outright at a 96 MB payload (384 MB of
     * tokens) -- writing a store bigger than RAM must not need RAM. */
    if (ok && mappable) {
        gn_store *s = gn_engine_store(e);
        uint32_t nch = gn_store__count(s);
        gn_tok *swap = NULL; uint32_t swap_cap = 0;
        const int le = host_is_little_endian();
        for (uint32_t i = 0; ok && i < nch; i++) {
            uint32_t n; gn_cid id;
            const gn_tok *t = gn_store__at(s, i, &n, &id);
            if (!n) continue;
            const gn_tok *w = t;
            if (!le) {
                /* The file is little-endian regardless of host, so a store
                 * written on a big-endian machine still loads on a small one
                 * (by copy, not by mapping). */
                if (n > swap_cap) {
                    gn_tok *nt = realloc(swap, (size_t)n * sizeof(gn_tok));
                    if (!nt) { ok = 0; break; }
                    swap = nt; swap_cap = n;
                }
                for (uint32_t k = 0; k < n; k++) {
                    uint32_t v = t[k];
                    swap[k] = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8)
                            | ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
                }
                w = swap;
            }
            size_t nb = (size_t)n * sizeof(gn_tok);
            if (fwrite(w, 1, nb, f) != nb) { ok = 0; break; }
            bulk_crc = crc32_up(bulk_crc, w, nb);
            raw_len += nb;
        }
        free(swap);

        /* Rewrite the header now that the bulk length and CRC are known.
         * Rewinding is safe: this is the private .tmp file, not the store. */
        if (ok) {
            hb.n = 24;
            bu64(&hb, raw_len);              /* on_disk_n == raw_len here  */
            bu32(&hb, pcrc);
            bu64(&hb, raw_len);
            bu64(&hb, crc_len);
            bu32(&hb, bulk_crc);
            bu32(&hb, crc32_up(0, hdr, 56));
            ok = (fseek(f, 0, SEEK_SET) == 0)
              && (fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr);
        }
    }

    if (ok) ok = (file_sync(f) == 0);
    if (fclose(f) != 0) ok = 0;
    free(payload.p);
    free(zbuf);

    if (!ok) { remove(tmp); free(tmp); return -1; }
    if (atomic_replace(tmp, path) != 0) { remove(tmp); free(tmp); return -1; }
    free(tmp);

    /* The snapshot is now the checkpoint: start a fresh log at the new
     * generation. An old log left over from a crash carries the previous
     * generation and is ignored by gn_open.                                */
    if (old) { wal_free(old); gn_engine__set_wal(e, NULL); }
    gn_wal *w = wal_start(e, path, gen);
    if (!w) return -1;
    gn_engine__set_wal(e, w);
    return 0;
}

/* ====================================================================== */
/* memory mapping                                                         */
/* ====================================================================== */

/* Map a file read-only. Returns the base pointer, or NULL. */
static uint8_t *map_file(const char *path, size_t *len_out, void **handle) {
    *len_out = 0; *handle = NULL;
#if defined(_WIN32)
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) { CloseHandle(f); return NULL; }
    HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);
    if (!m) return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(m); return NULL; }
    *len_out = (size_t)sz.QuadPart;
    *handle = m;                       /* keep the section alive           */
    return (uint8_t *)p;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)sz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *len_out = (size_t)sz;
    return (uint8_t *)p;
#endif
}

void gn_store_unmap(gn_store *s) {
    if (!s) return;
    void *map = NULL, *h = NULL; size_t len = 0;
    gn_store__get_map(s, &map, &len, &h);
    if (!map) return;
#if defined(_WIN32)
    UnmapViewOfFile(map);
    if (h) CloseHandle((HANDLE)h);
#else
    munmap(map, len);
#endif
    gn_store__set_map(s, NULL, 0, NULL);
}

/* ====================================================================== */
/* snapshot: read                                                         */
/* ====================================================================== */

/* `mapped_base` non-NULL means fbuf IS a mapping we may borrow from; chunk
 * token arrays are then installed by pointer instead of being copied. */
static gn_engine *snap_load_ex(const uint8_t *fbuf, size_t flen,
                               uint64_t *gen_out, const uint8_t *mapped_base);

static gn_engine *snap_load(const uint8_t *fbuf, size_t flen, uint64_t *gen_out) {
    return snap_load_ex(fbuf, flen, gen_out, NULL);
}

static gn_engine *snap_load_ex(const uint8_t *fbuf, size_t flen,
                               uint64_t *gen_out, const uint8_t *mapped_base) {
    if (flen < SNAP_HDR_BYTES) { errno = EINVAL; return NULL; }
    if (memcmp(fbuf, GN_SNAP_MAGIC, 8) != 0) { errno = EINVAL; return NULL; }

    rdr h = { fbuf, SNAP_HDR_BYTES, 8, 0 };
    uint32_t ver   = ru32(&h);
    uint32_t flags = ru32(&h);
    uint64_t gen   = ru64(&h);
    uint64_t plen  = ru64(&h);            /* bytes on disk after the header */
    uint32_t pcrc  = ru32(&h);            /* CRC of the UNCOMPRESSED payload */
    uint64_t rawlen= ru64(&h);
    uint64_t crclen= ru64(&h);            /* how much of the payload pcrc covers */
    uint32_t bcrc  = ru32(&h);            /* CRC of the uncovered remainder      */
    uint32_t hcrc  = ru32(&h);
    (void)bcrc;                           /* checked by gn_verify_chunks() */
    if (h.err || ver != GN_FORMAT_VER)              { errno = EINVAL; return NULL; }
    if (hcrc != crc32_up(0, fbuf, 56))              { errno = EINVAL; return NULL; }
    if (plen > flen - SNAP_HDR_BYTES)               { errno = EINVAL; return NULL; }
    if (crclen > rawlen)                            { errno = EINVAL; return NULL; }

    const uint8_t *pl = fbuf + SNAP_HDR_BYTES;
    uint8_t *inflated = NULL;
    if (flags & GN_SNAP_FLAG_ZSTD) {
#ifdef GN_HAVE_ZSTD
        if (rawlen > (uint64_t)SIZE_MAX)            { errno = EINVAL; return NULL; }
        inflated = malloc(rawlen ? (size_t)rawlen : 1);
        if (!inflated)                              { errno = ENOMEM; return NULL; }
        size_t out = ZSTD_decompress(inflated, (size_t)rawlen, pl, (size_t)plen);
        if (ZSTD_isError(out) || (uint64_t)out != rawlen) {
            free(inflated); errno = EINVAL; return NULL;
        }
        pl = inflated; plen = rawlen;
#else
        errno = ENOTSUP; return NULL;
#endif
    } else if (flags & GN_SNAP_FLAG_DEFLATE) {
#ifdef GN_HAVE_ZLIB
        if (rawlen > (uint64_t)SIZE_MAX)            { errno = EINVAL; return NULL; }
        inflated = malloc(rawlen ? (size_t)rawlen : 1);
        if (!inflated)                              { errno = ENOMEM; return NULL; }
        uLongf out = (uLongf)rawlen;
        if (uncompress(inflated, &out, pl, (uLong)plen) != Z_OK
            || (uint64_t)out != rawlen) {
            free(inflated); errno = EINVAL; return NULL;
        }
        pl = inflated; plen = rawlen;
#else
        /* A compressed store cannot be read without zlib. Say so rather than
         * failing with a checksum error that suggests corruption. */
        errno = ENOTSUP; return NULL;
#endif
    } else if (rawlen && rawlen != plen) {
        errno = EINVAL; return NULL;
    }

    /* A non-mappable store sets crclen == rawlen, so this is the whole
     * payload exactly as before. A mappable one stops at the metadata. */
    if (rawlen && crclen == 0) { errno = EINVAL; return NULL; }
    if (pcrc != crc32_up(0, pl, (size_t)(rawlen ? crclen : plen))) {
        free(inflated); errno = EINVAL; return NULL;
    }

    /* Where the mapped bulk begins, and a cursor walking it. Chunk token
     * arrays were written in chunk-index order, so offsets accumulate. */
    const uint8_t *bulk = pl + (size_t)crclen;
    size_t bulk_left = (size_t)(rawlen - crclen);
    const int map_fmt = (flags & GN_SNAP_FLAG_MAPPABLE) != 0;

    /* The metadata reader must not run off into the bulk region. */
    rdr r = { pl, (size_t)(rawlen ? crclen : plen), 0, 0 };
    gn_engine *e = gn_engine_new();
    if (!e) { free(inflated); errno = ENOMEM; return NULL; }

    /* If a monoid is registered, the leaves built below will compute their
     * annotations against whatever store the monoid was attached to -- which
     * is not this one, because this engine did not exist when the caller
     * attached. Point it here for the duration of the load. */
    gn_ext_monoid_bind(gn_engine_store(e));

    gn_enode **nodes = NULL;
    uint32_t   n_nodes = 0, built = 0;

    /* --- dictionary --------------------------------------------------- */
    uint32_t nent = ru32(&r);
    uint64_t tlen = ru64(&r);
    const uint8_t *texts = rraw(&r, (size_t)tlen);
    if (r.err) goto bad;
    /* each entry is {u32 off; u32 len} = 8 bytes */
    if (!rfits(&r, nent, 8)) { errno = EINVAL; goto bad; }
    {
        uint32_t *offs = NULL, *lens = NULL;
        if (nent) {
            offs = malloc((size_t)nent * 4);
            lens = malloc((size_t)nent * 4);
            if (!offs || !lens) { free(offs); free(lens); goto bad; }
            for (uint32_t i = 0; i < nent; i++) { offs[i] = ru32(&r); lens[i] = ru32(&r); }
        }
        if (r.err) { free(offs); free(lens); goto bad; }
        int rc = gn_dict__restore(gn_engine_dict(e), texts, tlen, offs, lens, nent);
        free(offs); free(lens);
        if (rc != 0) goto bad;
    }

    /* --- chunk store: re-put in the original order, so the open-addressed
     * index lands in exactly the same shape it had before the save.       */
    {
        uint32_t nch = ru32(&r);
        uint64_t dedup = ru64(&r);
        if (r.err) goto bad;
        /* smallest chunk record is {u64 cid; u32 n} = 12 bytes of metadata */
        if (!rfits(&r, nch, 12)) { errno = EINVAL; goto bad; }
        gn_store *s = gn_engine_store(e);
        gn_tok *tmp = NULL; uint32_t tcap = 0;
        for (uint32_t i = 0; i < nch; i++) {
            uint64_t id = ru64(&r);
            uint32_t n  = ru32(&r);
            if (r.err) { free(tmp); goto bad; }
            /* A chunk's tokens must fit in what is left: in the bulk region
             * for a mappable store (checked below), otherwise inline here.
             * Without this a flipped byte in n_tokens is a multi-GB malloc. */
            if (!map_fmt && !rfits(&r, n, 4)) { free(tmp); errno = EINVAL; goto bad; }

            if (map_fmt) {
                /* Tokens live in the trailing bulk region, written in this
                 * same chunk order, so offsets accumulate. */
                size_t want = (size_t)n * sizeof(gn_tok);
                if (want > bulk_left) { free(tmp); errno = EINVAL; goto bad; }

                if (mapped_base) {
                    /* Zero copy: hand the store a pointer into the mapping.
                     * These bytes ARE the gn_tok array -- the caller checked
                     * 4-byte alignment and little-endianness first.
                     *
                     * Nothing here dereferences them, which is the point: a
                     * chunk's pages stay unfaulted until a read reaches it. */
                    gn_cid got = gn_store__put_borrowed(
                        s, id, (gn_tok *)(void *)(uintptr_t)bulk, n);
                    if (got != id) { free(tmp); errno = EINVAL; goto bad; }
                    bulk += want; bulk_left -= want;
                    continue;
                }

                /* Not mappable on this host (big-endian, unaligned mapping,
                 * or mmap refused). Correctness does not depend on mapping:
                 * copy and byte-swap instead, giving up only the RAM win. */
                if (n > tcap) {
                    gn_tok *nt = realloc(tmp, (size_t)n * sizeof(gn_tok));
                    if (!nt) { free(tmp); goto bad; }
                    tmp = nt; tcap = n;
                }
                for (uint32_t k = 0; k < n; k++) {
                    const uint8_t *b = bulk + (size_t)k * 4u;
                    tmp[k] = (uint32_t)b[0]        | ((uint32_t)b[1] << 8)
                           | ((uint32_t)b[2] << 16)| ((uint32_t)b[3] << 24);
                }
                bulk += want; bulk_left -= want;
            } else {
                if (n > tcap) {
                    gn_tok *nt = realloc(tmp, (size_t)n * sizeof(gn_tok));
                    if (!nt) { free(tmp); goto bad; }
                    tmp = nt; tcap = n;
                }
                for (uint32_t k = 0; k < n; k++) tmp[k] = ru32(&r);
                if (r.err) { free(tmp); goto bad; }
            }
            gn_cid got = gn_store_put(s, tmp, n);
            if (got != id) { free(tmp); errno = EINVAL; goto bad; }  /* corrupt */
        }
        free(tmp);
        gn_store__set_deduped(s, dedup);
    }

    /* --- version DAG -------------------------------------------------- */
    n_nodes = ru32(&r);
    if (r.err) goto bad;
    /* smallest node record is an inner node: kind + left + right + prio = 13 */
    if (!rfits(&r, n_nodes, 13)) { errno = EINVAL; goto bad; }
    if (n_nodes) {
        nodes = calloc(n_nodes, sizeof(gn_enode*));
        if (!nodes) goto bad;
    }
    for (uint32_t i = 0; i < n_nodes; i++) {
        uint8_t kind = ru8(&r);
        gn_enode *n = NULL;
        if (kind == 0) {
            gn_extent x;
            x.chunk      = ru64(&r);
            x.off        = ru32(&r);
            x.len        = ru32(&r);
            uint64_t by  = ru64(&r);
            uint32_t pr  = ru32(&r);
            if (r.err) goto bad;

            /* The engine assumes every extent lies inside its chunk -- it
             * built them all, so read_leaf() indexes chunk tokens without
             * checking. Materializing a tree from bytes on disk breaks that
             * assumption, and an out-of-range (off,len) becomes a read past
             * the chunk allocation. Uphold the invariant here, at the trust
             * boundary, rather than adding a bounds test to the hot path. */
            {
                size_t cn = 0;
                const gn_tok *ct = gn_store_get(gn_engine_store(e), x.chunk, &cn);
                if (!ct || (uint64_t)x.off + x.len > (uint64_t)cn) {
                    errno = EINVAL; goto bad;
                }
            }
            n = gn_ext_mk_leaf_p(x, by, pr);
        } else if (kind == 1) {
            uint32_t li = ru32(&r), ri2 = ru32(&r), pr = ru32(&r);
            if (r.err) goto bad;
            /* children must already exist: post-order guarantees index < i  */
            if ((li != NODE_NIL && li >= i) || (ri2 != NODE_NIL && ri2 >= i)) {
                errno = EINVAL; goto bad;
            }
            n = gn_ext_mk_inner_p(li == NODE_NIL ? NULL : nodes[li],
                                  ri2 == NODE_NIL ? NULL : nodes[ri2], pr);
        } else { errno = EINVAL; goto bad; }
        if (!n) goto bad;
        /* Loader reference: every node is held by the loader until wiring is
         * finished, so a cascade can never free a node we still need. The
         * refs are dropped in one pass at the end, leaving each node with
         * exactly (parents + version roots) references -- and freeing any
         * node a malformed file left unreferenced.                          */
        gn_ext_retain(n);
        nodes[i] = n;
        built = i + 1;
    }

    /* --- objects ------------------------------------------------------- */
    {
        uint32_t n_obj = ru32(&r);
        if (r.err) goto bad;
        /* smallest object record: nl + 1 name byte + n_ver + one version
         * {u32 root; u64 tokens; u64 bytes; u64 dict} = 1+1+4+28 = 34 */
        if (!rfits(&r, n_obj, 34)) { errno = EINVAL; goto bad; }
        for (uint32_t oi = 0; oi < n_obj; oi++) {
            uint8_t nl = ru8(&r);
            const uint8_t *nm = rraw(&r, nl);
            uint32_t n_ver = ru32(&r);
            /* gn_object::name is char[64]; a corrupt (or crafted) length byte
             * of up to 255 would otherwise overflow it. */
            if (r.err || n_ver == 0 || nl > 63) { errno = EINVAL; goto bad; }
            /* each version is {u32 root; u64 tokens; u64 bytes; u64 dict} */
            if (!rfits(&r, n_ver, 28)) { errno = EINVAL; goto bad; }

            gn_object *o = calloc(1, sizeof *o);
            if (!o) goto bad;
            memcpy(o->name, nm, nl); o->name[nl] = 0;
            o->ver = calloc(n_ver, sizeof(gn_version));
            if (!o->ver) { free(o); goto bad; }
            o->n_ver = n_ver;
            for (uint32_t v = 0; v < n_ver; v++) {
                uint32_t ridx = ru32(&r);
                o->ver[v].total_tokens = ru64(&r);
                o->ver[v].total_bytes  = ru64(&r);
                o->ver[v].dict_version = ru64(&r);
                o->ver[v].ext = NULL; o->ver[v].n_ext = 0;   /* v1/v2 arm only */

                gn_enode *root = NULL;
                int ok = !r.err && (ridx == NODE_NIL || ridx < n_nodes);
                if (ok) {
                    root = (ridx == NODE_NIL) ? NULL : nodes[ridx];
                    /* integrity: the tree must agree with the recorded totals */
                    ok = gn_ext_tokens(root) == o->ver[v].total_tokens &&
                         gn_ext_bytes(root)  == o->ver[v].total_bytes;
                }
                if (!ok) {
                    /* release the roots retained by EARLIER versions of this
                     * object -- they are not reachable from the engine yet, so
                     * the loader's own cleanup pass will not cover them. */
                    for (uint32_t k = 0; k < v; k++) gn_ext_release(o->ver[k].root);
                    free(o->ver); free(o); errno = EINVAL; goto bad;
                }
                o->ver[v].root = gn_ext_retain(root);
            }
            gn_engine__attach(e, o);
        }
    }

    /* --- stats ---------------------------------------------------------- */
    {
        gn_stats st; memset(&st, 0, sizeof st);
        st.chunks_created     = ru64(&r);
        st.chunks_deduped     = ru64(&r);
        st.tokens_scanned     = ru64(&r);
        st.tokens_detokenized = ru64(&r);
        st.bytes_in           = ru64(&r);
        st.bytes_resident     = ru64(&r);
        if (r.err) goto bad;
        gn_engine__stats_set(e, &st);
    }

    for (uint32_t i = 0; i < built; i++) gn_ext_release(nodes[i]);
    free(nodes);
    free(inflated);
    *gen_out = gen;
    return e;

bad:
    if (!errno) errno = EINVAL;
    {   int save = errno;
        for (uint32_t i = 0; i < built; i++) gn_ext_release(nodes[i]);
        free(nodes);
        free(inflated);
        gn_engine_free(e);
        errno = save;
    }
    return NULL;
}

/* ====================================================================== */
/* WAL                                                                    */
/* ====================================================================== */

static void wal_free(gn_wal *w) {
    if (!w) return;
    if (w->f) fclose(w->f);
    free(w->path); free(w->wpath); free(w);
}

/* Truncate the log file and write a valid header pinning `gen`.           */
static int wal_write_header(const char *wpath, uint64_t gen) {
    FILE *f = fopen(wpath, "wb");
    if (!f) return -1;
    uint8_t hdr[WAL_HDR_BYTES]; memset(hdr, 0, sizeof hdr);
    buf hb = { hdr, 0, sizeof hdr, 0 };
    braw(&hb, GN_WAL_MAGIC, 8);
    bu32(&hb, GN_FORMAT_VER);
    bu32(&hb, 0);
    bu64(&hb, gen);
    bu32(&hb, crc32_up(0, hdr, 24));
    int ok = (fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr) && (file_sync(f) == 0);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* Is the log on disk a valid log for THIS snapshot generation?
 * Missing, truncated, corrupt, or from a superseded generation all answer 0. */
static int wal_header_matches(const char *wpath, uint64_t gen) {
    FILE *f = fopen(wpath, "rb");
    if (!f) return 0;
    uint8_t hdr[WAL_HDR_BYTES];
    size_t got = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (got != sizeof hdr) return 0;
    if (memcmp(hdr, GN_WAL_MAGIC, 8) != 0) return 0;
    rdr h = { hdr, sizeof hdr, 8, 0 };
    uint32_t ver = ru32(&h); (void)ru32(&h);
    uint64_t g   = ru64(&h);
    uint32_t hc  = ru32(&h);
    if (h.err || ver != GN_FORMAT_VER || g != gen) return 0;
    return hc == crc32_up(0, hdr, 24);
}

/* Create/truncate the log and write its header. The header pins the
 * generation of the snapshot the records apply to.                        */
static gn_wal *wal_start(gn_engine *e, const char *path, uint64_t gen) {
    gn_wal *w = calloc(1, sizeof *w);
    if (!w) { errno = ENOMEM; return NULL; }
    w->e = e; w->gen = gen; w->sync = 1;
    w->path  = cat2(path, "");
    w->wpath = cat2(path, ".wal");
    if (!w->path || !w->wpath) { wal_free(w); errno = ENOMEM; return NULL; }

    if (wal_write_header(w->wpath, gen) != 0) { wal_free(w); return NULL; }

    /* reopen for append */
    w->f = fopen(w->wpath, "ab");
    if (!w->f) { wal_free(w); return NULL; }
    return w;
}

/* Append one record and (by default) fsync it. This runs BEFORE the edit is
 * applied in memory, which is the entire guarantee: if the edit is visible
 * to anyone, its record is already durable.                                */
static void wal_append(gn_wal *w, const buf *rec) {
    if (!w || !w->f || w->replaying) return;

    /* Any failure here means the edit about to be applied will NOT be in the
     * log, which is precisely the guarantee this layer sells. There is no
     * way to fail the edit from inside the engine's mutator, so record it:
     * the WAL is latched broken and gn_wal_ok() reports it. Silently
     * returning would leave a store that looks durable and is not. */
    if (rec->oom || rec->n > 0xFFFFFFFFu) { w->broken = 1; return; }

    uint8_t pre[8];
    buf pb = { pre, 0, sizeof pre, 0 };
    bu32(&pb, (uint32_t)rec->n);
    bu32(&pb, crc32_up(0, rec->p, rec->n));
    if (fwrite(pre, 1, 8, w->f) != 8)                     { w->broken = 1; return; }
    if (rec->n && fwrite(rec->p, 1, rec->n, w->f) != rec->n) { w->broken = 1; return; }
    if (w->sync) { if (file_sync(w->f) != 0)              { w->broken = 1; return; } }
    else         { if (fflush(w->f) != 0)                 { w->broken = 1; return; } }
    w->records++;
    w->wbytes += 8 + rec->n;
}

static void wname(buf *b, const char *name) {
    size_t n = strlen(name);
    if (n > 63) n = 63;
    bu8(b, (uint8_t)n);
    braw(b, name, n);
}

void gn_wal__create(gn_engine *e, const char *name, const uint8_t *text, size_t len) {
    gn_wal *w = (gn_wal*)gn_engine__wal(e);
    if (!w || w->replaying) return;
    buf b = {0};
    bu8(&b, WOP_CREATE); wname(&b, name);
    bu64(&b, (uint64_t)len); braw(&b, text, len);
    wal_append(w, &b); free(b.p);
}
void gn_wal__update(gn_engine *e, const char *name, uint64_t off,
                    uint64_t del, const uint8_t *text, size_t len) {
    gn_wal *w = (gn_wal*)gn_engine__wal(e);
    if (!w || w->replaying) return;
    buf b = {0};
    bu8(&b, WOP_UPDATE); wname(&b, name);
    bu64(&b, off); bu64(&b, del);
    bu64(&b, (uint64_t)len); braw(&b, text, len);
    wal_append(w, &b); free(b.p);
}
void gn_wal__delete(gn_engine *e, const char *name) {
    gn_wal *w = (gn_wal*)gn_engine__wal(e);
    if (!w || w->replaying) return;
    buf b = {0};
    bu8(&b, WOP_DELETE); wname(&b, name);
    wal_append(w, &b); free(b.p);
}
void gn_wal__graft(gn_engine *e, const char *dst, uint64_t dst_off,
                   const char *src, uint64_t src_off, uint64_t src_len) {
    gn_wal *w = (gn_wal*)gn_engine__wal(e);
    if (!w || w->replaying) return;
    buf b = {0};
    bu8(&b, WOP_GRAFT); wname(&b, dst); wname(&b, src);
    bu64(&b, dst_off); bu64(&b, src_off); bu64(&b, src_len);
    wal_append(w, &b); free(b.p);
}
void gn_wal__cut(gn_engine *e, const char *name, uint64_t off, uint64_t len) {
    gn_wal *w = (gn_wal*)gn_engine__wal(e);
    if (!w || w->replaying) return;
    buf b = {0};
    bu8(&b, WOP_CUT); wname(&b, name); bu64(&b, off); bu64(&b, len);
    wal_append(w, &b); free(b.p);
}
void gn_wal__trim(gn_engine *e, const char *name, uint32_t keep) {
    gn_wal *w = (gn_wal*)gn_engine__wal(e);
    if (!w || w->replaying) return;
    buf b = {0};
    bu8(&b, WOP_TRIM); wname(&b, name); bu32(&b, keep);
    wal_append(w, &b); free(b.p);
}

/* ---- replay ----------------------------------------------------------- */

static char *rname(rdr *r) {
    static char nm[64];
    uint8_t n = ru8(r);
    if (n > 63) { r->err = 1; return NULL; }
    const uint8_t *p = rraw(r, n);
    if (!p) return NULL;
    memcpy(nm, p, n); nm[n] = 0;
    return nm;
}

/* Replay every intact record. Stops at the first record that is short or
 * fails its checksum -- that is the tear left by the kill, and everything
 * after it is by definition not committed.                                 */
static uint64_t wal_replay(gn_engine *e, const char *wpath, uint64_t gen,
                           uint64_t *good_end, uint64_t *file_len) {
    size_t flen = 0;
    uint8_t *fb = slurp_file(wpath, &flen);
    if (good_end) *good_end = 0;
    if (file_len) *file_len = 0;
    if (!fb) return 0;
    if (file_len) *file_len = flen;

    uint64_t applied = 0;
    if (flen < WAL_HDR_BYTES || memcmp(fb, GN_WAL_MAGIC, 8) != 0) { free(fb); return 0; }
    {
        rdr h = { fb, WAL_HDR_BYTES, 8, 0 };
        uint32_t ver = ru32(&h); (void)ru32(&h);
        uint64_t g   = ru64(&h);
        uint32_t hc  = ru32(&h);
        /* A log from a previous generation belongs to a snapshot we have
         * already superseded: ignore it rather than replaying stale edits. */
        if (h.err || ver != GN_FORMAT_VER || g != gen ||
            hc != crc32_up(0, fb, 24)) { free(fb); return 0; }
    }

    gn_wal *w = (gn_wal*)gn_engine__wal(e);
    int saved = w ? w->replaying : 0;
    if (w) w->replaying = 1;

    size_t at = WAL_HDR_BYTES;
    while (at + 8 <= flen) {
        rdr pre = { fb, flen, at, 0 };
        uint32_t rlen = ru32(&pre), rcrc = ru32(&pre);
        if (pre.err) break;
        if (rlen == 0 || at + 8 + rlen > flen) break;        /* torn tail    */
        const uint8_t *body = fb + at + 8;
        if (crc32_up(0, body, rlen) != rcrc) break;          /* torn record  */

        rdr r = { body, rlen, 0, 0 };
        uint8_t op = ru8(&r);
        char name[64];
        switch (op) {
        case WOP_CREATE: {
            char *nm = rname(&r); if (!nm) goto stop;
            snprintf(name, sizeof name, "%s", nm);
            uint64_t len = ru64(&r);
            const uint8_t *txt = rraw(&r, (size_t)len);
            if (r.err) goto stop;
            gn_create(e, name, txt, (size_t)len);
            break;
        }
        case WOP_UPDATE: {
            char *nm = rname(&r); if (!nm) goto stop;
            snprintf(name, sizeof name, "%s", nm);
            uint64_t off = ru64(&r), del = ru64(&r), len = ru64(&r);
            const uint8_t *txt = rraw(&r, (size_t)len);
            if (r.err) goto stop;
            gn_object *o = gn_object_open(e, name);
            if (o) gn_update(e, o, off, del, len ? txt : NULL, (size_t)len);
            break;
        }
        case WOP_DELETE: {
            char *nm = rname(&r); if (!nm) goto stop;
            snprintf(name, sizeof name, "%s", nm);
            if (r.err) goto stop;
            gn_delete(e, name);
            break;
        }
        case WOP_GRAFT: {
            char *nm = rname(&r); if (!nm) goto stop;
            snprintf(name, sizeof name, "%s", nm);
            char sname[64];
            char *sn = rname(&r); if (!sn) goto stop;
            snprintf(sname, sizeof sname, "%s", sn);
            uint64_t doff = ru64(&r), soff = ru64(&r), slen = ru64(&r);
            if (r.err) goto stop;
            gn_object *d = gn_object_open(e, name), *s = gn_object_open(e, sname);
            if (d && s) gn_graft(e, d, doff, s, soff, slen);
            break;
        }
        case WOP_CUT: {
            char *nm = rname(&r); if (!nm) goto stop;
            snprintf(name, sizeof name, "%s", nm);
            uint64_t off = ru64(&r), len = ru64(&r);
            if (r.err) goto stop;
            gn_object *o = gn_object_open(e, name);
            if (o) gn_cut(e, o, off, len);
            break;
        }
        case WOP_TRIM: {
            char *nm = rname(&r); if (!nm) goto stop;
            snprintf(name, sizeof name, "%s", nm);
            uint32_t keep = ru32(&r);
            if (r.err) goto stop;
            gn_object *o = gn_object_open(e, name);
            if (o) gn_trim_history(e, o, keep);
            break;
        }
        default: goto stop;                                  /* unknown op   */
        }
        applied++;
        at += 8 + rlen;
        if (good_end) *good_end = at;   /* end of the last INTACT record */
    }
stop:
    if (w) { w->replaying = saved; w->replayed = applied; }
    free(fb);
    return applied;
}

/* ====================================================================== */
/* open                                                                   */
/* ====================================================================== */

gn_engine *gn_open(const char *path) {
    if (!path) { errno = EINVAL; return NULL; }

    uint64_t gen = 0;
    gn_engine *e = NULL;
    uint8_t *mapping = NULL; size_t map_len = 0; void *map_h = NULL;

    /* Take the mapped path only if the store was written for it AND the host
     * can use the bytes as they lie. Otherwise fall through to reading and
     * copying, which always works. */
    {
        uint8_t probe[SNAP_HDR_BYTES];
        FILE *pf = fopen(path, "rb");
        if (!pf) { errno = ENOENT; return NULL; }
        size_t got = fread(probe, 1, sizeof probe, pf);
        fclose(pf);
        if (got == sizeof probe && memcmp(probe, GN_SNAP_MAGIC, 8) == 0) {
            uint32_t fl = (uint32_t)probe[12] | ((uint32_t)probe[13] << 8)
                        | ((uint32_t)probe[14] << 16) | ((uint32_t)probe[15] << 24);
            if ((fl & GN_SNAP_FLAG_MAPPABLE) && host_is_little_endian()) {
                mapping = map_file(path, &map_len, &map_h);
                if (mapping) {
                    /* the mapping base is page-aligned, and the format put
                     * chunk data on 4-byte boundaries from the file start */
                    if (((uintptr_t)mapping & 3u) == 0)
                        e = snap_load_ex(mapping, map_len, &gen, mapping);
                        /* NB: nothing above this point reads the bulk. Adding
                         * any full-payload pass here (a CRC, a byte-length
                         * precompute) silently reverts out-of-core to a full
                         * fault-in -- that regression is what oocore_test
                         * measures. */
                    if (!e) {
                        gn_store *dummy = NULL; (void)dummy;
#if defined(_WIN32)
                        UnmapViewOfFile(mapping);
                        if (map_h) CloseHandle((HANDLE)map_h);
#else
                        munmap(mapping, map_len);
#endif
                        mapping = NULL; map_h = NULL; map_len = 0;
                    }
                }
            }
        }
    }

    if (!e) {
        size_t flen = 0;
        uint8_t *fb = slurp_file(path, &flen);
        if (!fb) { if (!errno) errno = ENOENT; return NULL; }
        e = snap_load(fb, flen, &gen);
        free(fb);
        if (!e) return NULL;
    } else {
        /* the store now owns the mapping: its chunks point into it */
        gn_store__set_map(gn_engine_store(e), mapping, map_len, map_h);
    }

    /* Attach the log BEFORE replay so the replayed ops see a live (but
     * suppressed) handle, then leave it attached for continued editing.
     * The existing log file is kept as-is: replay always starts from the
     * snapshot, so re-replaying it after another crash is idempotent.      */
    char *wpath = cat2(path, ".wal");
    if (!wpath) { gn_engine_free(e); errno = ENOMEM; return NULL; }

    gn_wal *w = calloc(1, sizeof *w);
    if (!w) { free(wpath); gn_engine_free(e); errno = ENOMEM; return NULL; }
    w->e = e; w->gen = gen; w->sync = 1; w->f = NULL;
    w->path = cat2(path, ""); w->wpath = wpath;
    if (!w->path) { wal_free(w); gn_engine_free(e); errno = ENOMEM; return NULL; }
    gn_engine__set_wal(e, w);

    /* Is the log on disk one we can extend?
     *
     * If it is missing, corrupt, or from a superseded generation, we must
     * REPLACE it, not append to it. Opening a missing file "ab" would create
     * an empty, HEADERLESS log; every edit written there afterwards would be
     * rejected by the magic check on the next open and silently lost. A
     * stale-generation log is the same trap: its records are ignored, so
     * anything appended to it is ignored too.                              */
#ifdef GN_WALFIX_OFF
    int usable = 1;   /* pre-fix behaviour, for tools/verify_walfix.sh only */
#else
    int usable = wal_header_matches(wpath, gen);
#endif

    uint64_t good_end = 0, wal_len = 0;
    uint64_t n = usable ? wal_replay(e, wpath, gen, &good_end, &wal_len) : 0;
    w->replayed = n;

    if (!usable && wal_write_header(wpath, gen) != 0) {
        gn_engine__set_wal(e, NULL); wal_free(w);
        return e;                     /* store is loaded; logging is off */
    }

    /* Replay stops at the first torn record. If we then appended after it,
     * the tear would sit between the old records and the new ones, and the
     * NEXT open would stop there and silently ignore everything written
     * since. Cut the log back to the end of the last intact record so the
     * append point is clean. If the truncate fails we must not append into
     * a log we know is unreadable past this point -- log nothing instead. */
#ifndef GN_WALFIX_OFF
    if (good_end < WAL_HDR_BYTES) good_end = WAL_HDR_BYTES;
    if (usable && good_end < wal_len) {
        if (file_truncate(wpath, good_end) != 0) {
            gn_engine__set_wal(e, NULL); wal_free(w);
            return e;
        }
    }
#endif

    /* Resume appending to a log that is valid for this generation.         */
    w->f = fopen(wpath, "ab");
    if (!w->f) {
        /* Read-only medium: the store is loaded and correct, but further
         * edits cannot be logged. Say so rather than pretending.           */
        gn_engine__set_wal(e, NULL);
        wal_free(w);
    }
    return e;
}

/* ---- optimistic concurrency ------------------------------------------ */

/* Read a snapshot's generation without loading it. This is the version token
 * a writer holds while it works. */
uint64_t gn_store_generation(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t hdr[SNAP_HDR_BYTES];
    size_t got = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (got != sizeof hdr || memcmp(hdr, GN_SNAP_MAGIC, 8) != 0) return 0;
    rdr h = { hdr, sizeof hdr, 16, 0 };
    uint64_t gen = ru64(&h);
    return h.err ? 0 : gen;
}

/* The bulk check gn_open() skips for a mappable store. Streams the tail of
 * the file so verifying a store bigger than RAM does not require RAM. */
int gn_verify_chunks(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[SNAP_HDR_BYTES];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr
        || memcmp(hdr, GN_SNAP_MAGIC, 8) != 0) {
        fclose(f); errno = EINVAL; return -1;
    }
    rdr h = { hdr, sizeof hdr, 8, 0 };
    uint32_t ver = ru32(&h); (void)ru32(&h); (void)ru64(&h);
    (void)ru64(&h); (void)ru32(&h);
    uint64_t rawlen = ru64(&h);
    uint64_t crclen = ru64(&h);
    uint32_t bcrc   = ru32(&h);
    if (h.err || ver != GN_FORMAT_VER) { fclose(f); errno = EINVAL; return -1; }
    if (crclen >= rawlen) { fclose(f); return 1; }   /* no separate bulk */

    if (fseek(f, (long)(SNAP_HDR_BYTES + crclen), SEEK_SET) != 0) {
        fclose(f); return -1;
    }
    uint64_t left = rawlen - crclen;
    uint32_t crc = 0;
    uint8_t *io = malloc(1u << 20);
    if (!io) { fclose(f); errno = ENOMEM; return -1; }
    while (left) {
        size_t want = left > (1u << 20) ? (1u << 20) : (size_t)left;
        if (fread(io, 1, want, f) != want) {
            free(io); fclose(f); errno = EIO; return -1;
        }
        crc = crc32_up(crc, io, want);
        left -= want;
    }
    free(io); fclose(f);
    if (crc != bcrc) { errno = EINVAL; return -1; }
    return 0;
}

uint64_t gn_engine_generation(const gn_engine *e) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL;
    return w ? w->gen : 0;
}

/* Commit iff the store on disk is still at `expected_gen`.
 *
 * This is the whole multi-writer story: writers work independently, then
 * commit against the generation they started from. A writer whose base has
 * moved is REJECTED (GN_CONFLICT) and must reload and redo -- there is no
 * merge, because merging two divergent edit histories needs semantics this
 * layer does not have and should not invent.
 *
 * Returns 0 on success, GN_CONFLICT if another writer got there first,
 * GN_LOCKED if the lock could not be taken, -1 on I/O failure.            */
int gn_commit(gn_engine *e, const char *path, uint64_t expected_gen) {
    if (!e || !path) { errno = EINVAL; return -1; }
    char *lock = cat2(path, ".lock");
    if (!lock) { errno = ENOMEM; return -1; }

    /* Bounded retry: the critical section is a stat + a rename, so a holder
     * that is alive clears quickly. A holder that is dead does not, which is
     * what gn_lock_break is for. */
    int got = -1;
    for (int i = 0; i < 200; i++) {
        if (lock_acquire(lock) == 0) { got = 0; break; }
#if defined(_WIN32)
        Sleep(5);
#else
        usleep(5000);
#endif
    }
    if (got != 0) { free(lock); return GN_LOCKED; }

    uint64_t on_disk = gn_store_generation(path);
    if (on_disk != expected_gen) {
        lock_release(lock); free(lock);
        return GN_CONFLICT;
    }
    int rc = gn_save(e, path);
    lock_release(lock);
    free(lock);
    return rc;
}

/* Remove a lock left behind by a killed writer. Deliberately manual: silently
 * stealing a lock on a timeout is how two writers end up believing they hold
 * it at once. */
int gn_lock_break(const char *path) {
    char *lock = cat2(path, ".lock");
    if (!lock) return -1;
    int rc = remove(lock);
    free(lock);
    return rc;
}

int gn_lock_held(const char *path) {
    char *lock = cat2(path, ".lock");
    if (!lock) return 0;
    FILE *f = fopen(lock, "rb");
    int held = (f != NULL);
    if (f) fclose(f);
    free(lock);
    return held;
}

void gn_close(gn_engine *e) { gn_engine_free(e); }

/* Called by gn_engine_free so the handle is never leaked.                  */
void gn_wal__destroy(void *w) { wal_free((gn_wal*)w); }

int gn_wal_active(const gn_engine *e) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL;
    return (w && w->f) ? 1 : 0;
}
int gn_wal_ok(const gn_engine *e) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL;
    if (!w || !w->f) return 0;
    return w->broken ? 0 : 1;
}
int gn_wal_set_sync(gn_engine *e, int on) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL;
    if (!w) return -1;
    int prev = w->sync; w->sync = on ? 1 : 0;
    if (!on && w->f) fflush(w->f);
    return prev;
}
uint64_t gn_wal_records(const gn_engine *e) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL; return w ? w->records : 0;
}
uint64_t gn_wal_bytes(const gn_engine *e) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL; return w ? w->wbytes : 0;
}
uint64_t gn_wal_replayed(const gn_engine *e) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL; return w ? w->replayed : 0;
}
const char *gn_store_path(const gn_engine *e) {
    gn_wal *w = e ? (gn_wal*)gn_engine__wal(e) : NULL; return w ? w->path : NULL;
}
