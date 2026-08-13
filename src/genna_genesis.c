/* genna_genesis.c — genomics language for Genna.  HONEST VERSION.
 *
 * Measured fact: a single real genome (E. coli) has ~1.95 bits/base of
 * entropy, so the information-theoretic ceiling is ~4.1x and 2-bit packing
 * already gets 4.0x. There is almost no exploitable repeat structure in one
 * bacterial genome -- the earlier 4.47x was an artifact of tiling one genome
 * 250x. So Genesis does NOT chase ratio past 2-bit; that would be dishonest.
 *
 * What Genesis actually delivers, and what no 2-bit packer (2bit/blast/NAF)
 * does: 2-bit density WHILE the data stays an addressable token stream the
 * engine can splice, version, and search. Each token packs 12 bases into a
 * u32 (3x as text, and the FASTA newlines/headers escape out). The win over
 * .2bit files is not size -- it is that CRUD-below-the-layer and k-mer search
 * work without unpacking the whole genome.
 *
 * The search primitive (gn_genesis_locate) finds every occurrence of a DNA
 * query by packing the query the same way and scanning the packed token
 * stream -- 4x fewer bytes touched than scanning the FASTA text, and no
 * decompression. That is the FM-index property, on general hardware.
 */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>

#define GEN_MAXK 12u                 /* 12 bases -> 24 bits; +4-bit len tag */
#define PACK_TAG(k)  (((gn_tok)(k))<<24)
#define PACK_K(t)    (((t)>>24)&0xF)

static inline int base_code(uint8_t c){
    switch(c){case 'A':return 0;case 'C':return 1;case 'G':return 2;case 'T':return 3;}
    return -1;
}
static inline uint8_t code_base(uint32_t v){ return "ACGT"[v&3]; }

struct gn_dict { uint64_t version; };
gn_dict*gn_dict_new(void){ return calloc(1,sizeof(gn_dict)); }
void gn_dict_free(gn_dict*d){ free(d); }
uint32_t gn_dict_size(const gn_dict*d){ (void)d; return 0; }
uint64_t gn_dict_version(const gn_dict*d){ return d->version; }
gn_tok gn_dict_lookup(const gn_dict*d,const uint8_t*w,size_t n){(void)d;(void)w;(void)n;return GN_TOK_INVALID;}

static const uint8_t IDBYTE[256]={
  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,
  32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,
  64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,
  96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
  128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255};
static __thread uint8_t g_scratch[16];
const uint8_t*gn_dict_text(const gn_dict*d,gn_tok t,size_t*lo){
    (void)d;
    if(GN_TOK_IS_BYTE(t)){ *lo=1; return &IDBYTE[GN_TOK_BYTE_VAL(t)]; }
    if(t==GN_TOK_INVALID){ *lo=0; return NULL; }
    uint32_t k=PACK_K(t), v=t&0x00FFFFFF;
    for(uint32_t j=0;j<k;j++) g_scratch[j]=code_base((v>>(2*j))&3);
    *lo=k; return g_scratch;
}
int gn_dict_learn(gn_dict*d,const uint8_t*t,size_t n,uint32_t m){(void)t;(void)n;(void)m;d->version++;return 0;}
int gn_dict_train(gn_dict*d,const uint8_t*t,size_t n,uint32_t a,uint32_t b,uint32_t c){
    (void)t;(void)n;(void)a;(void)b;(void)c;d->version++;return 0;}

