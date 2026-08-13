#!/usr/bin/env bash
# Prove the missing/stale/torn-log tests are load-bearing: build once with the
# header check neutered (the pre-fix behaviour, via -DGN_WALFIX_OFF, no source
# edit) and once normally. The first MUST fail, the second MUST pass.
#
# The success condition for arm A is INVERTED -- a passing pre-fix build would
# mean the test proves nothing. Hence the explicit verdict at the end: without
# it, a summary that scrapes for "N failures" picks up arm A and reads like a
# real regression.
source "$(dirname "$0")/env.sh"

CC=gcc
CFLAGS="-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude"
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
LIBS="-lz -lzstd"
W=/tmp/gn_walfix; mkdir -p $W; rm -rf $W/*; mkdir -p $W
FILT='log missing|stale log|torn tail|survive the next open|FAIL'

echo "=== A. with the fix DISABLED (wal_header_matches forced to 1) ==="
$CC $CFLAGS -DGN_WALFIX_OFF tests/crash_test.c $ENGINE -o $W/pre.exe $LIBS 2>/dev/null || {
  echo "build failed"; exit 1; }
"$W/pre.exe" parent "$W/a.gn" "$W/a.wit" sample_ml_dataset.jsonl 1 250 > "$W/a.out" 2>&1
grep -E "$FILT" "$W/a.out" | sed 's/^/   /'
echo

echo "=== B. with the fix ENABLED (as shipped) ==="
$CC $CFLAGS tests/crash_test.c $ENGINE -o $W/post.exe $LIBS || exit 1
"$W/post.exe" parent "$W/b.gn" "$W/b.wit" sample_ml_dataset.jsonl 1 250 > "$W/b.out" 2>&1
grep -E "$FILT" "$W/b.out" | sed 's/^/   /'

a_failed=$(grep -c "FAIL" "$W/a.out" 2>/dev/null || true)
b_failed=$(grep -c "FAIL" "$W/b.out" 2>/dev/null || true)
a_failed=${a_failed:-0}; b_failed=${b_failed:-0}
echo
if [ "$a_failed" -gt 0 ] && [ "$b_failed" -eq 0 ]; then
  echo "WALFIX VERDICT: PASS - pre-fix build fails $a_failed checks (as it must),"
  echo "  shipped build passes all of them."
  exit 0
fi
echo "WALFIX VERDICT: FAIL - pre-fix failures=$a_failed, shipped failures=$b_failed."
echo "  Expected >0 and 0. A passing pre-fix build means the test is not load-bearing."
exit 1
