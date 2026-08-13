/* gitcmp.c — same workload as the git benchmark: store a real codebase, then
 * 100 commits each appending to one file. Measure Genna's storage. */
#include "../include/genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
static double ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1e3+t.tv_nsec/1e6;}
int main(int argc,char**argv){
  /* argv[1] = concatenated codebase (all files) as one object */
  FILE*f=fopen(argv[1],"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  uint8_t*code=malloc(n+1); if(fread(code,1,n,f)!=(size_t)n)return 1; fclose(f);
  gn_engine*e=gn_engine_new();
  gn_dict_train(gn_engine_dict(e),code,n,6,200000,16);
  gn_object*repo=gn_create(e,"repo",code,n);
  gn_stats st0; gn_engine_stats(e,&st0);
  double store_mb=st0.bytes_resident/1048576.0;
  uint64_t nodes0=gn_ext_nodes_alloced();

  printf("Genna initial store of %.1f MB codebase: %.2f MB resident (dict+chunks)\n",
    n/1048576.0, store_mb);

  /* 100 commits: each appends "// edit i\n" at a spread-out offset (like editing
     100 different files). Every commit = a new version, all history kept. */
  double t0=ms();
  for(int i=0;i<100;i++){
    char line[32]; int ln=snprintf(line,sizeof line,"// edit %d\n",i);
    uint64_t at=(uint64_t)((double)i/100*(n-64));  /* spread across the codebase */
    gn_update(e,repo,at,0,(const uint8_t*)line,ln);
  }
  double dt=ms()-t0;
  uint64_t new_nodes=gn_ext_nodes_alloced()-nodes0;
  double version_overhead=new_nodes*(double)gn_ext_node_size()/1048576.0;

  printf("Genna after 100 commits: +%.3f MB tree nodes for full version history\n",version_overhead);
  printf("  time: %.1f ms (%.4f ms/commit)\n",dt,dt/100);
  printf("  versions kept: %u\n",repo->n_ver);
  printf("  total Genna footprint: %.2f MB (store %.2f + history %.3f)\n",
    store_mb+version_overhead, store_mb, version_overhead);

  /* verify v0 exact */
  uint8_t*chk=malloc(n+256);
  size_t v0=gn_read_version(e,repo,0,0,n+128,chk);
  printf("  v0 byte-exact: %s\n",(v0==(size_t)n&&!memcmp(chk,code,n))?"YES":"NO");

  /* read-any-version latency: read the OLDEST version 100x */
  double r0=ms();
  for(int k=0;k<100;k++) gn_read_version(e,repo,0,0,n+128,chk);
  double rd=(ms()-r0)/100;
  printf("\n  read OLDEST version (full %.1f MB): %.3f ms each\n",n/1048576.0,rd);
  /* read a single region of an old version (fair vs git cat-file one file) */
  r0=ms();
  for(int k=0;k<100;k++) gn_read_version(e,repo,0,50000,20000,chk);
  double rd2=(ms()-r0)/100;
  printf("  read a 20KB slice of oldest version: %.4f ms each\n",rd2);
  return 0;
}
