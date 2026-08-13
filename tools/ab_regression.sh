#!/usr/bin/env bash
# Did the persistence work move any engine number?
#
# genna_complete.tar.gz holds the ORIGINAL, pre-change sources. Build the same
# benchmarks from both trees and diff the numbers, rather than asserting from
# the diff that nothing could have changed.
source "$(dirname "$0")/env.sh"

CC=gcc
CFLAGS="-O2 -g -D_GNU_SOURCE"
C=$GENNA/corpora
W=/tmp/gn_ab; rm -rf $W; mkdir -p $W/orig
tar -xzf "$GENNA/genna_complete.tar.gz" -C $W/orig || exit 1
O=$W/orig/genna_complete

echo "############ A/B: original tree vs persistence tree ############"
echo

# ---- original: engine WITHOUT genna_persist.c -------------------------
OE="$O/src/genna_engine3.c $O/src/genna_ext.c $O/src/genna_dict2.c"
NE="$GENNA/src/genna_engine3.c $GENNA/src/genna_ext.c $GENNA/src/genna_dict2.c $GENNA/src/genna_persist.c"

echo "== build original =="
$CC $CFLAGS -I$O/include $O/tests/gitcmp.c    $OE -o $W/gitcmp_orig.exe    || exit 1
$CC $CFLAGS -I$O/include $O/tests/realbench.c $OE -o $W/realbench_orig.exe || exit 1
$CC $CFLAGS -I$O/include $O/tests/rsynccmp.c  $OE $O/src/genna_net.c -o $W/rsynccmp_orig.exe || exit 1
echo "   ok"
echo "== build current =="
$CC $CFLAGS -I$GENNA/include $GENNA/tests/gitcmp.c    $NE -o $W/gitcmp_new.exe    || exit 1
$CC $CFLAGS -I$GENNA/include $GENNA/tests/realbench.c $NE -o $W/realbench_new.exe || exit 1
$CC $CFLAGS -I$GENNA/include $GENNA/tests/rsynccmp.c  $NE $GENNA/src/genna_net.c -o $W/rsynccmp_new.exe || exit 1
echo "   ok"
echo

for b in gitcmp realbench rsynccmp; do
  case $b in
    gitcmp)    ARG="$C/redis_src.txt" ;;
    *)         ARG="$C/wikitext-2-raw/wiki.train.raw" ;;
  esac
  echo "=================== $b ==================="
  "$W/${b}_orig.exe" "$ARG" > $W/$b.orig 2>&1
  "$W/${b}_new.exe"  "$ARG" > $W/$b.new  2>&1
  # Drop timing lines: those vary run to run and are not what this is checking.
  # What must be identical is every SIZE, COUNT and byte-exactness verdict.
  clean() { grep -vE 'time:|ms each|ms\)|ms total' "$1"; }
  if diff <(clean $W/$b.orig) <(clean $W/$b.new) > $W/$b.diff; then
    echo "  IDENTICAL (all sizes, counts and byte-exact verdicts unchanged)"
  else
    echo "  *** DIFFERENCES ***"
    cat $W/$b.diff
  fi
  echo "  --- timing lines (informational, vary per run) ---"
  paste -d'|' <(grep -E 'time:|ms each' $W/$b.orig) <(grep -E 'time:|ms each' $W/$b.new) \
    | sed 's/^/    orig: /; s/|/   ||   new: /'
  echo
done
