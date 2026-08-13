#!/usr/bin/env bash
# Build the native library into the Python package.
source "$(dirname "$0")/env.sh"

OUT=$GENNA/python/genna/genna.dll
# --export-all-symbols: MinGW auto-exports everything ONLY when no symbol is
# explicitly marked. genna_capi.c uses __declspec(dllexport), which silently
# switches that off and would leave the whole engine unexported.
gcc -O2 -g -std=c11 -shared -static-libgcc -Wall -Wextra \
    -Wl,--export-all-symbols \
    -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude \
    src/genna_engine3.c src/genna_ext.c src/genna_dict2.c \
    src/genna_persist.c src/genna_capi.c src/genna_bin.c \
    -Wl,-Bstatic -lz -lzstd -Wl,-Bdynamic \
    -o "$OUT" || exit 1
echo "built $OUT ($(stat -c %s "$OUT") bytes)"
echo "exported symbols (sample):"
nm -g --defined-only "$OUT" 2>/dev/null | grep -oE 'gn_[a-z_]+' | sort -u | head -12 | sed 's/^/   /'
echo "   ... $(nm -g --defined-only "$OUT" 2>/dev/null | grep -cE ' T gn_') exported gn_* symbols"
echo "dependencies:"
objdump -p "$OUT" | grep -i 'DLL Name' | sed 's/^/   /'
