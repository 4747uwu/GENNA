#!/usr/bin/env bash
# Multi-writer optimistic concurrency: N real processes racing on one store.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
W=/tmp/gn_conc; mkdir -p $W; rm -f $W/*
WRITERS=${WRITERS:-8}
ROUNDS=${ROUNDS:-5}

echo "== build =="
$CC $CFLAGS tests/concurrent_test.c $ENGINE -o $W/conc.exe -lz -lzstd || exit 1
echo "   ok"
echo
"$W/conc.exe" parent "$W/store.gn" sample_ml_dataset.jsonl "$WRITERS" "$ROUNDS"
