/* genna_net.c — Genna-Net sync core.
 *
 * Tests the claim: syncing an edited object sends only (a) chunks the
 * receiver lacks + (b) a tiny extent structure, not the whole object.
 * Every function MEASURES real bytes, so the benchmark reports wire payload
 * rather than asserting it. Content addressing makes the diff exact:
 * identical chunks hash identically and are never resent.
 */
#include "../include/genna.h"
#include <stdlib.h>
#include <string.h>

/* ---- extent collection (file-scope, single-threaded harness) --------- */
typedef struct { gn_cid c; uint32_t o, l; } ext3;
static ext3   *g_E=NULL; static uint32_t g_nE=0, g_capE=0;
static void collect_leaf(void*ctx,const gn_extent*x,uint64_t xb){
    (void)ctx;(void)xb;
    if(g_nE==g_capE){ g_capE=g_capE?g_capE*2:256; g_E=realloc(g_E,g_capE*sizeof(ext3)); }
    g_E[g_nE++]=(ext3){x->chunk,x->off,x->len};
}
static void collect(gn_object*o){ g_nE=0; gn_ext_walk(o->ver[o->n_ver-1].root, collect_leaf, NULL); }

/* ---- manifest: unique chunk ids a version references ----------------- */
uint32_t gn_net_manifest(gn_engine*e, gn_object*o, gn_cid**out){
    (void)e; collect(o);
    gn_cid*ids=malloc(g_nE*sizeof(gn_cid)); uint32_t n=0;
    for(uint32_t i=0;i<g_nE;i++){ int seen=0;
        for(uint32_t j=0;j<n;j++) if(ids[j]==g_E[i].c){seen=1;break;}
        if(!seen) ids[n++]=g_E[i].c; }
    *out=ids; return n;
}

/* ---- diff: chunks the sender has that the receiver lacks -------------- */
uint32_t gn_net_diff(const gn_cid*sender,uint32_t ns,
                     const gn_cid*receiver,uint32_t nr,gn_cid**missing){
    gn_cid*m=malloc((ns?ns:1)*sizeof(gn_cid)); uint32_t nm=0;
    for(uint32_t i=0;i<ns;i++){ int have=0;
        for(uint32_t j=0;j<nr;j++) if(receiver[j]==sender[i]){have=1;break;}
        if(!have) m[nm++]=sender[i]; }
    *missing=m; return nm;
}

/* ---- serialize: THE wire payload ------------------------------------- */
/* u32 nmiss; {u64 cid,u32 ntok,tok[]}...; u32 next; {u64 cid,u32 off,u32 len}... */
/* Which dict token ids appear in the missing chunks? The receiver needs
 * their text or the tokens dereference to nothing (objects pin their dict).
 * needed_ids: 1 bit per token id present in any transmitted chunk.        */
static size_t emit_dict(gn_engine*e,const gn_cid*missing,uint32_t nm,
                        uint8_t*buf,size_t p,int size_only){
    uint32_t nd=gn_dict_size(gn_engine_dict(e));
    uint8_t*need=calloc((size_t)nd+1,1);
    for(uint32_t i=0;i<nm;i++){ size_t cn; const gn_tok*ct=gn_store_get(gn_engine_store(e),missing[i],&cn);
        for(size_t k=0;k<cn;k++){ if(GN_TOK_IS_BYTE(ct[k]))continue;
            uint32_t id=ct[k]; if(id>=1&&id<=nd) need[id]=1; } }
    uint32_t cnt=0; for(uint32_t id=1;id<=nd;id++) if(need[id])cnt++;
    if(size_only){ size_t sz=4;
        for(uint32_t id=1;id<=nd;id++) if(need[id]){ size_t l; gn_dict_text(gn_engine_dict(e),id,&l); sz+=4+4+l; }
        free(need); return sz; }
    memcpy(buf+p,&cnt,4); p+=4;
    for(uint32_t id=1;id<=nd;id++) if(need[id]){ size_t l; const uint8_t*tx=gn_dict_text(gn_engine_dict(e),id,&l);
        memcpy(buf+p,&id,4); p+=4; uint32_t l32=(uint32_t)l; memcpy(buf+p,&l32,4); p+=4;
        memcpy(buf+p,tx,l); p+=l; }
    free(need); return p;
}

