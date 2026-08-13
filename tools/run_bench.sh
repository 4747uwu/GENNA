#!/usr/bin/env bash
# Every documented head-to-head, re-measured on this machine with the real
# tool actually installed and running -- not compared against a number copied
# from a previous report.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS="-O2 -g -D_GNU_SOURCE -Iinclude"
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
# vbench swaps the LANGUAGE module: genna_vdict.c defines the same gn_dict_*
# symbols as genna_dict2.c, so it replaces it rather than joining it. The
# Makefile's old bench line listed both and could never have linked.
ENGINE_V="src/genna_engine3.c src/genna_ext.c src/genna_vdict.c src/genna_persist.c"
C=$GENNA/corpora
W=/tmp/gn_bench; mkdir -p $W; rm -rf $W/*; mkdir -p $W

# awk instead of bc (not installed); ms() gives wall-clock milliseconds.
now_ns() { date +%s%N; }
fdiv() { awk -v a="$1" -v b="$2" -v d="${3:-2}" 'BEGIN{if(b==0){print "n/a"}else{printf "%.*f", d, a/b}}'; }

echo "############ TOOL VERSIONS (documented -> measured here) ############"
echo "  git    2.43        -> $(git --version | awk '{print $3}')"
echo "  rsync  3.2.7       -> $(rsync --version | head -1 | awk '{print $3}')"
echo "  ffmpeg (unstated)  -> $(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}')"
echo

echo "############ BUILD ############"
$CC $CFLAGS tests/realbench.c $ENGINE                 -o $W/realbench.exe || exit 1
$CC $CFLAGS tests/gitcmp.c    $ENGINE                 -o $W/gitcmp.exe    || exit 1
$CC $CFLAGS tests/rsynccmp.c  $ENGINE src/genna_net.c -o $W/rsynccmp.exe  || exit 1
$CC $CFLAGS -DGN_CHUNK_TARGET_TOKENS=30 tests/vbench.c $ENGINE_V -o $W/vbench.exe -lpsapi || exit 1
echo "  ok"
echo

# ====================================================================
echo "############ 1. vs MosaicML MDS - WikiText-2, mutate with history ############"
echo "  documented: 2,755x less written, 119x faster"
"$W/realbench.exe" "$C/wikitext-2-raw/wiki.train.raw"
echo
echo "  NOTE: the MDS side is a Python harness (mosaicml-streaming) that is NOT"
echo "        in this repo, so only Genna's side is re-measured above. The"
echo "        2,755x ratio is therefore NOT re-verified here."
echo

# ====================================================================
echo "############ 2. vs git - Redis source, 100 commits ############"
echo "  documented: 60x less written/edit, 91x faster old-version read"
"$W/gitcmp.exe" "$C/redis_src.txt" | tee $W/gitcmp.out
GN_HIST_MB=$(grep -oE '\+[0-9.]+ MB tree nodes' $W/gitcmp.out | grep -oE '[0-9.]+')
GN_SLICE=$(grep -oE '20KB slice of oldest version: [0-9.]+' $W/gitcmp.out | grep -oE '[0-9.]+$')
echo
echo "  --- real git, same workload (100 commits, one appended line each) ---"
R=$W/gitrepo; rm -rf $R; mkdir -p $R
cp -r "$C/redis/src" "$R/src"
find "$R/src" -type f ! -name '*.c' ! -name '*.h' -delete
( cd $R
  git init -q .
  git config user.email b@b; git config user.name b; git config commit.gpgsign false
  # Auto-gc repacks mid-run and makes .git SHRINK, which would report a
  # negative "bytes written". Disable it and pack the baseline first, so the
  # delta measured below is purely what the 100 commits added.
  git config gc.auto 0
  git add -A && git commit -qm init
  git gc -q --aggressive 2>/dev/null || git gc -q )
GIT0=$(du -sb $R/.git | cut -f1)
FILES=($(cd $R && git ls-files | head -100))
T0=$(now_ns)
( cd $R
  for i in $(seq 0 99); do
    f=${FILES[$((i % ${#FILES[@]}))]}
    echo "// edit $i" >> "$f"
    git add "$f" && git commit -qm "edit $i"
  done )
T1=$(now_ns)
GIT1=$(du -sb $R/.git | cut -f1)
GITMS=$(( (T1 - T0) / 1000000 ))
DELTA=$(( GIT1 - GIT0 ))
echo "  .git after packed baseline: $GIT0 bytes"
echo "  .git after 100 commits:     $GIT1 bytes"
echo "  git WROTE for 100 commits:  $DELTA bytes  ($(fdiv $DELTA 1024 1) KB, $(fdiv $DELTA 100 0) bytes/commit)"
echo "  git commit time: ${GITMS} ms total ($(fdiv $GITMS 100) ms/commit)"
GN_BYTES=$(awk -v m="$GN_HIST_MB" 'BEGIN{printf "%d", m*1048576}')
echo "  genna wrote for 100 commits: $GN_BYTES bytes ($(fdiv $GN_BYTES 100 0) bytes/commit)"
echo "  -> bytes written: genna $(fdiv $DELTA $GN_BYTES 1)x less than git   [documented 60x]"
# old-version read: git cat-file of one file at the oldest commit
FIRST=$(cd $R && git rev-list --max-parents=0 HEAD)
F1=${FILES[0]}
T0=$(now_ns)
( cd $R && for k in $(seq 1 20); do git show "$FIRST:$F1" > /dev/null; done )
T1=$(now_ns)
GITREAD=$(awk -v a="$((T1-T0))" 'BEGIN{printf "%.4f", a/20/1000000}')
echo "  git show <oldest>:$F1 -> $GITREAD ms each (20 runs)"
echo "  genna 20KB slice of oldest version -> $GN_SLICE ms each"
echo "  -> old-version read: genna $(fdiv $GITREAD $GN_SLICE 1)x faster   [documented 91x]"
echo

# ====================================================================
echo "############ 3. vs rsync - WikiText-2, 100 scattered edits ############"
echo "  documented: 10x less over the wire (rsync 3.2.7 sent 335,439 bytes)"
"$W/rsynccmp.exe" "$C/wikitext-2-raw/wiki.train.raw" "$W" | tee $W/rs.out
GN_NET=$(grep -oE 'Genna-Net sent:  [0-9]+' $W/rs.out | grep -oE '[0-9]+$')
echo
echo "  --- real rsync 3.4.4, same change ---"
mkdir -p $W/recv && cp $W/original.bin $W/recv/data.bin
# --no-whole-file forces the delta algorithm even for a local transfer,
# which is what an over-the-wire comparison is about.
rsync --no-whole-file --stats -a "$W/edited.bin" "$W/recv/data.bin" > $W/rsync.out 2>&1
grep -E "Total bytes sent|Literal data|Matched data" $W/rsync.out | sed 's/^/    /'
RS_SENT=$(grep "Total bytes sent" $W/rsync.out | grep -oE '[0-9,]+$' | tr -d ',')
echo "    rsync sent $RS_SENT bytes | Genna-Net sent $GN_NET bytes"
echo "  -> over the wire: Genna-Net $(fdiv $RS_SENT $GN_NET 1)x less   [documented 10x]"
echo

# ====================================================================
echo "############ 4. vs ffmpeg - H.264 structural cuts ############"
echo "  documented: ~110,000x on structural cuts, byte-exact"
"$W/vbench.exe" "$C/vid/sample.264" 1000 "$W/timeline.264" | tee $W/vb.out
GN_CUT=$(grep -oE 'ripple cut   x[0-9]+ +mean [0-9.]+' $W/vb.out | grep -oE '[0-9.]+$')
echo
echo "  --- real ffmpeg 9.0, one equivalent structural cut (-c copy) ---"
TOT=0
for k in 1 2 3; do
  T0=$(now_ns)
  ffmpeg -hide_banner -loglevel error -y -i "$C/vid/sample.264" \
         -ss 10 -to 20 -c copy -f h264 "$W/ff_cut_$k.264"
  T1=$(now_ns)
  MS=$(awk -v a="$((T1-T0))" 'BEGIN{printf "%.2f", a/1000000}')
  echo "    run $k: $MS ms"
  TOT=$(awk -v t="$TOT" -v m="$MS" 'BEGIN{print t+m}')
done
FF=$(fdiv $TOT 3)
echo "    ffmpeg mean: $FF ms   |   genna ripple cut mean: $GN_CUT ms"
echo "  -> structural cut: genna $(fdiv $FF $GN_CUT 0)x faster   [documented ~110,000x]"
echo
echo "############ BENCHMARKS DONE ############"
