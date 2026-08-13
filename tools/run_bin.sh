#!/usr/bin/env bash
# Binary path: content-defined chunking + Morton ordering, A/B on real scans.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c src/genna_bin.c"
W=/tmp/gn_bin; mkdir -p $W; rm -f $W/*

echo "== build =="
$CC $CFLAGS tests/bin_test.c $ENGINE -o $W/bin_test.exe -lm -lz -lzstd -lz -lzstd || exit 1
echo "   ok"
echo
"$W/bin_test.exe" "$@"
