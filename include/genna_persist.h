/* genna_persist.h — Stage 2 / Feature 1: on-disk persistence.
 *
 * Genna's engine is RAM-only: every version, chunk and edit dies with the
 * process. This layer adds two things and nothing else:
 *
 *   1. A snapshot format.  gn_save() serializes the whole engine — the
 *      content-addressed chunk store, the dictionary, and every object's
 *      full version DAG — into one file. gn_open() reads it back ready to
 *      use. The version trees are written as a DAG, not as N independent
 *      trees, so the structural sharing that makes versions cheap in RAM is
 *      also what makes them cheap on disk.
 *
 *   2. A write-ahead log.  Once an engine is bound to a path (by gn_save or
 *      gn_open), every mutating verb appends a record to <path>.wal and
 *      fsyncs it BEFORE the edit is applied in memory. gn_open() replays
 *      whatever the last snapshot does not already contain. A process killed
 *      at any instant reopens to a consistent state, with every edit that
 *      returned to its caller still present.
 *
 * Layering: this is a layer over the engine, not a rewrite of it. The treap,
 * the O(log n) edits and the byte-exactness are untouched. The engine gains
 * exactly one field (a WAL handle) and one logging call at the head of each
 * mutator; everything else lives in genna_persist.c.
 *
 * See PERSISTENCE.md for the on-disk layout and the honest list of what is
 * NOT handled.
 */
#ifndef GENNA_PERSIST_H
#define GENNA_PERSIST_H

#include "genna.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- snapshot -------------------------------------------------------- */

/* Serialize the entire engine to `path`.
 *
 * Atomic: written to "<path>.tmp", fsynced, then renamed over `path`, so a
 * crash at any point leaves either the previous snapshot or the new one --
 * never a half-written file.
 *
 * Side effect: binds `e` to `path` and (re)starts a fresh WAL at
 * "<path>.wal", because the snapshot is now the checkpoint the log replays
 * on top of. Editing after a save therefore stays crash-safe.
 *
 * Returns 0 on success, -1 on error (errno set).                          */
int gn_save(gn_engine *e, const char *path);

/* ---- out-of-core stores ----------------------------------------------
 * gn_open() reads the whole store into RAM and copies every chunk out of it.
 * That caps a store at available memory, which for real datasets is the
 * binding constraint long before any compression ratio matters.
 *
 * GN_SAVE_MAPPABLE writes a store that can be memory-mapped instead: chunk
 * token arrays are 4-byte aligned and stored UNCOMPRESSED, so gn_open() can
 * point the chunk table straight into the mapping and copy nothing. Pages
 * are faulted in by the OS only where the tree is actually read.
 *
 * The trade is explicit and unavoidable: you cannot point into compressed
 * bytes. A mappable store forfeits the whole-payload zstd pass (measured at
 * 3.85x on a text store), so it is bigger on disk in exchange for not being
 * bounded by RAM. Choose per store; the reader detects which it got.
 *
 * Zero-copy additionally requires a little-endian host, since the mapped
 * bytes ARE the gn_tok array. On a big-endian host the loader falls back to
 * copying and byte-swapping, and gn_store_is_mapped() reports 0.
 */
#define GN_SAVE_MAPPABLE 1u

/* Write the payload uncompressed, keeping the ordinary (non-split) layout.
 *
 * Exists because the corrupt-store fuzzer must be able to repair a store's
 * checksum after mutating it, and the CRC covers the UNCOMPRESSED payload
 * while the bytes on disk are compressed -- so in any build with zlib/zstd
 * every mutant was rejected at the checksum and the structural validators
 * were never reached. The campaign printed green while testing nothing. */
#define GN_SAVE_RAW      2u

int gn_save_ex(gn_engine *e, const char *path, uint32_t flags);

/* 1 if this engine's chunk data is borrowed from a mapping rather than
 * owned in the heap. */
int      gn_store_is_mapped(const gn_engine *e);
/* Bytes of chunk payload served from the mapping (i.e. NOT heap-allocated). */
uint64_t gn_store_mapped_bytes(const gn_engine *e);

/* Load a snapshot, replay any un-checkpointed WAL records, and return an
 * engine ready to use. The returned engine is bound to `path` with its WAL
 * enabled, so continuing to edit continues to be crash-safe.
 *
 * Returns NULL if `path` is missing or corrupt (errno set).               */
gn_engine *gn_open(const char *path);

/* Free an engine and release its WAL. gn_engine_free() does this too; this
 * name just reads better next to gn_open().                               */
void gn_close(gn_engine *e);

/* ---- write-ahead log ------------------------------------------------- */

/* 1 if `e` is bound to a store path with an active WAL.                   */
int gn_wal_active(const gn_engine *e);

