/* genna_smart.c — frame-accurate video cuts on top of the engine.
 *
 * The limit found in the first video probe: splices are only legal at IDR
 * frames, so a 3600-frame file had 120 cut points and cuts snapped to
 * one-second boundaries. No editor accepts that.
 *
 * The fix is what NLEs call smart render. To cut at an arbitrary frame N:
 *
 *   [ ... whole GOPs ... ][ GOP containing N ][ ... whole GOPs ... ]
 *                          ^ only this one is decoded and re-encoded
 *
 * Everything outside the boundary GOP is grafted untouched -- no decode, no
 * re-encode, no quality loss. Only the partial GOP is rebuilt, as a new
 * closed GOP starting exactly at N. So the cost of frame accuracy is
 * bounded by the GOP length, not the clip length: re-encode 30 frames
 * instead of 3600, regardless of how long the timeline is.
 *
 * The re-encode shells out to ffmpeg. That is deliberate -- the engine
 * stays codec-agnostic and this file is the only place that knows H.264
 * exists as anything other than opaque frames.
 */
#include "../include/genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int gn_vdict_is_idr(const gn_dict *d, gn_tok t);

/* Frame index -> byte offset, and the IDR that governs it. */
typedef struct {
    uint64_t byte_off;      /* byte offset of this frame                  */
    uint64_t gop_start;     /* byte offset of the IDR opening its GOP     */
    uint64_t gop_end;       /* byte offset just past the GOP              */
    uint32_t gop_first;     /* frame index of that IDR                    */
    uint32_t gop_count;     /* frames in the GOP                          */
    int      is_idr;
} gn_frame_info;

typedef struct {
    gn_engine *e; gn_dict *d;
    uint64_t *fb; uint8_t *fidr; uint32_t n; uint32_t cap;
    uint64_t pos;
} scanctx;

static void scan_leaf(void *ctx, const gn_extent *x, uint64_t xb) {
    (void)xb; scanctx *C = (scanctx*)ctx;
    size_t cn; const gn_tok *ct = gn_store_get(gn_engine_store(C->e), x->chunk, &cn);
    for (uint32_t k = 0; k < x->len; k++) {
        if (C->n == C->cap) {
            C->cap = C->cap ? C->cap * 2 : 4096;
            C->fb = realloc(C->fb, (size_t)C->cap * 8);
            C->fidr = realloc(C->fidr, C->cap);
        }
        size_t l; gn_dict_text(C->d, ct[x->off + k], &l);
        C->fb[C->n] = C->pos;
        C->fidr[C->n] = (uint8_t)gn_vdict_is_idr(C->d, ct[x->off + k]);
        C->n++; C->pos += l;
    }
}

/* Build the frame table for an object's current version. O(frames). */
uint32_t gn_frame_table(gn_engine *e, gn_object *o,
                        uint64_t **byte_off, uint8_t **is_idr) {
    gn_version *v = &o->ver[o->n_ver - 1];
    scanctx C = { e, gn_engine_dict(e), NULL, NULL, 0, 0, 0 };
    gn_ext_walk(v->root, scan_leaf, &C);
    *byte_off = C.fb; *is_idr = C.fidr;
    return C.n;
}

/* Locate the GOP containing frame `f`. */
static gn_frame_info frame_info(uint64_t *fb, uint8_t *idr, uint32_t nf,
                                uint64_t total_bytes, uint32_t f) {
    gn_frame_info fi = {0};
    fi.byte_off = fb[f];
    fi.is_idr = idr[f];
    uint32_t s = f; while (s > 0 && !idr[s]) s--;
    uint32_t t = f + 1; while (t < nf && !idr[t]) t++;
    fi.gop_first = s;
    fi.gop_start = fb[s];
    fi.gop_end   = (t < nf) ? fb[t] : total_bytes;
    fi.gop_count = t - s;
    return fi;
}

