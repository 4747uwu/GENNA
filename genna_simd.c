/* genna_simd.c — SIMD-accelerated primitives, benchmarked against the scalar
 * paths. The technique for search is the standard one: use AVX2 to scan 32
 * bytes at a time for the needle's first byte, and only run a full compare at
 * positions where the first byte matched. This is what glibc memmem does and
 * is why the scalar per-offset loop lost 3x.
 *
 * Build test:
 *   cc -O2 -mavx2 -D_GNU_SOURCE genna_simd.c -o simdtest && ./simdtest
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <immintrin.h>

static double ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }

/* ---- scalar reference: find all occurrences of needle in hay ---- */
static size_t scan_scalar(const uint8_t *hay, size_t n, const uint8_t *needle, size_t nl){
    size_t count = 0;
    if (nl == 0 || n < nl) return 0;
    for (size_t i = 0; i + nl <= n; i++){
        if (memcmp(hay + i, needle, nl) == 0) count++;
    }
    return count;
}

/* ---- SIMD: AVX2 first-byte filter, then verify ---- */
static size_t scan_simd(const uint8_t *hay, size_t n, const uint8_t *needle, size_t nl){
    size_t count = 0;
    if (nl == 0 || n < nl) return 0;
    const __m256i first = _mm256_set1_epi8((char)needle[0]);
    const __m256i last  = _mm256_set1_epi8((char)needle[nl-1]);
    size_t i = 0;
    /* scan blocks of 32; for each, find positions where byte[i]==needle[0]
       AND byte[i+nl-1]==needle[nl-1]. This two-anchor filter (first+last byte)
       cuts false positives hard before the memcmp. */
    size_t limit = (n >= nl + 31) ? n - nl - 31 : 0;
    for (; i < limit; i += 32){
        __m256i blk_first = _mm256_loadu_si256((const __m256i*)(hay + i));
        __m256i blk_last  = _mm256_loadu_si256((const __m256i*)(hay + i + nl - 1));
        __m256i eqf = _mm256_cmpeq_epi8(blk_first, first);
        __m256i eql = _mm256_cmpeq_epi8(blk_last,  last);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_and_si256(eqf, eql));
        while (mask){
            uint32_t bit = __builtin_ctz(mask);
            /* verify the middle (first & last already match) */
            if (nl <= 2 || memcmp(hay + i + bit + 1, needle + 1, nl - 2) == 0)
                count++;
            mask &= mask - 1;
        }
    }
    /* tail */
    for (; i + nl <= n; i++){
        if (memcmp(hay + i, needle, nl) == 0) count++;
    }
    return count;
}

int main(void){
    /* build a realistic haystack: repeating structured text with the needle
       sprinkled in, ~16 MB (like a real corpus scan) */
    size_t N = 16u * 1024 * 1024;
    uint8_t *hay = malloc(N);
    const char *pat = "the quick brown fox jumps over the lazy dog and runs away swiftly ";
    size_t pl = strlen(pat);
    for (size_t i = 0; i < N; i++) hay[i] = pat[i % pl];

    struct { const char *name; const char *s; } needles[] = {
        {"short (4B)",  "fox "},
        {"medium (12B)","brown fox ju"},
        {"long (32B)",  "quick brown fox jumps over the l"},
        {"rare (miss)", "ZZQXJKWVNONEXISTENTNEEDLE12345678"},
    };

    printf("Substring scan: SIMD (AVX2 two-anchor) vs scalar, 16 MB haystack\n");
    printf("%-14s %-12s %-12s %-10s %s\n","needle","scalar ms","simd ms","speedup","counts match");
    for (int k = 0; k < 4; k++){
        const uint8_t *nd = (const uint8_t*)needles[k].s;
        size_t nl = strlen(needles[k].s);
        /* warm + time scalar */
        volatile size_t c1=0,c2=0;
        double t0=ms(); for(int r=0;r<3;r++) c1=scan_scalar(hay,N,nd,nl); double ts=(ms()-t0)/3;
        t0=ms(); for(int r=0;r<3;r++) c2=scan_simd(hay,N,nd,nl); double td=(ms()-t0)/3;
        printf("%-14s %-12.2f %-12.2f %-9.2fx %s (%zu)\n",
            needles[k].name, ts, td, ts/td, c1==c2?"YES":"NO", (size_t)c1);
    }
    /* compare against glibc memmem as the real bar */
    printf("\nvs glibc memmem (the real opponent):\n");
    const uint8_t *nd=(const uint8_t*)"brown fox ju"; size_t nl=12;
    double t0=ms(); size_t mc=0; for(int r=0;r<3;r++){ mc=0; const uint8_t*p=hay,*e=hay+N; while((p=memmem(p,e-p,nd,nl))){ mc++; p++; } } double tm=(ms()-t0)/3;
    t0=ms(); size_t sc=0; for(int r=0;r<3;r++) sc=scan_simd(hay,N,nd,nl); double td2=(ms()-t0)/3;
    printf("  memmem: %.2f ms | genna-simd: %.2f ms | %s\n", tm, td2, mc==sc?"counts match":"MISMATCH");
    free(hay);
    return 0;
}
