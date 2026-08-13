#!/usr/bin/env bash
# The corrupt-store fuzzer under ASan+UBSan: this is where a bad length byte
# or an out-of-range index in a store file turns into a memory error.
export PATH=/clang64/bin:/usr/bin:$PATH
export GENNA=/d/website/devops/GENNA
cd "$GENNA" || exit 1
export ASAN_OPTIONS=halt_on_error=1:detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g -O1 -std=c11 -D_GNU_SOURCE -Iinclude -Wall -Wextra"
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
W=/tmp/gn_cf; mkdir -p $W; rm -f $W/*

clang $CFLAGS tests/fuzz_test.c $ENGINE -o $W/fz.exe -lwinpthread || exit 1
bad=0
for s in ${SEEDS:-11 22 33 44 55 66 77 88 99 101 202 303}; do
  out=$("$W/fz.exe" "$s" 800 "$W/st_$s.gn" 2>&1)
  # Both store kinds are reported. The mappable line was silently dropped by
  # an earlier grep for "corrupted stores:", which its "corrupted MAPPABLE
  # stores:" text does not match -- so the whole mapped load path was being
  # fuzzed and its result thrown away.
  plain=$(echo "$out" | grep -E "[0-9]+ corrupted stores:"          | tail -1)
  mapd=$( echo "$out" | grep -E "[0-9]+ corrupted MAPPABLE stores:" | tail -1)
  if echo "$out" | grep -qE "AddressSanitizer|runtime error|FAIL"; then
    echo "  seed $s: *** PROBLEM ***"; bad=$((bad+1))
    echo "$out" | grep -E "AddressSanitizer|runtime error|FAIL|#0 |#1 " | head -6
  elif [ -z "$plain" ] || [ -z "$mapd" ]; then
    echo "  seed $s: *** PROBLEM *** a store kind reported nothing"; bad=$((bad+1))
  else
    printf "  seed %-4s clean | %s\n" "$s" "${plain#*ok    }"
    printf "  %-9s       | %s\n"      ""   "${mapd#*ok    }"
  fi
  rm -f "$W/st_$s.gn"*
done
echo
echo "corrupt-store campaign: $bad seed(s) with problems"
exit $bad