/* Re-encode bytes [gop_start, gop_end) keeping only frames from
 * `skip_frames` onward, emitting a closed GOP. Returns malloc'd Annex-B. */
static uint8_t *reencode_tail(const uint8_t *gop, size_t gop_len,
                              uint32_t skip_frames, size_t *out_len,
                              const char *tmpdir) {
    char fin[256], fout[256], cmd[1024];
    snprintf(fin,  sizeof fin,  "%s/gn_seam_in_%d.264",  tmpdir, (int)getpid());
    snprintf(fout, sizeof fout, "%s/gn_seam_out_%d.264", tmpdir, (int)getpid());
    FILE *f = fopen(fin, "wb");
    if (!f) return NULL;
    fwrite(gop, 1, gop_len, f); fclose(f);

    /* select frames >= skip, force every output frame to be an IDR so the
     * result is a self-contained closed GOP that can be grafted anywhere  */
    snprintf(cmd, sizeof cmd,
        "ffmpeg -v error -y -i %s -vf \"select='gte(n\\,%u)'\" -vsync 0 "
        "-c:v libx264 -preset ultrafast -g 1 -bf 0 -pix_fmt yuv420p "
        "-f h264 %s 2>/dev/null",
        fin, skip_frames, fout);
    int rc = system(cmd);
    unlink(fin);
    if (rc != 0) { unlink(fout); return NULL; }

    f = fopen(fout, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fclose(f); free(buf); unlink(fout); return NULL; }
    fclose(f); unlink(fout);
    *out_len = (size_t)sz;
    return buf;
}

/* ---- the public operation -------------------------------------------
 * Delete frames [from, to) from `o`, exactly, whatever their type.
 * Cost: O(log n) tree work + at most ONE GOP re-encoded at each seam.
 * Returns 0 on success, -1 on failure. `reencoded_frames` reports how many
 * frames actually had to be rebuilt (the honest cost of frame accuracy).
 */
int gn_cut_frames(gn_engine *e, gn_object *o,
                  uint32_t from, uint32_t to,
                  uint32_t *reencoded_frames, const char *tmpdir) {
    if (to <= from) return -1;
    if (!tmpdir) tmpdir = "/tmp";
    uint64_t *fb; uint8_t *idr;
    uint32_t nf = gn_frame_table(e, o, &fb, &idr);
    if (to > nf) { free(fb); free(idr); return -1; }
    uint64_t total = o->ver[o->n_ver - 1].total_bytes;
    if (reencoded_frames) *reencoded_frames = 0;

    /* The tail frame `to` must become the start of a valid GOP. If it is
     * already an IDR the cut is free -- pure tree surgery. If it is not,
     * rebuild only its GOP's tail as a closed GOP. */
    gn_frame_info ti = frame_info(fb, idr, nf, total, to);

    if (to == nf || ti.is_idr) {
        int rc = gn_cut(e, o, fb[from], (to < nf ? fb[to] : total) - fb[from]);
        free(fb); free(idr);
        return rc;
    }

    /* materialize just the boundary GOP */
    size_t gop_len = (size_t)(ti.gop_end - ti.gop_start);
    uint8_t *gop = malloc(gop_len + 64);
    gn_read(e, o, ti.gop_start, gop_len, gop);

    size_t new_len = 0;
    uint8_t *fresh = reencode_tail(gop, gop_len, to - ti.gop_first,
                                   &new_len, tmpdir);
    free(gop);
    if (!fresh) { free(fb); free(idr); return -1; }
    if (reencoded_frames) *reencoded_frames = ti.gop_count - (to - ti.gop_first);

    /* replace [from, gop_end) with the rebuilt tail: one splice, and the
     * only bytes the engine has never seen are the re-encoded frames. */
    int rc = gn_update(e, o, fb[from], ti.gop_end - fb[from], fresh, new_len);
    free(fresh); free(fb); free(idr);
    return rc;
}
