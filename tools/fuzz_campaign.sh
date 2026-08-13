#!/usr/bin/env bash
# Many seeds. One passing seed proves nothing; a campaign is the evidence.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
ITERS=${ITERS:-2000}
W=/tmp/gn_fzc; mkdir -p $W; rm -f $W/*

$CC $CFLAGS tests/fuzz_test.c $ENGINE -o $W/fuzz_test.exe -lz -lzstd -lz -lzstd || exit 1

SEEDS=${SEEDS:-"1 2 3 7 42 1337 20260811 99991 2147483647 8675309 123456789 5555"}
pass=0; fail=0
for s in $SEEDS; do
  out=$("$W/fuzz_test.exe" "$s" "$ITERS" "$W/st_$s.gn" 2>&1)
  if echo "$out" | grep -q "FUZZ: ALL PASS"; then
    printf "  seed %-12s PASS\n" "$s"; pass=$((pass+1))
  else
    printf "  seed %-12s FAIL\n" "$s"; fail=$((fail+1))
    echo "$out" | grep -E "FAIL" | head -8
  fi
done
echo
echo "campaign: $pass passed, $fail failed ($ITERS splices per seed)"
exit $fail
