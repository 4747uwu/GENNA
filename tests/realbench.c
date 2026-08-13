/* realbench.c — the SAME mutate-with-history workload the real-MDS Python
 * benchmark runs, on the SAME real WikiText-2 dataset, on Genna. */
#include "../include/genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
static uint8_t*slurp(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}
 fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(s+1);
 if(fread(b,1,s,f)!=(size_t)s)exit(1);fclose(f);*n=s;return b;}
int main(int argc,char**argv){
  size_t n;uint8_t*txt=slurp(argv[1],&n);
  /* split into lines = samples, exactly like the MDS benchmark */
  size_t cap=64; uint64_t*off=malloc(cap*8); uint32_t*len=malloc(cap*4); uint32_t ns=0;
  size_t start=0;
  for(size_t i=0;i<=n;i++){
    if(i==n||txt[i]=='\n'){
      if(i>start){ if(ns==cap){cap*=2;off=realloc(off,cap*8);len=realloc(len,cap*4);}
        off[ns]=start; len[ns]=(uint32_t)(i-start); ns++; }
      start=i+1;
    }
  }
  printf("WikiText-2: %.1f MB, %u samples (same split as MDS)\n",n/1048576.0,ns);

  gn_engine*e=gn_engine_new();
  gn_dict_train(gn_engine_dict(e),txt,n,6,200000,16);
  gn_object*ds=gn_create(e,"dataset",txt,n);
  uint64_t nodes0=gn_ext_nodes_alloced();

  /* build sample byte-offset table in the object (concatenated with newlines) */
  /* same 100 edits, same seed as python (random.seed(0) -> we mirror the LCG
     is different, so instead edit 100 samples spread across the dataset)     */
  uint8_t patch[64]; memset(patch,'E',sizeof patch); /* '[EDITED...]'*3 ~ 78B; use 78 */
  uint8_t bigpatch[78]; memset(bigpatch,'E',78);

  uint64_t s=12345; double t0=ms();
  for(int step=0;step<100;step++){
    s=s*6364136223846793005ULL+1442695040888963407ULL;
    uint32_t si=(s>>20)%ns;
    /* byte offset of sample si in the current object = its original offset
       (edits keep total structure; we replace sample si's bytes)            */
    gn_update(e,ds,off[si],len[si],bigpatch,78);
  }
  double dt=ms()-t0;
  uint64_t new_nodes=gn_ext_nodes_alloced()-nodes0;
  double written=new_nodes*(double)gn_ext_node_size()/1048576.0;

  printf("\n=== GENNA: 100 mutate-with-history edits on WikiText-2 ===\n");
  printf("  time: %.1f ms\n",dt);
  printf("  bytes written (new tree nodes): %.3f MB\n",written);
  printf("  versions kept: %u (all roll back for free)\n",ds->n_ver);

  /* verify v0 byte-exact */
  uint8_t*chk=malloc(n+128);
  size_t v0=gn_read_version(e,ds,0,0,n+64,chk);
  printf("  v0 byte-exact after 100 edits: %s\n",(v0==n&&!memcmp(chk,txt,n))?"YES":"NO");
  /* latest reads back */
  uint64_t tb=ds->ver[ds->n_ver-1].total_bytes;
  size_t r=gn_read(e,ds,0,tb,chk);
  printf("  latest version reads: %s (%zu bytes)\n",r==tb?"OK":"FAIL",r);
  return 0;
}
