#!/usr/bin/env bash
export PATH=/clang64/bin:/usr/bin:$PATH
export GENNA=/d/website/devops/GENNA
cd "$GENNA" || exit 1
export ASAN_OPTIONS=halt_on_error=1:detect_leaks=0
CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -std=c11 -D_GNU_SOURCE -Iinclude"
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
W=/tmp/gn_r33; mkdir -p $W; rm -f $W/*
clang $CFLAGS tests/fuzz_test.c $ENGINE -o $W/fz.exe -lwinpthread || exit 1
"$W/fz.exe" 33 400 "$W/st.gn" 2>&1 | sed -n '/AddressSanitizer/,/^$/p' | head -30
