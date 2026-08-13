#!/usr/bin/env bash
# Aggregates in the node: correct first, then fast, then what they cost.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
BASE="-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_bin.c src/genna_agg.c src/genna_persist.c"
LIBS="-lz -lzstd"
W=${GN_AGG_DIR:-"$GENNA/.agg-work"}; mkdir -p "$W"; rm -f "$W"/*
MB=${MB:-8}

echo "== build (with -DGN_NODE_AGG) =="
$CC $BASE -DGN_NODE_AGG tests/agg_test.c $ENGINE -o "$W/agg.exe" $LIBS || exit 1
echo "   ok"
echo
export GN_AGG_DIR="$W"
"$W/agg.exe" "$MB"; rc=$?

# The A/B that justifies the node width. Building without the define must
# still compile and link -- the calls become no-ops -- and reports the node
# size the engine has when the feature is off.
echo
echo "== A/B: the same engine built WITHOUT -DGN_NODE_AGG =="
$CC $BASE tests/agg_test.c $ENGINE -o "$W/noagg.exe" $LIBS || exit 1
"$W/noagg.exe" "$MB" 2>&1 | head -4

rm -f "$W"/*.exe
echo
if [ $rc -eq 0 ]; then echo "AGGREGATES VERDICT: PASS"; else echo "AGGREGATES VERDICT: FAIL"; fi
exit $rc
