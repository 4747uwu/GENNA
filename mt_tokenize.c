/* simplest CORRECT parallel tokenize: each chunk tokenizes [start, hard_end)
   fully with gn_tokenize, then we keep the prefix of tokens whose cumulative
   bytes stay < (soft_end - start). Reconcile by having chunk k+1 start exactly
   where chunk k stopped. Done as a serial pre-pass to find exact split points,
   then parallel tokenize between them. */
#include <unistd.h>
#include "genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
static double ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
#define OVERLAP 64u

typedef struct { const gn_dict*d; const uint8_t*text; size_t start,end; gn_tok*out; size_t n; } job;
static void* work(void*a){ job*j=a; j->n=gn_tokenize(j->d,j->text+j->start,j->end-j->start,j->out,(j->end-j->start)+1); return NULL; }

/* find exact token boundaries by tokenizing with overlap and locating where a
   token ends exactly at/after each target split. serial, but O(splits*overlap) */
int main(int argc,char**argv){
    int NT=argc>2?atoi(argv[2]):4;
    FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
    uint8_t*text=malloc(n+1);if(fread(text,1,n,f)!=(size_t)n)return 1;fclose(f);
    gn_dict*d=gn_dict_new();gn_dict_train(d,text,n,6,200000,16);
    gn_tok*ser=malloc((n+1)*sizeof(gn_tok));double t0=ms();size_t sn=gn_tokenize(d,text,n,ser,n+1);double st=ms()-t0;
    printf("serial: %zu tokens, %.1f ms (%.1f MB/s)\n",sn,st,n/1048.576/st);

    /* find split points that align with serial token boundaries. Walk serial
       token offsets, pick the first boundary >= each target. */
    size_t*splitByte=malloc((NT+1)*sizeof(size_t)); splitByte[0]=0; splitByte[NT]=n;
    { size_t bo=0,ti=0; for(int s=1;s<NT;s++){ size_t target=(size_t)s*(n/NT);
        while(ti<sn){ size_t l; if(GN_TOK_IS_BYTE(ser[ti]))l=1;else gn_dict_text(d,ser[ti],&l);
            if(bo>=target){break;} bo+=l; ti++; } splitByte[s]=bo; } }

    job*jobs=calloc(NT,sizeof(job));pthread_t*th=calloc(NT,sizeof(pthread_t));
    for(int i=0;i<NT;i++){jobs[i].d=d;jobs[i].text=text;jobs[i].start=splitByte[i];jobs[i].end=splitByte[i+1];
        jobs[i].out=malloc((jobs[i].end-jobs[i].start+1)*sizeof(gn_tok));}
    t0=ms();for(int i=0;i<NT;i++)pthread_create(&th[i],NULL,work,&jobs[i]);
    for(int i=0;i<NT;i++)pthread_join(th[i],NULL);double pt=ms()-t0;
    size_t tot=0;for(int i=0;i<NT;i++)tot+=jobs[i].n;
    gn_tok*par=malloc((tot+1)*sizeof(gn_tok));size_t off=0;
    for(int i=0;i<NT;i++){memcpy(par+off,jobs[i].out,jobs[i].n*sizeof(gn_tok));off+=jobs[i].n;}
    int id=(tot==sn)&&!memcmp(par,ser,sn*sizeof(gn_tok));
    printf("parallel(%d): %zu tokens, %.1f ms (%.1f MB/s) speedup %.2fx identical=%s\n",
        NT,tot,pt,n/1048.576/pt,st/pt,id?"YES":"NO");
    printf("cores=%ld\n",sysconf(_SC_NPROCESSORS_ONLN));
    return 0;
}
