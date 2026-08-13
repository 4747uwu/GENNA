/* genna_kdict.c — a language for sequence data with no word boundaries.
 * DNA, protein, raw byte streams: segment into fixed-width k-grams and let
 * BPE merge frequent k-gram runs. Same gn_dict interface, engine untouched.
 * This is how bioinformatics tokenizes DNA (k-mers); we just make the
 * k-mers the language's words. */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef GN_KMER
#define GN_KMER 8u      /* bases per base-unit; 8 packs to a tidy token */
#endif
#define GN_MAX_ENTRY_BYTES 64u
#define GN_MAX_GRAM 8u

typedef struct { uint32_t off, len; gn_tok id; } dent;
struct gn_dict {
    uint8_t *texts; size_t tl, tc;
    uint32_t *eo,*el,ne,ce; dent*tab; uint32_t tc2; uint32_t maxlen;
};
static uint64_t fnv(const uint8_t*p,size_t n){uint64_t h=1469598103934665603ULL;
    for(size_t i=0;i<n;i++){h^=p[i];h*=1099511628211ULL;}return h;}
static void grow(gn_dict*d);
gn_dict*gn_dict_new(void){gn_dict*d=calloc(1,sizeof*d);d->tc2=1u<<16;
    d->tab=calloc(d->tc2,sizeof(dent));d->ce=1u<<14;d->eo=malloc(d->ce*4);
    d->el=malloc(d->ce*4);d->tc=1u<<20;d->texts=malloc(d->tc);return d;}
void gn_dict_free(gn_dict*d){if(!d)return;free(d->texts);free(d->eo);free(d->el);
    free(d->tab);free(d);}
uint32_t gn_dict_size(const gn_dict*d){return d->ne;}
uint64_t gn_dict_version(const gn_dict*d){return d->ne;}
static dent*slot(const gn_dict*d,const uint8_t*w,size_t n){uint64_t h=fnv(w,n);
    uint32_t m=d->tc2-1,i=(uint32_t)h&m;for(;;){dent*e=&d->tab[i];
    if(e->id==GN_TOK_INVALID)return e;
    if(e->len==n&&!memcmp(d->texts+e->off,w,n))return e;i=(i+1)&m;}}
gn_tok gn_dict_lookup(const gn_dict*d,const uint8_t*w,size_t n){
    if(!n||n>d->maxlen)return GN_TOK_INVALID;return slot(d,w,n)->id;}
static gn_tok add(gn_dict*d,const uint8_t*w,size_t n){
    if(!n||n>GN_MAX_ENTRY_BYTES)return GN_TOK_INVALID;dent*e=slot(d,w,n);
    if(e->id!=GN_TOK_INVALID)return e->id;if(d->ne+1>=GN_BYTE_BASE-1)return GN_TOK_INVALID;
    if(d->tl+n>d->tc){while(d->tl+n>d->tc)d->tc*=2;d->texts=realloc(d->texts,d->tc);}
    if(d->ne==d->ce){d->ce*=2;d->eo=realloc(d->eo,d->ce*4);d->el=realloc(d->el,d->ce*4);}
    uint32_t o=(uint32_t)d->tl;memcpy(d->texts+o,w,n);d->tl+=n;uint32_t x=d->ne++;
    d->eo[x]=o;d->el[x]=(uint32_t)n;e->off=o;e->len=(uint32_t)n;e->id=x+1;
    gn_tok id=e->id;if((uint32_t)n>d->maxlen)d->maxlen=(uint32_t)n;
    if(d->ne*10>d->tc2*7)grow(d);return id;}
static void grow(gn_dict*d){uint32_t oc=d->tc2;dent*old=d->tab;d->tc2*=2;
    d->tab=calloc(d->tc2,sizeof(dent));for(uint32_t i=0;i<oc;i++){
    if(old[i].id==GN_TOK_INVALID)continue;dent*e=slot(d,d->texts+old[i].off,old[i].len);
    *e=old[i];}free(old);}
static const uint8_t BT[256]={0};
const uint8_t*gn_dict_text(const gn_dict*d,gn_tok t,size_t*lo){
    if(GN_TOK_IS_BYTE(t)){static const uint8_t*base=BT;(void)base;
        *lo=1;return &BT[GN_TOK_BYTE_VAL(t)];}
    if(t==GN_TOK_INVALID||t>d->ne){*lo=0;return NULL;}
    *lo=d->el[t-1];return d->texts+d->eo[t-1];}

/* segment into fixed k-mers */
int gn_dict_learn(gn_dict*d,const uint8_t*t,size_t len,uint32_t mx){(void)mx;
    uint32_t added=0;for(size_t i=0;i+GN_KMER<=len;i+=GN_KMER){
        if(gn_dict_lookup(d,t+i,GN_KMER)==GN_TOK_INVALID)
            if(add(d,t+i,GN_KMER)!=GN_TOK_INVALID)added++;}
    /* tail remainder as its own entry */
    size_t rem=len%GN_KMER; if(rem&&gn_dict_lookup(d,t+len-rem,rem)==GN_TOK_INVALID)add(d,t+len-rem,rem);
    return added;}