/* 1 if the WAL is active AND every record so far was written and synced.
 * Goes to 0 permanently on the first failed write or fsync (a full disk,
 * a removed volume). The engine's mutators cannot fail an edit that is
 * already in progress, so this is how a caller learns that the durability
 * guarantee has lapsed and a fresh gn_save() is needed.                   */
int gn_wal_ok(const gn_engine *e);

/* Durability knob.
 *   1 (default) — fsync the log after every record. An edit that returned
 *                 to its caller survives SIGKILL and survives power loss.
 *   0           — write() only, no fsync. Survives SIGKILL (the bytes are
 *                 in the OS page cache, which outlives the process) but NOT
 *                 power loss. Much faster for bulk curation.
 * Returns the previous mode.                                              */
int gn_wal_set_sync(gn_engine *e, int on);

/* Counters, for tests and for honest reporting.                           */
uint64_t gn_wal_records(const gn_engine *e);   /* records appended, this session */
uint64_t gn_wal_bytes(const gn_engine *e);     /* bytes appended,   this session */
uint64_t gn_wal_replayed(const gn_engine *e);  /* records replayed at open       */

/* Path the engine is bound to, or NULL.                                   */
const char *gn_store_path(const gn_engine *e);

/* ---- optimistic concurrency ------------------------------------------
 * There is no merge here, and deliberately so: merging two divergent edit
 * histories needs semantics this layer does not have. What it provides is
 * detection -- a writer whose base moved is told, rather than silently
 * overwriting the other writer.
 *
 *   gen = gn_store_generation(path);   // token for what you loaded
 *   ... edit ...
 *   if (gn_commit(e, path, gen) == GN_CONFLICT) { reload; redo; }
 */
#define GN_CONFLICT (-2)   /* another writer committed first                */
#define GN_LOCKED   (-3)   /* could not take the store lock                 */

/* Generation of the snapshot on disk, without loading it. 0 if absent. */
uint64_t gn_store_generation(const char *path);

/* Generation this engine was loaded from / last saved at. */
uint64_t gn_engine_generation(const gn_engine *e);

/* Save iff the store is still at `expected_gen`. Takes an advisory lock for
 * the check-and-write so two writers cannot both pass the check. */
int gn_commit(gn_engine *e, const char *path, uint64_t expected_gen);

/* The lock is advisory and a killed writer leaves it behind. Breaking it is
 * manual on purpose: auto-stealing on a timeout is how two writers both come
 * to believe they hold it. */
int gn_lock_held(const char *path);
int gn_lock_break(const char *path);

/* ---- object enumeration ----------------------------------------------
 * After gn_open() the caller has an engine but no object handles yet, so it
 * needs a way to find what came back.                                     */
uint32_t   gn_engine_objects(const gn_engine *e);
gn_object *gn_engine_object(const gn_engine *e, uint32_t i);

/* ---- internal: called from the engine mutators ------------------------
 * These append the "I am about to do X" record and fsync it. They are no-ops
 * when no WAL is attached, and are suppressed during replay. Not for
 * application use.                                                        */
void gn_wal__create(gn_engine *e, const char *name,
                    const uint8_t *text, size_t len);
void gn_wal__update(gn_engine *e, const char *name, uint64_t off,
                    uint64_t del, const uint8_t *text, size_t len);
void gn_wal__delete(gn_engine *e, const char *name);
void gn_wal__graft (gn_engine *e, const char *dst, uint64_t dst_off,
                    const char *src, uint64_t src_off, uint64_t src_len);
void gn_wal__cut   (gn_engine *e, const char *name, uint64_t off, uint64_t len);
void gn_wal__trim  (gn_engine *e, const char *name, uint32_t keep);

/* ---- format constants ------------------------------------------------ */
#define GN_SNAP_MAGIC   "GENNAsnp"
#define GN_WAL_MAGIC    "GENNAwal"
/* v2 added the compressed-payload header fields; v3 added crc_len/bulk_crc so
 * a mappable store can verify its metadata without reading (and therefore
 * faulting in) its bulk. The layout changed each time, so an older store is
 * rejected rather than misread. */
#define GN_FORMAT_VER   3u

/* Verify the chunk bulk of a store on disk against the CRC in its header.
 * This is the check gn_open() deliberately does NOT do for a mappable store,
 * because performing it reads every byte and defeats the mapping. Call it
 * when you want the guarantee and can pay for a full pass.
 *
 * Returns 0 if intact, -1 on mismatch or I/O error (errno set), and 1 if the
 * store has no separate bulk (a normal store, already fully verified at open).
 */
int gn_verify_chunks(const char *path);

#ifdef __cplusplus
}
#endif
#endif /* GENNA_PERSIST_H */
