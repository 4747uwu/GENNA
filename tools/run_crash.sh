#!/usr/bin/env bash
# SIGKILL a live editing session, repeatedly, and verify recovery each time.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
CORPUS=${CORPUS:-sample_ml_dataset.jsonl}
ROUNDS=${ROUNDS:-6}
MS=${MS:-350}
W=/tmp/gn_ct; mkdir -p $W; rm -f $W/*

echo "== build =="
$CC $CFLAGS tests/crash_test.c $ENGINE -o $W/crash_test.exe -lz -lzstd -lz -lzstd || exit 1
echo "   ok ($CC)"
echo
"$W/crash_test.exe" parent "$W/store.gn" "$W/witness.txt" "$CORPUS" "$ROUNDS" "$MS"
rc=$?
echo
echo "== final store on disk =="
ls -la $W/store.gn $W/store.gn.wal 2>/dev/null | awk '{printf "   %-24s %10d bytes\n", $9, $5}'
exit $rc
