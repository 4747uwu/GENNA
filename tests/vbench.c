/* vbench.c — a realistic NLE timeline workload on the video engine.
 *
 * 1000 operations of the kind an editor actually performs: ripple cuts,
 * copy/paste of a range, duplicate a segment, undo (read an old version).
 * Every operation is frame-aligned; every result is byte-exact readable.
 *
 * The comparison point is ffmpeg -c copy, which is what a file-based tool
 * must do for the same edit: demux, copy bytes, remux into a new file.
 */
#include "../include/genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int gn_vdict_is_idr(const gn_dict*, gn_tok);

/* Enumerate the IDR byte offsets of a version's CURRENT token stream.
 * Cut points move after every edit, so they must be re-derived, not reused
 * from the source file. O(frames) -- a few thousand, microseconds. */
typedef struct { gn_engine *e; gn_dict *d; uint64_t *out; size_t n; uint64_t pos; } idrctx;
static void idr_leaf(void *ctx, const gn_extent *x, uint64_t xb) {
    (void)xb; idrctx *C = (idrctx*)ctx;
    size_t cn; const gn_tok *ct = gn_store_get(gn_engine_store(C->e), x->chunk, &cn);
    for (uint32_t k = 0; k < x->len; k++) {
        size_t l; gn_dict_text(C->d, ct[x->off+k], &l);
        if (gn_vdict_is_idr(C->d, ct[x->off+k])) C->out[C->n++] = C->pos;
        C->pos += l;
    }
}
static size_t idr_points(gn_engine *e, gn_dict *d, const gn_version *v, uint64_t *out) {
    idrctx C = { e, d, out, 0, 0 };
    gn_ext_walk(v->root, idr_leaf, &C);
    return C.n;
}
static double ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec*1e3+t.tv_nsec/1e6;}
/* resident set in KB. /proc is Linux-only; Windows keeps the same number in
 * the process memory counters. Previously this dereferenced a NULL FILE* on
 * any platform without /proc. */
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
static long rss(void){
    PROCESS_MEMORY_COUNTERS pmc;
    if(GetProcessMemoryInfo(GetCurrentProcess(),&pmc,sizeof pmc))
        return (long)(pmc.WorkingSetSize/1024);
    return 0;
}
#else
static long rss(void){FILE*f=fopen("/proc/self/status","r");char l[256];long k=0;
    if(!f) return 0;
    while(fgets(l,256,f))if(!strncmp(l,"VmRSS:",6))sscanf(l+6,"%ld",&k);fclose(f);return k;}
#endif
static uint8_t*slurp(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}
    fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(s+1);
    if(fread(b,1,s,f)!=(size_t)s)exit(1);fclose(f);*n=s;return b;}

