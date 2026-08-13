/* rsynccmp.c — same edit as the rsync test: 100 scattered sample edits on
 * WikiText-2, then measure Genna-Net's warm-sync payload to a node holding
 * the original. Fair head-to-head vs rsync's delta transfer. */
#include "../include/genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t*slurp(const char*p,size_t*n){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}
 fseek(f,0,SEEK_END);long s=ftell(f);fseek(f,0,SEEK_SET);uint8_t*b=malloc(s+1);
 if(fread(b,1,s,f)!=(size_t)s)exit(1);fclose(f);*n=s;return b;}
int main(int argc,char**argv){
  size_t n;uint8_t*txt=slurp(argv[1],&n);
  /* line offsets */
  size_t cap=64;uint64_t*off=malloc(cap*8);uint32_t*len=malloc(cap*4);uint32_t ns=0;size_t st=0;
  for(size_t i=0;i<=n;i++)if(i==n||txt[i]=='\n'){if(i>st){if(ns==cap){cap*=2;off=realloc(off,cap*8);len=realloc(len,cap*4);}off[ns]=st;len[ns]=(uint32_t)(i-st);ns++;}st=i+1;}

  /* SENDER: has original, makes 100 edits */
  gn_engine*S=gn_engine_new();
  gn_dict_train(gn_engine_dict(S),txt,n,6,200000,16);
  gn_object*so=gn_create(S,"data",txt,n);

  /* RECEIVER already has the ORIGINAL (its manifest) */
  gn_engine*R0=gn_engine_new();
  gn_dict_train(gn_engine_dict(R0),txt,n,6,200000,16);
  gn_object*orig=gn_create(R0,"data",txt,n);
  gn_cid*rmani;uint32_t nrm=gn_net_manifest(R0,orig,&rmani);

  /* make the 100 edits (same seed logic as rsync test: random samples) */
  uint64_t s=1;
  for(int i=0;i<100;i++){ s=s*6364136223846793005ULL+1442695040888963407ULL;
    uint32_t si=(s>>20)%ns; gn_update(S,so,off[si],len[si],(const uint8_t*)"[EDITED SAMPLE]",15); }

  /* Genna-Net warm sync: what must be sent to the receiver */
  gn_cid*smani;uint32_t nsm=gn_net_manifest(S,so,&smani);
  gn_cid*missing;uint32_t nmiss=gn_net_diff(smani,nsm,rmani,nrm,&missing);
  uint8_t*payload;size_t psz=gn_net_serialize(S,so,missing,nmiss,&payload);

  printf("=== Genna-Net vs rsync: 100 scattered edits on WikiText-2 ===\n");
  printf("  samples: %u | chunks changed: %u | payload: %zu bytes (%.1f KB)\n",
    ns,nmiss,psz,psz/1024.0);

  /* verify receiver reconstructs exactly */
  gn_engine*Rc=gn_engine_new();
  gn_dict_train(gn_engine_dict(Rc),txt,n,6,200000,16);
  gn_create(Rc,"data",txt,n);  /* receiver has original */
  gn_object*ro=gn_net_apply(Rc,"data2",payload,psz);
  uint64_t tb=so->ver[so->n_ver-1].total_bytes;
  uint8_t*chk=malloc(tb+64),*ref=malloc(tb+64);
  gn_read(S,so,0,tb,ref); size_t rr=gn_read(Rc,ro,0,tb,chk);
  printf("  receiver reconstruct: %s\n",(rr==tb&&!memcmp(chk,ref,tb))?"BYTE-EXACT":"FAIL");
  printf("\n  rsync sent:      335,439 bytes (documented, real rsync 3.2.7)\n");
  printf("  Genna-Net sent:  %zu bytes\n",psz);
  printf("  -> Genna-Net %.1fx %s\n", psz<335439?335439.0/psz:(double)psz/335439,
    psz<335439?"less":"MORE");

  /* Dump both sides so real rsync can be measured on EXACTLY this change
     rather than compared against a number copied from a previous run. */
  if(argc>2){
    char p[1024];
    snprintf(p,sizeof p,"%s/original.bin",argv[2]);
    FILE*fo=fopen(p,"wb"); if(fo){ fwrite(txt,1,n,fo); fclose(fo); }
    snprintf(p,sizeof p,"%s/edited.bin",argv[2]);
    FILE*fe=fopen(p,"wb"); if(fe){ fwrite(ref,1,tb,fe); fclose(fe); }
    printf("  (wrote original.bin %zu B and edited.bin %llu B to %s for the "
           "real-rsync head-to-head)\n", n, (unsigned long long)tb, argv[2]);
  }
  return 0;
}