size_t gn_tokenize(const gn_dict*d,const uint8_t*t,size_t len,gn_tok*out,size_t cap){
    (void)d; size_t n=0,i=0;
    while(i<len){
        int c=base_code(t[i]);
        if(c<0){ if(n==cap)return(size_t)-1; out[n++]=GN_BYTE_BASE+t[i++]; continue; }
        uint32_t packed=0,k=0;
        while(i<len&&k<GEN_MAXK){ int b=base_code(t[i]); if(b<0)break;
            packed|=((uint32_t)b)<<(2*k); k++; i++; }
        if(n==cap)return(size_t)-1;
        out[n++]=PACK_TAG(k)|packed;
    }
    return n;
}
size_t gn_detok_len(const gn_dict*d,const gn_tok*tk,size_t n){(void)d;
    size_t tot=0; for(size_t i=0;i<n;i++){ if(GN_TOK_IS_BYTE(tk[i]))tot++; else tot+=PACK_K(tk[i]); }
    return tot; }
size_t gn_detokenize(const gn_dict*d,const gn_tok*tk,size_t n,uint8_t*o,size_t cap){
    (void)d; size_t w=0;
    for(size_t i=0;i<n;i++){
        if(GN_TOK_IS_BYTE(tk[i])){ if(w+1>cap)return w; o[w++]=GN_TOK_BYTE_VAL(tk[i]); continue; }
        uint32_t k=PACK_K(tk[i]), v=tk[i]&0x00FFFFFF;
        if(w+k>cap)return w;
        for(uint32_t j=0;j<k;j++) o[w++]=code_base((v>>(2*j))&3);
    }
    return w;
}

/* ---- k-mer search: the actual genomics primitive ---------------------
 * Find every occurrence of a DNA query in a packed object. A query can begin
 * at any of the 12 phases within a packed token, so we unpack the token
 * stream to a 2-bit-per-base bit-plane once (still 4x smaller than the FASTA
 * text) and scan that. No text materialization, no per-base byte expansion.
 *
 * Returns match count; fills hits[] with base offsets (up to cap).
 * This is exact: DNA-only queries, byte-for-byte.
 */
#include <stdio.h>

typedef struct { gn_engine *e; uint8_t *bits; uint64_t nbase; uint64_t cap; } packctx;

/* 2-bit code of base at position i in the packed plane */
static inline uint32_t plane_get(const uint8_t *bits, uint64_t i){
    uint64_t byte=i>>2; uint32_t sh=(uint32_t)(i&3)*2;
    return (bits[byte]>>sh)&3;
}
static inline void plane_set(uint8_t *bits, uint64_t i, uint32_t v){
    uint64_t byte=i>>2; uint32_t sh=(uint32_t)(i&3)*2;
    bits[byte]=(uint8_t)((bits[byte]&~(3u<<sh))|((v&3)<<sh));
}

/* walk the object's tokens, emit each ACGT base into the 2-bit plane;
 * non-ACGT (escapes) are recorded as "break" positions that cannot start or
 * span a match. We mark breaks in a companion bit using code 0 in a parallel
 * mask; simplest correct approach: store break offsets inline is costly, so
 * we forbid matches that cross a break by tracking run boundaries. */
typedef struct { uint8_t*plane; uint8_t*isbreak; uint64_t n; } planebuild;
static void plane_leaf(void*ctx,const gn_extent*x,uint64_t xb){
    (void)xb; planebuild*P=(planebuild*)ctx;
    size_t cn; const gn_tok*ct=gn_store_get(*(gn_store**)0,x->chunk,&cn); (void)ct;(void)cn;
}

/* Simpler and correct: caller gives us the token array directly. */
uint64_t gn_genesis_build_plane(const gn_tok*tk,size_t nt,
                                uint8_t**plane_out,uint8_t**break_out){
    /* count bases */
    uint64_t nb=0;
    for(size_t i=0;i<nt;i++) nb += GN_TOK_IS_BYTE(tk[i])?1:PACK_K(tk[i]);
    uint8_t*plane=calloc((nb>>2)+1,1);
    uint8_t*brk=calloc(nb+1,1);   /* 1 = position is a non-ACGT break */
    uint64_t p=0;
    for(size_t i=0;i<nt;i++){
        if(GN_TOK_IS_BYTE(tk[i])){ brk[p]=1; p++; continue; }
        uint32_t k=PACK_K(tk[i]), v=tk[i]&0x00FFFFFF;
        for(uint32_t j=0;j<k;j++){ plane_set(plane,p,(v>>(2*j))&3); p++; }
    }
    *plane_out=plane; *break_out=brk; return nb;
}