int main(int argc, char **argv) {
    size_t n; uint8_t *B = slurp(argv[1], &n);
    int NOPS = argc > 2 ? atoi(argv[2]) : 1000;

    gn_engine *e = gn_engine_new();
    double t0 = ms();
    gn_object *o = gn_create(e, "timeline", B, n);
    double t_ing = ms() - t0;
    gn_dict *d = gn_engine_dict(e);

    printf("== ingest ==\n");
    printf("  %.1f MB -> %llu frames in %.0f ms (%.0f MB/s)\n",
        n/1048576.0, (unsigned long long)o->ver[0].total_tokens, t_ing,
        n/1048.576/t_ing);
    printf("  language %u unique frames | tree leaves %u depth %u | RSS %.0f MB\n",
        gn_dict_size(d), gn_ext_leaves(o->ver[0].root),
        gn_ext_depth(o->ver[0].root), rss()/1024.0);

    /* IDR byte offsets = legal cut points */
    gn_tok *tk = malloc((n+1)*sizeof(gn_tok));
    size_t nt = gn_tokenize(d, B, n, tk, n+1);
    uint64_t *idr = malloc(nt*8); size_t nidr = 0; uint64_t pos = 0;
    for (size_t i = 0; i < nt; i++) { size_t l; gn_dict_text(d, tk[i], &l);
        if (gn_vdict_is_idr(d, tk[i])) idr[nidr++] = pos; pos += l; }
    printf("  %zu IDR cut points\n", nidr);

    /* a second object to paste from */
    gn_object *src = gn_create(e, "bin", B, n);

    printf("== %d timeline operations ==\n", NOPS);
    uint64_t s = 31337; int n_cut=0, n_paste=0, n_undo=0, fail=0;
    double t_cut=0, t_paste=0, t_undo=0;
    uint8_t *tmp = malloc(8u<<20);

    /* keep the timeline bounded, as a real edit session is: cuts and
       pastes roughly cancel. Without this the workload is just "grow". */
    uint64_t LO = (uint64_t)(n * 0.7), HI = (uint64_t)(n * 1.3);
    uint64_t *cur = malloc(((size_t)(HI / 4096) + 4096) * 8);
    double t_pts = 0;
    for (int i = 0; i < NOPS; i++) {
        gn_version *v = &o->ver[o->n_ver-1];
        double xp = ms();
        size_t ncur = idr_points(e, d, v, cur);   /* re-derive cut points */
        t_pts += ms() - xp;
        s = s*6364136223846793005ULL + 1442695040888963407ULL;
        int op = (s>>33) % 3;
        if (v->total_bytes > HI) op = 0;          /* too long -> must cut  */
        if (v->total_bytes < LO && op == 0) op = 1;

        if (op == 0 && ncur > 8) {                            /* ripple cut */
            s = s*6364136223846793005ULL + 1442695040888963407ULL;
            size_t a = (s>>33)%(ncur-4), b = a + 1 + ((s>>17)%4);
            if (b >= ncur) b = ncur-1;
            double x = ms();
            if (gn_cut(e, o, cur[a], cur[b]-cur[a]) == 0) { t_cut += ms()-x; n_cut++; }
            else fail++;
        } else if (op == 1 && ncur > 2) {                        /* copy/paste */
            s = s*6364136223846793005ULL + 1442695040888963407ULL;
            size_t a = (s>>33)%nidr, b = a + 1 + ((s>>17)%16);
            if (b >= nidr) b = nidr-1;
            size_t at = (s>>41)%ncur;
            double x = ms();
            if (gn_graft(e, o, cur[at], src, idr[a], idr[b]-idr[a]) == 0)
                { t_paste += ms()-x; n_paste++; }
            else fail++;
        } else {                                              /* undo/scrub */
            s = s*6364136223846793005ULL + 1442695040888963407ULL;
            uint32_t ver = (s>>33) % o->n_ver;
            uint64_t off = o->ver[ver].total_bytes > (4u<<20)
                         ? (s>>13) % (o->ver[ver].total_bytes-(4u<<20)) : 0;
            double x = ms();
            gn_read_version(e, o, ver, off, 4u<<20, tmp);
            t_undo += ms()-x; n_undo++;
        }
    }

    printf("  ripple cut   x%-5d mean %.4f ms\n", n_cut,   n_cut?t_cut/n_cut:0);
    printf("  copy/paste   x%-5d mean %.4f ms   (clip length irrelevant)\n",
        n_paste, n_paste?t_paste/n_paste:0);
    printf("  scrub 4MB    x%-5d mean %.4f ms   (random version, random offset)\n",
        n_undo, n_undo?t_undo/n_undo:0);
    printf("  rejected (not frame-aligned): %d\n", fail);
    printf("  [cut-point re-derivation, not part of the edit: %.4f ms/op]\n",
        t_pts/NOPS);

    gn_version *cv = &o->ver[o->n_ver-1];
    printf("== final state ==\n");
    printf("  timeline %.1f MB, %llu frames, %u versions\n",
        cv->total_bytes/1048576.0, (unsigned long long)cv->total_tokens, o->n_ver);
    printf("  tree leaves %u depth %u | %llu nodes = %.1f MB\n",
        gn_ext_leaves(cv->root), gn_ext_depth(cv->root),
        (unsigned long long)gn_ext_nodes_alloced(), gn_ext_node_bytes()/1048576.0);
    printf("  RSS %.0f MB for a %.1f MB timeline built from %.1f MB of source\n",
        rss()/1024.0, cv->total_bytes/1048576.0, 2*n/1048576.0);

    /* write the result out so ffmpeg can judge it */
    size_t ocap = (cv->total_bytes > n ? cv->total_bytes : n) + 64;
    uint8_t *out = malloc(ocap);
    t0 = ms();
    size_t r = gn_read(e, o, 0, cv->total_bytes, out);
    double t_mat = ms() - t0;
    printf("  materialize whole timeline: %.0f ms (%.0f MB/s)\n",
        t_mat, r/1048.576/t_mat);
    FILE *f = fopen(argv[3] ? argv[3] : "vid/timeline.264", "wb");
    fwrite(out, 1, r, f); fclose(f);

    /* v0 must still be byte-exact */
    size_t v0 = gn_read_version(e, o, 0, 0, n+16, out);
    printf("  v0 after %d ops: %s\n", NOPS,
        (v0==n && !memcmp(out,B,n)) ? "BYTE-EXACT" : "*** CORRUPT ***");
    return 0;
}
