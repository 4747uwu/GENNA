/* genna_tokdict.c — load an EXTERNAL LLM tokenizer as Genna's language.
 *
 * The claim under test: swap the learned dictionary for a fixed tokenizer
 * (GPT-2 BPE / tiktoken) so an LLM reads and writes training data directly
 * as Genna tokens, with no byte<->token serialization on the GPU path.
 *
 * The dictionary is not learned here -- it is loaded once from the vocab
 * file and frozen. gn_dict_learn/train are no-ops. Tokenization is greedy
 * longest-match over the loaded vocab, with byte-escape fallback so any byte
 * sequence round-trips exactly (matching how real BPE handles the 256 base
 * bytes). Token id i in the vocab maps to Genna token i+1.
 */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TD_MAXLEN 128u

typedef struct { uint32_t off, len; gn_tok id; } dent;
struct gn_dict {
    uint8_t *texts; size_t tl, tc;
    uint32_t *eo, *el, ne, ce;
    dent *tab; uint32_t tcap;
    uint32_t maxlen;
    uint32_t bytemax[256];   /* longest entry starting with byte b */
    uint64_t version;
};
static uint64_t fnv(const uint8_t*p,size_t n){uint64_t h=1469598103934665603ULL;
    for(size_t i=0;i<n;i++){h^=p[i];h*=1099511628211ULL;}return h;}
static void grow(gn_dict*d);

gn_dict*gn_dict_new(void){gn_dict*d=calloc(1,sizeof*d);
    d->tcap=1u<<17;d->tab=calloc(d->tcap,sizeof(dent));
    d->ce=1u<<16;d->eo=malloc(d->ce*4);d->el=malloc(d->ce*4);
    d->tc=1u<<22;d->texts=malloc(d->tc);return d;}
void gn_dict_free(gn_dict*d){if(!d)return;free(d->texts);free(d->eo);free(d->el);free(d->tab);free(d);}
uint32_t gn_dict_size(const gn_dict*d){return d->ne;}
uint64_t gn_dict_version(const gn_dict*d){return d->version;}

static dent*slot(const gn_dict*d,const uint8_t*w,size_t n){uint64_t h=fnv(w,n);
    uint32_t m=d->tcap-1,i=(uint32_t)h&m;for(;;){dent*e=&d->tab[i];
    if(e->id==GN_TOK_INVALID)return e;
    if(e->len==n&&!memcmp(d->texts+e->off,w,n))return e;i=(i+1)&m;}}
gn_tok gn_dict_lookup(const gn_dict*d,const uint8_t*w,size_t n){
    if(!n||n>d->maxlen)return GN_TOK_INVALID;return slot(d,w,n)->id;}

static gn_tok add_at(gn_dict*d,const uint8_t*w,size_t n,uint32_t want_id){
    dent*e=slot(d,w,n); if(e->id!=GN_TOK_INVALID)return e->id;
    if(d->tl+n>d->tc){while(d->tl+n>d->tc)d->tc*=2;d->texts=realloc(d->texts,d->tc);}
    if(want_id>=d->ce){while(want_id>=d->ce)d->ce*=2;
        d->eo=realloc(d->eo,d->ce*4);d->el=realloc(d->el,d->ce*4);}
    uint32_t o=(uint32_t)d->tl;memcpy(d->texts+o,w,n);d->tl+=n;
    d->eo[want_id]=o;d->el[want_id]=(uint32_t)n;
    if(want_id>=d->ne)d->ne=want_id+1;
    e->off=o;e->len=(uint32_t)n;e->id=want_id+1;
    if((uint32_t)n>d->maxlen)d->maxlen=(uint32_t)n;
    if(d->ne*10>d->tcap*7)grow(d);
    return e->id;}
static void grow(gn_dict*d){uint32_t oc=d->tcap;dent*old=d->tab;d->tcap*=2;
    d->tab=calloc(d->tcap,sizeof(dent));for(uint32_t i=0;i<oc;i++){
    if(old[i].id==GN_TOK_INVALID)continue;dent*e=slot(d,d->texts+old[i].off,old[i].len);*e=old[i];}
    free(old);}

/* the swap: load a tokenizer vocab file (id<TAB>hexbytes per line) */
int gn_tokdict_load(gn_dict*d,const char*path){
    FILE*f=fopen(path,"r"); if(!f)return -1;
    char line[8192]; int loaded=0;
    while(fgets(line,sizeof line,f)){
        char*tab=strchr(line,'\t'); if(!tab)continue;
        *tab=0; uint32_t id=(uint32_t)strtoul(line,NULL,10);
        char*hex=tab+1; size_t hl=strlen(hex);
        while(hl&&(hex[hl-1]=='\n'||hex[hl-1]=='\r'))hex[--hl]=0;
        size_t n=hl/2; uint8_t buf[4096]; if(n>4096)n=4096;
        for(size_t i=0;i<n;i++){unsigned v;sscanf(hex+2*i,"%2x",&v);buf[i]=(uint8_t)v;}
        if(n>0){ add_at(d,buf,n,id); loaded++; }
    }
    fclose(f); d->version=d->ne;
    /* per-first-byte longest entry, so tokenize probes only plausible lengths */
    for(uint32_t id=0;id<d->ne;id++){
        if(!d->el[id])continue;
        uint8_t b=d->texts[d->eo[id]]; uint32_t l=d->el[id];
        if(l>d->bytemax[b]) d->bytemax[b]=l;
    }
    return loaded;}

