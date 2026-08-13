#!/usr/bin/env bash
# Three-way merge: right bytes on a clean merge, refusal on an overlap.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c src/genna_merge.c"
W=${GN_MERGE_DIR:-"$GENNA/.merge-work"}; mkdir -p "$W"; rm -f "$W"/*
export GN_MERGE_DIR="$W"

echo "== build =="
$CC $CFLAGS tests/merge_test.c $ENGINE -o "$W/merge.exe" -lz -lzstd || exit 1
echo "   ok"
echo
"$W/merge.exe"; rc=$?

rm -f "$W"/*.gn "$W"/*.gn.wal "$W"/*.exe
echo
if [ $rc -eq 0 ]; then echo "MERGE VERDICT: PASS"; else echo "MERGE VERDICT: FAIL"; fi
exit $rc
