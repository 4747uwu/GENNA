/* genna_embed.c — lightweight text embedding for near-duplicate detection.
 *
 * HONEST SPEC (what it does and does not do):
 *   DOES: map text -> a 256-dim L2-normalized vector from hashed word-ngram and
 *         char-ngram features. Cosine similarity then catches near-duplicates
 *         that EXACT MATCH cannot: typos, whitespace/punctuation/case changes,
 *         word insertions/deletions, minor reorderings. Verified below against
 *         exact match head-to-head.
 *   DOES NOT: capture deep semantics / synonymy. "cat" vs "feline" scores low.
 *         True paraphrase detection needs a real sentence embedding model.
 *
 * [SEAM: REAL EMBEDDING MODEL]
 *   embed() is the only function to replace for production semantics. Swap its
 *   body for a call to sentence-transformers (e.g. all-MiniLM-L6-v2 -> 384-dim,
 *   or gte-small). Everything downstream — cosine, LSH, the dedup loop, and
 *   Genna's role as the versioned byte-exact store — is unchanged. The vector
 *   dimension is the only thing that moves (set EMB_DIM to match the model).
 *
 * Build: cc -O2 -D_GNU_SOURCE genna_embed.c -o embedtest -lm
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define EMB_DIM 256

static uint64_t fnv(const uint8_t *p, int n){
    uint64_t h=1469598103934665603ULL;
    for(int i=0;i<n;i++){ h^=p[i]; h*=1099511628211ULL; } return h;
}

/* signed feature hashing: hash -> (dim, sign), accumulate. This is the standard
   "hashing trick" that makes a fixed-width vector from arbitrary features. */
static inline void add_feature(float *vec, uint64_t h, float w){
    int dim = (int)(h % EMB_DIM);
    float sign = ((h >> 40) & 1) ? 1.f : -1.f;
    vec[dim] += sign * w;
}

/* [SEAM: REAL EMBEDDING MODEL] — replace this body to upgrade to real semantics */
void gn_embed(const char *text, float *vec){
    for(int i=0;i<EMB_DIM;i++) vec[i]=0.f;
    int L=(int)strlen(text);
    if(L==0){ return; }
    /* lowercase + collapse whitespace runs to single space (normalization —
       this is what makes spacing/case variants map close) */
    char *t=malloc(L+2); int m=0; int prev_space=1;
    for(int i=0;i<L;i++){
        unsigned char c=(unsigned char)text[i];
        if(isspace(c)){ if(!prev_space){ t[m++]=' '; prev_space=1; } }
        else { t[m++]=(char)tolower(c); prev_space=0; }
    }
    while(m>0 && t[m-1]==' ') m--;   /* trim trailing */
    t[m]=0;

    /* word tokens */
    char *words[1024]; int wlen[1024]; int nw=0;
    int i=0;
    while(i<m && nw<1024){
        while(i<m && t[i]==' ') i++;
        int st=i; while(i<m && t[i]!=' ') i++;
        if(i>st){ words[nw]=t+st; wlen[nw]=i-st; nw++; }
    }
    /* word unigrams (dominant signal) + bigrams (order) */
    for(int w=0; w<nw; w++){
        add_feature(vec, fnv((const uint8_t*)words[w], wlen[w]), 1.0f);
        if(w+1<nw){
            uint64_t h = fnv((const uint8_t*)words[w], wlen[w]) * 1099511628211ULL
                       ^ fnv((const uint8_t*)words[w+1], wlen[w+1]);
            add_feature(vec, h, 0.5f);
        }
    }
    /* char 4-grams over the normalized string (typo tolerance: one typo only
       disturbs a few n-grams, the rest still match) */
    for(int k=0; k+4<=m; k++){
        if(t[k]==' ') continue;
        add_feature(vec, fnv((const uint8_t*)(t+k), 4), 0.35f);
    }
    free(t);
    /* L2 normalize so cosine = dot product */
    float nrm=0; for(int d=0;d<EMB_DIM;d++) nrm+=vec[d]*vec[d];
    nrm=sqrtf(nrm); if(nrm>0) for(int d=0;d<EMB_DIM;d++) vec[d]/=nrm;
}

float gn_cosine(const float *a, const float *b){
    float s=0; for(int d=0;d<EMB_DIM;d++) s+=a[d]*b[d]; return s;
}

/* ---- read jsonl text field ---- */
static int read_texts(const char *path, char ***out){
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    char line[8192]; int cap=64,n=0; char **arr=malloc(cap*sizeof(char*));
    while(fgets(line,sizeof line,f)){
        char *k=strstr(line,"\"text\""); const char *val=line; int vlen=(int)strlen(line);
        if(k){ k+=6; while(*k==' '||*k==':')k++; if(*k=='"'){ k++; char *e=k;
            while(*e && !(*e=='"'&&e[-1]!='\\'))e++; val=k; vlen=(int)(e-k);} }
        while(vlen>0 && (val[vlen-1]=='\n'||val[vlen-1]=='\r')) vlen--;
        if(n==cap){cap*=2;arr=realloc(arr,cap*sizeof(char*));}
        arr[n]=malloc(vlen+1); memcpy(arr[n],val,vlen); arr[n][vlen]=0; n++;
    }
    fclose(f); *out=arr; return n;
}

int main(int argc, char **argv){
    if(argc<2){ printf("usage: %s <dataset.jsonl> [threshold]\n",argv[0]); return 1; }
    float THRESH = argc>2 ? atof(argv[2]) : 0.90f;
    char **texts; int n=read_texts(argv[1],&texts);
    printf("loaded %d records\n\n",n);

    float *emb=malloc((size_t)n*EMB_DIM*sizeof(float));
    for(int i=0;i<n;i++) gn_embed(texts[i], emb+(size_t)i*EMB_DIM);

    /* ---- HEAD-TO-HEAD: exact match vs embedding near-dedup ---- */
    /* exact */
    int exact_removed=0; char *edup=calloc(n,1);
    for(int i=0;i<n;i++){ if(edup[i])continue;
        for(int j=i+1;j<n;j++){ if(edup[j])continue;
            if(strcmp(texts[i],texts[j])==0){ edup[j]=1; exact_removed++; } } }
    /* embedding */
    int emb_removed=0; char *mdup=calloc(n,1);
    for(int i=0;i<n;i++){ if(mdup[i])continue;
        for(int j=i+1;j<n;j++){ if(mdup[j])continue;
            if(gn_cosine(emb+(size_t)i*EMB_DIM,emb+(size_t)j*EMB_DIM)>=THRESH){ mdup[j]=1; emb_removed++; } } }

    printf("=== EXACT MATCH vs LEXICAL EMBEDDING (cosine >= %.2f) ===\n",THRESH);
    printf("  exact match caught:      %d duplicates\n",exact_removed);
    printf("  embedding caught:        %d near-duplicates\n",emb_removed);
    printf("  embedding found %d MORE than exact match\n\n",emb_removed-exact_removed);

    printf("near-dupes the embedding caught that exact match MISSED:\n");
    int shown=0;
    for(int i=0;i<n && shown<8;i++){ if(mdup[i])continue;
        for(int j=i+1;j<n && shown<8;j++){
            if(!mdup[j]) continue;
            if(strcmp(texts[i],texts[j])==0) continue;  /* exact would've caught */
            float c=gn_cosine(emb+(size_t)i*EMB_DIM,emb+(size_t)j*EMB_DIM);
            if(c>=THRESH){ printf("  [%.3f] \"%s\" ~ \"%s\"\n",c,texts[i],texts[j]); shown++; } } }
    return 0;
}
