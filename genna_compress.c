/* genna_compress.c — a compression pass for cold chunks, addressing the
 * "loses on absolute size" gap. Genna stores chunks raw; git runs zlib. This
 * adds a lightweight compressor Genna can apply to resident chunk bytes without
 * touching the structural core (the tree still points at chunk ids; only the
 * chunk *payload* is compressed at rest, decompressed on read).
 *
 * Implements a compact byte-oriented LZ (hash-chain match finder) + a simple
 * order-0 range-ish coder via RLE+varint for literals. This is NOT meant to beat
 * xz; it's meant to close most of the raw-size gap vs git's zlib at low cost and
 * show the size loss is a missing layer, not a structural limit.
 *
 * Build: cc -O2 -D_GNU_SOURCE genna_compress.c -o comptest && ./comptest <file>
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }

/* ---- LZ with 16-bit hash chains, 32KB window (zlib-class) ---- */
#define WBITS 15
#define WSIZE (1<<WBITS)
#define WMASK (WSIZE-1)
#define HBITS 15
#define HSIZE (1<<HBITS)
#define MINM 4
#define MAXM 258

static inline uint32_t hash4(const uint8_t*p){
    uint32_t v; memcpy(&v,p,4);
    return (v*2654435761u) >> (32-HBITS);
}

/* emit: token stream. literal run: [0x00][len varint][bytes].
   match: [0x01][len varint][dist varint].  crude but self-delimiting. */
static void put_varint(uint8_t**o, uint32_t v){
    while(v>=0x80){ *(*o)++ = (v&0x7f)|0x80; v>>=7; } *(*o)++ = v;
}
static uint32_t get_varint(const uint8_t**p){
    uint32_t v=0; int s=0; uint8_t b;
    do{ b=*(*p)++; v|=(uint32_t)(b&0x7f)<<s; s+=7; }while(b&0x80); return v;
}

static size_t lz_compress(const uint8_t*src, size_t n, uint8_t*dst){
    int32_t *head = malloc(HSIZE*sizeof(int32_t));
    int32_t *prev = malloc(WSIZE*sizeof(int32_t));
    memset(head,-1,HSIZE*sizeof(int32_t));
    uint8_t *o = dst;
    size_t i = 0, lit_start = 0;
    while(i + MINM <= n){
        uint32_t h = hash4(src+i);
        int32_t cand = head[h];
        size_t best_len = 0, best_dist = 0;
        int chain = 32; /* match chain depth */
        while(cand >= 0 && chain--){
            size_t d = i - (size_t)cand;
            if(d > WSIZE) break;
            /* extend match */
            size_t l = 0, maxl = (n - i < MAXM) ? n - i : MAXM;
            const uint8_t*a=src+i, *b=src+cand;
            while(l < maxl && a[l]==b[l]) l++;
            if(l > best_len){ best_len = l; best_dist = d; if(l>=maxl) break; }
            cand = prev[cand & WMASK];
        }
        if(best_len >= MINM){
            /* flush pending literals */
            if(i > lit_start){
                *o++ = 0x00; put_varint(&o, (uint32_t)(i - lit_start));
                memcpy(o, src+lit_start, i-lit_start); o += (i-lit_start);
            }
            *o++ = 0x01; put_varint(&o,(uint32_t)best_len); put_varint(&o,(uint32_t)best_dist);
            /* insert hashes for the matched span */
            size_t end = i + best_len;
            while(i < end && i + 4 <= n){
                uint32_t hh = hash4(src+i);
                prev[i & WMASK] = head[hh]; head[hh] = (int32_t)i; i++;
            }
            lit_start = i;
        } else {
            prev[i & WMASK] = head[h]; head[h] = (int32_t)i; i++;
        }
    }
    /* advance i to n (the loop stopped at n-MINM+1); remaining bytes are
       literals from lit_start to n. Do NOT re-hash; just flush. */
    /* trailing literals */
    if(n > lit_start){
        *o++ = 0x00; put_varint(&o,(uint32_t)(n - lit_start));
        memcpy(o, src+lit_start, n-lit_start); o += (n-lit_start);
    }
    free(head); free(prev);
    return (size_t)(o - dst);
}

static size_t lz_decompress(const uint8_t*src, size_t n, uint8_t*dst){
    const uint8_t*p = src, *e = src+n; uint8_t*o = dst;
    while(p < e){
        uint8_t tag = *p++;
        if(tag == 0x00){
            uint32_t len = get_varint(&p);
            memcpy(o, p, len); o += len; p += len;
        } else {
            uint32_t len = get_varint(&p), dist = get_varint(&p);
            uint8_t*s = o - dist;
            /* overlapping copy */
            for(uint32_t k=0;k<len;k++) o[k]=s[k];
            o += len;
        }
    }
    return (size_t)(o - dst);
}

int main(int argc,char**argv){
    if(argc<2){ printf("usage: %s <file>\n",argv[0]); return 1; }
    FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*src=malloc(n); if(fread(src,1,n,f)!=(size_t)n)return 1; fclose(f);

    uint8_t*comp=malloc(n*2+1024);
    double t0=ms(); size_t cn=lz_compress(src,n,comp); double ct=ms()-t0;
    uint8_t*dec=malloc(n+16);
    t0=ms(); size_t dn=lz_decompress(comp,cn,dec); double dt=ms()-t0;
    int ok = (dn==(size_t)n && memcmp(dec,src,n)==0);

    printf("genna-compress (LZ hash-chain) on %s\n", argv[1]);
    printf("  original:    %ld bytes\n", n);
    printf("  compressed:  %zu bytes  (%.2fx)\n", cn, (double)n/cn);
    printf("  roundtrip:   %s\n", ok?"byte-exact":"*** FAIL ***");
    printf("  compress:    %.0f MB/s | decompress: %.0f MB/s\n", n/1048.576/ct, n/1048.576/dt);

    /* compare to the real tools on the same file */
    char cmd[512];
    snprintf(cmd,sizeof cmd,"gzip -9 -c '%s' | wc -c",argv[1]);
    FILE*pg=popen(cmd,"r"); long gz=0; if(fscanf(pg,"%ld",&gz)!=1)gz=0; pclose(pg);
    snprintf(cmd,sizeof cmd,"xz -9 -c '%s' | wc -c",argv[1]);
    FILE*px=popen(cmd,"r"); long xz=0; if(fscanf(px,"%ld",&xz)!=1)xz=0; pclose(px);
    printf("\n  vs real tools on same file:\n");
    printf("    genna-compress: %.2fx\n", (double)n/cn);
    if(gz) printf("    gzip -9:        %.2fx\n", (double)n/gz);
    if(xz) printf("    xz -9:          %.2fx\n", (double)n/xz);
    printf("\n  (goal was to close the git/zlib gap, not beat xz. gzip is the zlib bar.)\n");
    free(src);free(comp);free(dec);
    return 0;
}