static const uint8_t IDB[256]={0};
const uint8_t*gn_dict_text(const gn_dict*d,gn_tok t,size_t*lo){
    static __thread uint8_t one; (void)one;
    if(GN_TOK_IS_BYTE(t)){static __thread uint8_t b;b=GN_TOK_BYTE_VAL(t);*lo=1;return &b;}
    if(t==GN_TOK_INVALID||t>d->ne){*lo=0;return NULL;}
    *lo=d->el[t-1];return d->texts+d->eo[t-1];}

/* frozen: no learning */
int gn_dict_learn(gn_dict*d,const uint8_t*t,size_t n,uint32_t m){(void)t;(void)n;(void)m;return 0;}
int gn_dict_train(gn_dict*d,const uint8_t*t,size_t n,uint32_t a,uint32_t b,uint32_t c){
    (void)t;(void)n;(void)a;(void)b;(void)c;return 0;}

size_t gn_tokenize(const gn_dict*d,const uint8_t*t,size_t len,gn_tok*out,size_t cap){
    size_t n=0,i=0;
    while(i<len){
        size_t best=0; gn_tok bt=GN_TOK_INVALID;
        size_t mx=len-i; uint32_t bm=d->bytemax[t[i]];
        if(mx>bm)mx=bm; if(mx>TD_MAXLEN)mx=TD_MAXLEN;
        for(size_t L=mx;L>=1;L--){ gn_tok x=gn_dict_lookup(d,t+i,L);
            if(x!=GN_TOK_INVALID){bt=x;best=L;break;} }
        if(bt!=GN_TOK_INVALID){ if(n==cap)return(size_t)-1; out[n++]=bt; i+=best; }
        else { if(n==cap)return(size_t)-1; out[n++]=GN_BYTE_BASE+t[i++]; }
    }
    return n;}
size_t gn_detok_len(const gn_dict*d,const gn_tok*tk,size_t n){size_t tot=0,l;
    for(size_t i=0;i<n;i++){if(GN_TOK_IS_BYTE(tk[i]))tot++;else{gn_dict_text(d,tk[i],&l);tot+=l;}}return tot;}
size_t gn_detokenize(const gn_dict*d,const gn_tok*tk,size_t n,uint8_t*o,size_t cap){
    size_t w=0;for(size_t i=0;i<n;i++){size_t l;const uint8_t*p=gn_dict_text(d,tk[i],&l);
    if(w+l>cap)return w;memcpy(o+w,p,l);w+=l;}return w;}


/* ---- token-presence bitset: the honest O(1) exclusion ----------------
 * Claim 3 as stated ("O(1) no across petabytes") is only true for whole
 * tokens, not arbitrary substrings. Here it is, correctly scoped: a bitset
 * over the vocabulary marking which token ids appear anywhere in the corpus.
 * Built incrementally as objects are created. A query is excluded in O(1) if
 * ANY of its tokens is unmarked. This is a Bloom-filter-free exact negative:
 * no false "present", and instant "absent". */
#include <stdio.h>
static uint8_t *g_present=NULL; static uint32_t g_present_n=0;

void gn_tokpresence_reset(gn_dict*d){
    free(g_present); g_present_n=d->ne; g_present=calloc((g_present_n>>3)+1,1);
}
void gn_tokpresence_mark(const gn_tok*tk,size_t n){
    for(size_t i=0;i<n;i++){ if(GN_TOK_IS_BYTE(tk[i]))continue;
        uint32_t id=tk[i]-1; if(id<g_present_n) g_present[id>>3]|=1u<<(id&7); }
}
/* returns 1 if the string is DEFINITELY ABSENT (some token never seen),
 * 0 if it MIGHT be present (all tokens seen -> must verify).  O(tokens in q). */
int gn_tokpresence_excluded(gn_dict*d,const uint8_t*s,size_t n){
    gn_tok q[256]; size_t nq=gn_tokenize(d,s,n,q,256);
    if(nq==(size_t)-1) return 0;
    for(size_t i=0;i<nq;i++){ if(GN_TOK_IS_BYTE(q[i])) return 0; /* raw byte: can't exclude */
        uint32_t id=q[i]-1;
        if(id>=g_present_n || !(g_present[id>>3]&(1u<<(id&7)))) return 1; /* absent token */
    }
    return 0;
}