typedef struct{uint64_t key;uint32_t cnt,ao,al;}pc;
typedef struct{pc*c;uint32_t cap,n;}pt;
static void pi(pt*p,uint32_t c){p->cap=c;p->n=0;p->c=calloc(c,sizeof(pc));}
static void pf(pt*p){free(p->c);}
static void pb(pt*p,const uint8_t*s,uint32_t o,uint32_t l){uint64_t h=fnv(s+o,l);if(!h)h=1;
    uint32_t m=p->cap-1,i=(uint32_t)h&m;while(p->c[i].key){
    if(p->c[i].key==h&&p->c[i].al==l&&!memcmp(s+p->c[i].ao,s+o,l)){p->c[i].cnt++;return;}
    i=(i+1)&m;}if(p->n*10>p->cap*7)return;p->c[i].key=h;p->c[i].cnt=1;
    p->c[i].ao=o;p->c[i].al=l;p->n++;}

int gn_dict_train(gn_dict*d,const uint8_t*t,size_t len,uint32_t rounds,uint32_t merges,uint32_t minc){
    gn_dict_learn(d,t,len,0);
    size_t nu=len/GN_KMER; if(nu==0)return d->ne;
    uint32_t*seq=malloc((nu+2)*4);for(size_t k=0;k<=nu;k++)seq[k]=(uint32_t)(k*GN_KMER);
    size_t ns=nu;
    for(uint32_t r=0;r<rounds;r++){
        pt P;pi(&P,1u<<22);
        for(size_t k=0;k+1<ns;k++){uint32_t o=seq[k],e=seq[k+2>ns?ns:k+2];
            if(e-o>GN_MAX_ENTRY_BYTES)continue;pb(&P,t,o,e-o);}
        uint32_t th=minc;{uint32_t lo=minc,hi=1u<<28;for(int it=0;it<28;it++){
            if(hi-lo<=1)break;uint32_t mid=lo+(hi-lo)/2,c=0;
            for(uint32_t i=0;i<P.cap;i++)if(P.c[i].key&&P.c[i].cnt>=mid)c++;
            if(c>merges)lo=mid;else hi=mid;}th=lo>minc?lo:minc;}
        uint32_t ch=0;for(uint32_t i=0;i<P.cap&&ch<merges;i++){
            if(!P.c[i].key||P.c[i].cnt<th)continue;
            if(gn_dict_lookup(d,t+P.c[i].ao,P.c[i].al)!=GN_TOK_INVALID)continue;
            if(add(d,t+P.c[i].ao,P.c[i].al)!=GN_TOK_INVALID)ch++;}
        pf(&P);if(!ch)break;
        size_t w=0,k=0;while(k<ns){size_t best=1,mg=GN_MAX_GRAM;if(k+mg>ns)mg=ns-k;
            for(size_t g=mg;g>=2;g--){uint32_t o=seq[k],e=seq[k+g];
                if(e-o>GN_MAX_ENTRY_BYTES)continue;
                if(gn_dict_lookup(d,t+o,e-o)!=GN_TOK_INVALID){best=g;break;}}
            seq[w++]=seq[k];k+=best;}seq[w]=seq[ns];ns=w;}
    free(seq);return d->ne;}

size_t gn_tokenize(const gn_dict*d,const uint8_t*t,size_t len,gn_tok*out,size_t cap){
    size_t n=0,i=0;while(i<len){
        size_t maxb=GN_MAX_ENTRY_BYTES;if(i+maxb>len)maxb=len-i;
        /* align to k-mer multiples, longest first */
        gn_tok hit=GN_TOK_INVALID;size_t hl=0;
        size_t g=(maxb/GN_KMER)*GN_KMER;
        for(;g>=GN_KMER;g-=GN_KMER){gn_tok x=gn_dict_lookup(d,t+i,g);
            if(x!=GN_TOK_INVALID){hit=x;hl=g;break;}}
        if(hit==GN_TOK_INVALID){size_t rem=len-i;
            gn_tok x=gn_dict_lookup(d,t+i,rem<GN_KMER?rem:GN_KMER);
            if(x!=GN_TOK_INVALID){hit=x;hl=rem<GN_KMER?rem:GN_KMER;}}
        if(hit!=GN_TOK_INVALID){if(n==cap)return(size_t)-1;out[n++]=hit;i+=hl;continue;}
        if(n==cap)return(size_t)-1;out[n++]=GN_BYTE_BASE+t[i++];}
    return n;}
size_t gn_detok_len(const gn_dict*d,const gn_tok*tk,size_t n){size_t tot=0,l;
    for(size_t i=0;i<n;i++){if(GN_TOK_IS_BYTE(tk[i])){tot++;continue;}
    gn_dict_text(d,tk[i],&l);tot+=l;}return tot;}
size_t gn_detokenize(const gn_dict*d,const gn_tok*tk,size_t n,uint8_t*o,size_t cap){
    size_t w=0;for(size_t i=0;i<n;i++){size_t l;const uint8_t*p=gn_dict_text(d,tk[i],&l);
    if(w+l>cap)return w;memcpy(o+w,p,l);w+=l;}return w;}
