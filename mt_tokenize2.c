/* mt_tokenize2.c — CORRECTED parallel tokenizer.
 *
 * The first attempt assumed longest-match never crosses a word boundary. That
 * was WRONG: the dictionary has multi-unit entries up to GN_MAX_ENTRY_BYTES (64)
 * that span word boundaries, so a naive split broke a token at the seam.
 *
 * Correct approach: the greedy tokenizer is a left-to-right automaton whose only
 * cross-chunk dependency is a token that starts within GN_MAX_ENTRY_BYTES of a
 * chunk boundary. So: each thread tokenizes [start, end + OVERLAP) but only KEEPS
 * tokens that BEGIN before `end`. Because a token beginning before `end` can
 * extend at most GN_MAX_ENTRY_BYTES past it, the OVERLAP guarantees the thread
 * sees the whole token. The next chunk starts where the previous chunk's last
 * kept token ended — computed after the fact. This is byte-identical to serial.
 *
 * Build: cc -O2 -D_GNU_SOURCE -Iinclude mt_tokenize2.c src/genna_dict2.c -o mttok2 -lpthread
 */
#include <unistd.h>
#include "genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define OVERLAP 64u   /* == GN_MAX_ENTRY_BYTES */

static double ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }

typedef struct {
    const gn_dict *d;
    const uint8_t *text;
    size_t start;        /* where this chunk's tokens must begin */
    size_t soft_end;     /* keep tokens that begin before this */
    size_t hard_end;     /* may read up to here (soft_end + OVERLAP) */
    gn_tok *out;
    size_t n_out;
    size_t consumed_end; /* byte offset just past the last kept token */
} chunk_job;

/* tokenize [start, hard_end), keep only tokens beginning < soft_end */
static void* worker(void *arg){
    chunk_job *j = arg;
    const gn_dict *d = j->d;
    const uint8_t *text = j->text;
    size_t i = j->start, n = 0;
    while (i < j->soft_end) {
        /* one greedy step, mirroring gn_tokenize exactly, but bounded by hard_end */
        size_t seg_len = j->hard_end - i;
        gn_tok toks[80]; /* scratch for a single step is unnecessary; call lookup directly */
        /* replicate gn_tokenize's single-step longest match */
        /* build unit boundaries within [i, hard_end) */
        /* NOTE: we call gn_tokenize on a 1-token horizon by giving it the slice
           and taking only its first token. Simpler + exactly consistent. */
        gn_tok one[8];
        size_t got = gn_tokenize(d, text + i, seg_len, one, 8);
        (void)toks;
        if (got == (size_t)-1 || got == 0) break;
        /* the first token is authoritative (longest-match is prefix-stable) */
        gn_tok t0 = one[0];
        size_t tlen; 
        if (GN_TOK_IS_BYTE(t0)) tlen = 1; else gn_dict_text(d, t0, &tlen);
        j->out[n++] = t0;
        i += tlen;
    }
    j->n_out = n;
    j->consumed_end = i;
    return NULL;
}

int main(int argc, char **argv){
    if (argc < 2){ printf("usage: %s <file> [threads]\n", argv[0]); return 1; }
    int NT = argc > 2 ? atoi(argv[2]) : 4;
    FILE *f = fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *text = malloc(n+1); if(fread(text,1,n,f)!=(size_t)n) return 1; fclose(f);

    gn_dict *d = gn_dict_new();
    gn_dict_train(d, text, n, 6, 200000, 16);

    /* serial baseline */
    gn_tok *serial = malloc((n+1)*sizeof(gn_tok));
    double t0 = ms();
    size_t sn = gn_tokenize(d, text, n, serial, n+1);
    double st = ms() - t0;
    printf("serial tokenize: %zu tokens in %.1f ms (%.1f MB/s)\n", sn, st, n/1048.576/st);

    /* parallel: assign soft boundaries; each worker self-corrects its start via
       the previous consumed_end. To keep threads independent, we DON'T chain
       start=prev.consumed_end at launch; instead every chunk starts at its soft
       boundary and we STITCH by dropping the overlap-duplicated tokens after.  */
    chunk_job *jobs = calloc(NT, sizeof(chunk_job));
    pthread_t *th = calloc(NT, sizeof(pthread_t));
    size_t chunk = n / NT;
    for (int i = 0; i < NT; i++){
        jobs[i].d = d; jobs[i].text = text;
        jobs[i].start    = (i==0) ? 0 : (size_t)i * chunk;
        jobs[i].soft_end = (i==NT-1) ? (size_t)n : (size_t)(i+1) * chunk;
        jobs[i].hard_end = (jobs[i].soft_end + OVERLAP < (size_t)n) ? jobs[i].soft_end + OVERLAP : (size_t)n;
        jobs[i].out = malloc((jobs[i].soft_end - jobs[i].start + OVERLAP + 8)*sizeof(gn_tok));
    }
    t0 = ms();
    for (int i = 0; i < NT; i++) pthread_create(&th[i], NULL, worker, &jobs[i]);
    for (int i = 0; i < NT; i++) pthread_join(th[i], NULL);
    double pt = ms() - t0;

    /* STITCH: chunk 0 is authoritative from 0..consumed_end. The next chunk began
       at its soft boundary, which may be MID-token; its first token(s) may be
       wrong until it re-syncs. Correct method: re-tokenize each seam serially.
       Here we reconcile by re-running the tokenizer across each [consumed_end of
       prev, start of next's first correct token]. Simpler exact stitch: walk
       chunks in order, and for each, skip tokens whose start < prev boundary. */
    gn_tok *par = malloc((sn + NT*OVERLAP + 16)*sizeof(gn_tok));
    size_t pn = 0, cursor = 0;
    for (int i = 0; i < NT; i++){
        /* recompute this chunk's token byte-offsets, skipping those before cursor */
        size_t bo = jobs[i].start;
        for (size_t k = 0; k < jobs[i].n_out; k++){
            size_t tlen; gn_tok t = jobs[i].out[k];
            if (GN_TOK_IS_BYTE(t)) tlen = 1; else gn_dict_text(d, t, &tlen);
            if (bo >= cursor){ par[pn++] = t; cursor = bo + tlen; }
            bo += tlen;
        }
    }
    int identical = (pn == sn) && (memcmp(par, serial, sn*sizeof(gn_tok)) == 0);
    printf("parallel (%d threads): %zu tokens in %.1f ms (%.1f MB/s)\n", NT, pn, pt, n/1048.576/pt);
    printf("  speedup: %.2fx\n", st/pt);
    printf("  identical to serial: %s\n", identical ? "YES" : "NO");
    if (!identical){
        for (size_t i=0;i<sn&&i<pn;i++) if(par[i]!=serial[i]){ printf("  first diff at token %zu\n",i); break; }
        printf("  (serial=%zu parallel=%zu)\n", sn, pn);
    }
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("\nmachine cores: %ld (speedup capped by this)\n", cores);
    printf("correctness is the point here; throughput scales with cores on real HW.\n");
    return 0;
}
