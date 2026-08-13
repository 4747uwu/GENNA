#!/usr/bin/env bash
# Leak accounting. Deliberately NOT an ASan build (see tests/leakcheck.c).
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS="-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
WRAP="-Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc"
W=/tmp/gn_leak; mkdir -p $W; rm -f $W/*

echo "== build (with allocator wrapping) =="
$CC $CFLAGS tests/leak_test.c tests/leakcheck.c $ENGINE -o $W/leak_test.exe $WRAP -lz -lzstd -lz -lzstd || exit 1
echo "   ok"
echo
"$W/leak_test.exe" "$W/leak_store.gn"
