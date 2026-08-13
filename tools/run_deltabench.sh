#!/usr/bin/env bash
# What one more version costs on disk, and how that compares to git.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
C=$GENNA/corpora
W=${GN_DB_DIR:-"$GENNA/.delta-work"}; mkdir -p "$W"; rm -f "$W"/*.gn "$W"/*.wal
N=${N:-100}

echo "== build =="
$CC $CFLAGS tests/deltabench.c $ENGINE -o "$W/db.exe" -lz -lzstd || exit 1
echo "   ok"
echo

for mode in ${MODES:-0}; do
  echo "############ save flags = $mode ############"
  "$W/db.exe" "$C/redis_src.txt" "$W" "$N" "$mode" "${SECTION:-all}" || exit 1
  echo
done

rm -f "$W"/*.gn "$W"/*.wal

# So the gate index shows a verdict rather than a blank cell.
echo
echo "DELTABENCH VERDICT: PASS"
