/* edge.c — boundary conditions that fuzzing with mid-range offsets misses. */
#include "../include/genna.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails=0;
#define CK(c,m) do{ if(c) printf("  ok   %s\n",m); else {printf("  FAIL %s\n",m);fails++;} }while(0)
int main(void){
  gn_engine*e=gn_engine_new();
  gn_dict_train(gn_engine_dict(e),(const uint8_t*)"hello world hello",17,6,1000,2);

  /* empty object */
  gn_object*o=gn_create(e,"empty",(const uint8_t*)"",0);
  uint8_t buf[256]; size_t r=gn_read(e,o,0,256,buf);
  CK(r==0,"read empty object -> 0 bytes");

  /* insert into empty */
  CK(gn_update(e,o,0,0,(const uint8_t*)"abc",3)==0,"insert into empty object");
  r=gn_read(e,o,0,256,buf); buf[r]=0;
  CK(r==3&&!memcmp(buf,"abc",3),"empty+insert reads back");

  /* delete everything */
  CK(gn_update(e,o,0,3,NULL,0)==0,"delete entire content");
  r=gn_read(e,o,0,256,buf);
  CK(r==0,"emptied object reads 0");

  /* read past end */
  gn_object*o2=gn_create(e,"x",(const uint8_t*)"hello",5);
  r=gn_read(e,o2,3,100,buf); buf[r]=0;
  CK(r==2&&!memcmp(buf,"lo",2),"read past end clamps");
  r=gn_read(e,o2,100,10,buf);
  CK(r==0,"read entirely past end -> 0");

  /* delete more than exists */
  CK(gn_update(e,o2,2,999,NULL,0)==0,"delete beyond end clamps");
  r=gn_read(e,o2,0,256,buf); buf[r]=0;
  CK(r==2&&!memcmp(buf,"he",2),"over-delete leaves prefix");

  /* insert at exact end (append) */
  CK(gn_update(e,o2,2,0,(const uint8_t*)"XY",2)==0,"append at exact end");
  r=gn_read(e,o2,0,256,buf); buf[r]=0;
  CK(r==4&&!memcmp(buf,"heXY",4),"append reads back");

  /* offset exactly at length, zero delete, zero insert */
  CK(gn_update(e,o2,4,0,NULL,0)==0,"noop edit at end");
  r=gn_read(e,o2,0,256,buf); buf[r]=0;
  CK(r==4&&!memcmp(buf,"heXY",4),"noop edit unchanged");

  /* offset past length should fail cleanly, not crash */
  int rc=gn_update(e,o2,999,0,(const uint8_t*)"z",1);
  CK(rc!=0,"insert past end rejected");

  /* single huge insert */
  size_t big=2u<<20; uint8_t*B=malloc(big); memset(B,'Q',big);
  gn_object*o3=gn_create(e,"h",(const uint8_t*)"ab",2);
  CK(gn_update(e,o3,1,0,B,big)==0,"2MB insert mid-object");
  r=gn_read(e,o3,0,4,buf); buf[4]=0;
  CK(buf[0]=='a'&&buf[1]=='Q'&&buf[2]=='Q',"huge insert seam correct");
  CK(o3->ver[o3->n_ver-1].total_bytes==big+2,"huge insert length correct");
  free(B);

  /* read zero bytes */
  r=gn_read(e,o2,2,0,buf);
  CK(r==0,"zero-length read -> 0");

  /* version 0 of never-edited object */
  r=gn_read_version(e,o2,0,0,256,buf);
  CK(r>0,"v0 of edited object readable");

  gn_engine_free(e);
  printf("\n%s (%d failures)\n",fails?"EDGE FAILURES":"ALL EDGE CASES PASS",fails);
  return fails!=0;
}
