#!/usr/bin/env bash
# Differential fuzzing against a shadow buffer.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
SEED=${SEED:-20260811}
ITERS=${ITERS:-2000}
W=/tmp/gn_fz; mkdir -p $W; rm -f $W/*

echo "== build =="
$CC $CFLAGS tests/fuzz_test.c $ENGINE -o $W/fuzz_test.exe -lz -lzstd -lz -lzstd || exit 1
echo "   ok ($CC)"
echo
"$W/fuzz_test.exe" "$SEED" "$ITERS" "$W/fuzz_store.gn"