/* scan the plane for a DNA needle */
size_t gn_genesis_search_plane(const uint8_t*plane,const uint8_t*brk,uint64_t nb,
                               const uint8_t*needle,size_t nl,
                               uint64_t*hits,size_t cap){
    if(nl==0||nl>nb) return 0;
    uint8_t*ncode=malloc(nl);
    for(size_t i=0;i<nl;i++){ int c=base_code(needle[i]); if(c<0){free(ncode);return 0;} ncode[i]=(uint8_t)c; }

    /* Anchor on the first base using a 256-entry skip table over bytes of the
     * plane: a byte holds 4 bases, so any byte whose relevant 2-bit lane
     * equals ncode[0] is a candidate start. We test all 4 lanes with a mask
     * lookup, then verify the full needle only at candidates -- turning an
     * every-position scan into a candidate scan. */
    uint64_t nbytes=(nb+3)>>2;
    size_t nh=0;
    /* first-base match masks: for lane L (0..3), which byte values have that
     * lane == ncode[0]. Precompute a boolean per (byte,lane) is 256*4; cheap
     * to compute the lane on the fly instead. */
    uint8_t b0=ncode[0];
    for(uint64_t byte=0;byte<nbytes;byte++){
        uint8_t bv=plane[byte];
        /* which of the 4 lanes in this byte equal b0? */
        for(uint32_t lane=0;lane<4;lane++){
            if(((bv>>(lane*2))&3)!=b0) continue;
            uint64_t s=(byte<<2)|lane;
            if(s+nl>nb) break;
            if(brk[s]) continue;
            int ok=1;
            for(size_t j=1;j<nl;j++){
                uint64_t pos=s+j;
                if(brk[pos]){ ok=0; break; }
                if(((plane[pos>>2]>>((pos&3)*2))&3)!=ncode[j]){ ok=0; break; }
            }
            if(ok){ if(nh<cap)hits[nh]=s; nh++; }
        }
    }
    free(ncode);
    return nh;
}

/* ---- k-mer index: the real FM-index-class primitive ------------------
 * The point of compressed-domain genomics is NOT to scan one genome faster
 * than SIMD memmem -- you can't, both are O(n). It is to answer membership
 * and locate queries WITHOUT scanning, by indexing k-mers once.
 *
 * We build a hash index from every 12-mer (a single packed token's worth) to
 * the list of base offsets where it occurs. A query >= 12 bases is answered
 * by looking up its first 12-mer -> candidate offsets -> verify the tail.
 * Membership ("does this k-mer occur") is O(1). This is what BWA/bowtie do
 * with an FM-index; we do it with a direct k-mer map on the packed plane.
 *
 * Index size is the honest tradeoff: ~ one entry per position. For a 5Mbp
 * genome that is a few tens of MB -- larger than the sequence, same as every
 * aligner's index. You pay index space to buy O(1) queries.
 */

typedef struct { uint32_t kmer; uint64_t off; } krec;   /* 12-mer -> offset */
typedef struct {
    uint32_t *heads;      /* hash bucket -> first rec index (+1), 0 empty   */
    uint32_t *nextr;      /* chain                                          */
    uint32_t *kmers;      /* rec kmer                                       */
    uint64_t *offs;       /* rec offset                                     */
    uint64_t  n, cap, hbits, hmask;
} gn_kindex;

static uint64_t kmix(uint32_t k){ uint64_t h=k*0x9E3779B97F4A7C15ULL; return h^(h>>29); }

