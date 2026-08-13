#!/usr/bin/env bash
# Out-of-core: mapped store vs heap-resident store.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -DGN_NODE_AGG -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c src/genna_bin.c src/genna_agg.c"
# A mappable store is uncompressed and holds 4-byte tokens per input byte, so
# an N MB payload needs roughly 4N MB of disk plus the compressed store beside
# it. Default to a workdir on the repo's own volume; /tmp here lives on a full
# system drive and the run dies with ENOSPC halfway through.
W=${GN_OO_DIR:-"$GENNA/.oocore-work"}; mkdir -p "$W"; rm -f "$W"/*
MB=${MB:-192}

need=$(( MB * 6 + 256 ))
free=$(df -Pm "$W" 2>/dev/null | awk 'NR==2{print $4}')
if [ -n "$free" ] && [ "$free" -lt "$need" ]; then
  echo "SKIP-FATAL: need ~${need} MB free in $W, have ${free} MB."
  echo "  Set GN_OO_DIR to a volume with room, or lower MB."
  exit 1
fi
echo "   workdir $W (${free} MB free, need ~${need} MB)"

echo "== build =="
$CC $CFLAGS tests/oocore_test.c $ENGINE -o "$W/oo.exe" -lz -lzstd -lpsapi || exit 1
echo "   ok"
echo
"$W/oo.exe" "$W" "$MB" keep; rc=$?

# The aggregates/out-of-core conflict, with one FRESH PROCESS per measurement.
# In-process it is not measurable: after the run above has opened and freed
# other engines the working set stays high, so a later open appears to cost
# almost nothing. Measured that way the monoid arm came out LIGHTER than the
# plain one, which cannot be true.
echo
echo "== aggregates vs out-of-core: same store, one fresh process each =="
P0=$("$W/oo.exe" probe "$W/oo_mapped.gn" noagg | grep '^PROBE ')
P1=$("$W/oo.exe" probe "$W/oo_mapped.gn" agg   | grep '^PROBE ')
if [ -n "$P0" ] && [ -n "$P1" ]; then
  set -- $P0; R0=$2; T0=$3
  set -- $P1; R1=$2; T1=$3
  awk -v r0="$R0" -v t0="$T0" -v r1="$R1" -v t1="$T1" '
    BEGIN {
      printf("     no monoid  : %8.1f MB   (open %s ms)\n", r0/1048576, t0)
      printf("     MAX monoid : %8.1f MB   (open %s ms)\n", r1/1048576, t1)
      if (r1 > r0*2)
        printf("  ok    attaching a monoid COSTS the mapping win: %.1fx more resident, %.1fx slower open\n", r1/r0, (t0>0 ? t1/t0 : 0))
      else
        printf("  FAIL  expected the monoid arm to be much heavier (%.1fx)\n", r1/r0)
    }'
  if [ "$R1" -le $(( R0 * 2 )) ]; then rc=1; fi
else
  echo "  FAIL  probes produced no output"; rc=1
fi

rm -f "$W"/*.gn "$W"/*.gn.wal "$W"/*.gn.tmp
echo
if [ $rc -eq 0 ]; then echo "OUT-OF-CORE VERDICT: PASS"; else echo "OUT-OF-CORE VERDICT: FAIL"; fi
exit $rc