size_t gn_net_serialize(gn_engine*e,gn_object*o,
                        const gn_cid*missing,uint32_t nm,uint8_t**out){
    collect(o);
    size_t dsz=emit_dict(e,missing,nm,NULL,0,1);
    size_t sz=dsz+4;
    for(uint32_t i=0;i<nm;i++){ size_t cn; gn_store_get(gn_engine_store(e),missing[i],&cn);
        sz+=8+4+cn*sizeof(gn_tok); }
    sz+=4+(size_t)g_nE*(8+4+4);
    uint8_t*buf=malloc(sz); size_t p=0;
    p=emit_dict(e,missing,nm,buf,0,0);      /* dict section first */
    memcpy(buf+p,&nm,4); p+=4;
    for(uint32_t i=0;i<nm;i++){ size_t cn; const gn_tok*ct=gn_store_get(gn_engine_store(e),missing[i],&cn);
        memcpy(buf+p,&missing[i],8); p+=8;
        uint32_t c32=(uint32_t)cn; memcpy(buf+p,&c32,4); p+=4;
        memcpy(buf+p,ct,cn*sizeof(gn_tok)); p+=cn*sizeof(gn_tok); }
    memcpy(buf+p,&g_nE,4); p+=4;
    for(uint32_t i=0;i<g_nE;i++){ memcpy(buf+p,&g_E[i].c,8); p+=8;
        memcpy(buf+p,&g_E[i].o,4); p+=4; memcpy(buf+p,&g_E[i].l,4); p+=4; }
    *out=buf; return p;
}

/* how many bytes are the EXTENT structure alone (the "tree delta")? */
size_t gn_net_extent_bytes(gn_engine*e,gn_object*o){
    (void)e; collect(o); return 4+(size_t)g_nE*(8+4+4);
}
uint32_t gn_net_extent_count(gn_engine*e,gn_object*o){ (void)e; collect(o); return g_nE; }

/* ---- apply: receiver rebuilds the object from a payload --------------- *
 * Inserts any chunks it lacks, then rebuilds the object from the extent
 * list. Returns the reconstructed object. The receiver's store dedups, so
 * chunks it already had (not in the payload) are resolved locally.        */
/* receiver installs dict entries at their pinned ids (append-only safe:
 * ids already present are skipped by content). Uses a public loader. */
gn_object* gn_net_apply(gn_engine*e,const char*name,const uint8_t*buf,size_t len){
    size_t p=0; if(p+4>len)return NULL;
    uint32_t nde; memcpy(&nde,buf+p,4); p+=4;
    for(uint32_t i=0;i<nde;i++){ if(p+8>len)return NULL;
        uint32_t id,l; memcpy(&id,buf+p,4); p+=4; memcpy(&l,buf+p,4); p+=4;
        if(p+l>len)return NULL;
        gn_net__install_entry(gn_engine_dict(e),id,buf+p,l); p+=l; }
    if(p+4>len)return NULL;
    uint32_t nm; memcpy(&nm,buf+p,4); p+=4;
    /* ingest missing chunks into the receiver's store */
    for(uint32_t i=0;i<nm;i++){
        if(p+12>len)return NULL;
        gn_cid cid; memcpy(&cid,buf+p,8); p+=8;
        uint32_t ntok; memcpy(&ntok,buf+p,4); p+=4;
        if(p+(size_t)ntok*sizeof(gn_tok)>len)return NULL;
        gn_store_put(gn_engine_store(e),(const gn_tok*)(buf+p),ntok);   /* content-addressed: same cid */
        p+=(size_t)ntok*sizeof(gn_tok);
    }
    if(p+4>len)return NULL;
    uint32_t ne; memcpy(&ne,buf+p,4); p+=4;
    gn_extent*ex=malloc(ne*sizeof(gn_extent)); uint64_t*bl=malloc(ne*sizeof(uint64_t));
    for(uint32_t i=0;i<ne;i++){
        if(p+16>len){free(ex);free(bl);return NULL;}
        memcpy(&ex[i].chunk,buf+p,8); p+=8;
        memcpy(&ex[i].off,buf+p,4); p+=4;
        memcpy(&ex[i].len,buf+p,4); p+=4;
    }
    /* build the object via the engine's own create path using resolved chunks */
    gn_object*o=gn_net__rebuild(e,name,ex,ne);
    free(ex);free(bl);
    return o;
}