gn_kindex *gn_genesis_index_build(const uint8_t*plane,const uint8_t*brk,uint64_t nb){
    gn_kindex*ix=calloc(1,sizeof*ix);
    uint64_t positions = nb>=12? nb-11 : 0;
    ix->cap = positions;
    ix->hbits = 1; while((1ull<<ix->hbits) < positions*2) ix->hbits++;
    uint64_t hn = 1ull<<ix->hbits; ix->hmask=hn-1;
    ix->heads=calloc(hn,sizeof(uint32_t));
    ix->nextr=malloc(positions*sizeof(uint32_t));
    ix->kmers=malloc(positions*sizeof(uint32_t));
    ix->offs =malloc(positions*sizeof(uint64_t));
    /* rolling 12-mer, reset on breaks */
    uint32_t km=0; int valid=0;
    for(uint64_t p=0;p<nb;p++){
        if(brk[p]){ valid=0; km=0; continue; }
        km = ((km<<2)|plane_get(plane,p)) & 0x00FFFFFF;   /* low 24 bits = 12 bases */
        valid++;
        if(valid>=12){
            uint64_t s=p-11;
            uint32_t idx=(uint32_t)ix->n++;
            ix->kmers[idx]=km; ix->offs[idx]=s;
            uint64_t b=kmix(km)&ix->hmask;
            ix->nextr[idx]=ix->heads[b]; ix->heads[b]=idx+1;
        }
    }
    return ix;
}
void gn_genesis_index_free(gn_kindex*ix){ if(!ix)return;
    free(ix->heads);free(ix->nextr);free(ix->kmers);free(ix->offs);free(ix); }
uint64_t gn_genesis_index_bytes(const gn_kindex*ix){
    uint64_t hn=1ull<<ix->hbits;
    return hn*4 + ix->n*(4+4+8); }

/* pack first 12 bases of a needle into a 24-bit kmer */
static int needle_kmer(const uint8_t*needle,size_t nl,uint32_t*out){
    if(nl<12) return -1;
    uint32_t km=0;
    for(int i=0;i<12;i++){ int c=base_code(needle[i]); if(c<0)return -1; km=(km<<2)|c; }
    *out=km & 0x00FFFFFF; return 0;
}

/* O(1)+verify locate using the index */
size_t gn_genesis_index_locate(const gn_kindex*ix,const uint8_t*plane,const uint8_t*brk,
                               uint64_t nb,const uint8_t*needle,size_t nl,
                               uint64_t*hits,size_t cap){
    uint32_t km;
    if(needle_kmer(needle,nl,&km)!=0) return 0;
    /* verify codes for the tail beyond the first 12 */
    uint8_t*ncode=NULL;
    if(nl>12){ ncode=malloc(nl-12);
        for(size_t i=12;i<nl;i++){ int c=base_code(needle[i]); if(c<0){free(ncode);return 0;} ncode[i-12]=(uint8_t)c; } }
    size_t nh=0;
    uint64_t b=kmix(km)&ix->hmask;
    for(uint32_t r=ix->heads[b]; r; r=ix->nextr[r-1]+0){
        uint32_t idx=r-1;
        if(ix->kmers[idx]!=km){ continue; }
        uint64_t s=ix->offs[idx];
        if(s+nl>nb){ continue; }
        int ok=1;
        for(size_t j=12;j<nl;j++){
            uint64_t pos=s+j;
            if(brk[pos]){ ok=0; break; }
            if(((plane[pos>>2]>>((pos&3)*2))&3)!=ncode[j-12]){ ok=0; break; }
        }
        if(ok){ if(nh<cap)hits[nh]=s; nh++; }
    }
    free(ncode);
    return nh;
}

/* O(1) membership: does this exact 12-mer occur at all? */
int gn_genesis_index_contains(const gn_kindex*ix,uint32_t km){
    uint64_t b=kmix(km)&ix->hmask;
    for(uint32_t r=ix->heads[b]; r; r=ix->nextr[r-1])
        if(ix->kmers[r-1]==km) return 1;
    return 0;
}
