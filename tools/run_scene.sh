#!/usr/bin/env bash
# 3D spatial layer: instance transforms + quantized geometry, on real scans.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
LDFLAGS=${LDFLAGS:-}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c src/genna_scene.c"
W=/tmp/gn_scene; mkdir -p $W; rm -f $W/*

echo "== build =="
$CC $CFLAGS tests/scene_test.c $ENGINE -o $W/scene_test.exe -lm $LDFLAGS -lz -lzstd -lz -lzstd || exit 1
echo "   ok ($CC)"
echo
"$W/scene_test.exe" "$@"
