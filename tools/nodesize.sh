#!/usr/bin/env bash
# How big is a treap node, really? Several benchmarks hardcode 24 bytes.
source "$(dirname "$0")/env.sh"
W=/tmp/gn_ns; mkdir -p $W; rm -f $W/*

cat > $W/ns.c <<'EOF'
#include "../include/genna.h"
#include <stdio.h>
int main(void){
    gn_engine *e = gn_engine_new();
    const char *s = "the engine holds data below the layer";
    gn_object *o = gn_create(e, "a", (const uint8_t*)s, 36);
    for (int i = 0; i < 200; i++) gn_update(e, o, 5, 2, (const uint8_t*)"xy", 2);
    unsigned long long n = gn_ext_nodes_alloced();
    unsigned long long b = gn_ext_node_bytes();
    printf("live nodes      : %llu\n", n);
    printf("node bytes total: %llu\n", b);
    printf("bytes per node  : %.1f   <-- benchmarks assume 24.0\n",
           n ? (double)b / n : 0.0);
    gn_engine_free(e);
    return 0;
}
EOF
gcc -O2 -std=c11 -D_GNU_SOURCE -Iinclude $W/ns.c \
    src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c \
    -o $W/ns.exe || exit 1
"$W/ns.exe"
