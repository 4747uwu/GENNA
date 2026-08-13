#!/usr/bin/env bash
# Byte-exact round-trip: two separate processes, real data.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
CORPUS=${CORPUS:-sample_ml_dataset.jsonl}
W=/tmp/gn_pt; mkdir -p $W; rm -f $W/*

echo "== build =="
$CC $CFLAGS tests/persist_test.c $ENGINE -o $W/persist_test.exe -lz -lzstd -lz -lzstd || exit 1
echo "   ok ($CC)"
echo

"$W/persist_test.exe" write "$CORPUS" "$W/store.gn" "$W/ref.bin" || exit 1
echo
ls -la "$W/store.gn" "$W/store.gn.wal" 2>/dev/null | awk '{printf "   %-22s %10d bytes\n", $9, $5}'
echo
# separate process, no shared memory
"$W/persist_test.exe" verify "$W/store.gn" "$W/ref.bin"
rc=$?
exit $rc
