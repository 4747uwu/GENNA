#!/usr/bin/env bash
# genna-curate: a real curation session, saved, the process EXITED, reopened
# in a new process, and rolled back — with byte-exactness checked by diff
# against exports taken before the save.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -D_GNU_SOURCE -Iinclude -Wall -Wextra"}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
W=/tmp/gn_cur; mkdir -p $W; rm -rf $W/*
# genna-curate is a native Windows binary. MSYS2 rewrites Unix paths in argv
# for such programs, but NOT paths that appear inside a script file it reads,
# so anything embedded in a .curate script must already be Windows-native.
WN=$(cygpath -m $W)

echo "== build genna-curate =="
$CC $CFLAGS genna_curate.c $ENGINE -o $W/genna-curate.exe -lm || exit 1
echo "   ok"
echo

# ---- session 1: curate, export references, save, EXIT --------------------
cat > $W/s1.curate <<EOF
count
dedup
filter-length 20 0
drop-contains spam
replace model MODEL
versions
export 0 $WN/v0.txt
export 1 $WN/v1.txt
export 2 $WN/v2.txt
stats
save $WN/store.gn
quit
EOF

echo "== session 1: curate -> save -> exit =="
"$W/genna-curate.exe" sample_ml_dataset.jsonl $W/s1.curate > $W/s1.log 2>&1
rc1=$?
sed -n '1,200p' $W/s1.log
echo "   (exit $rc1)"
echo

# capture the head version count reported before exit
NVER=$(grep -oE 'saved [0-9]+ versions' $W/s1.log | grep -oE '[0-9]+')
echo "== process has exited. store on disk: =="
ls -la $W/store.gn $W/store.gn.wal | awk '{printf "   %-26s %10d bytes\n", $9, $5}'
echo

# ---- session 2: NEW PROCESS, open, rollback, export ----------------------
cat > $W/s2.curate <<EOF
count
versions
export 0 $WN/r_v0.txt
export 1 $WN/r_v1.txt
export 2 $WN/r_v2.txt
rollback 0
export $NVER $WN/r_rollback0.txt
count
quit
EOF

echo "== session 2: NEW PROCESS -> open -> rollback =="
"$W/genna-curate.exe" --open $W/store.gn $W/s2.curate > $W/s2.log 2>&1
rc2=$?
sed -n '1,200p' $W/s2.log
echo "   (exit $rc2)"
echo

echo "== byte-exactness: pre-save exports vs post-reload exports =="
fails=0
for v in 0 1 2; do
  if cmp -s $W/v$v.txt $W/r_v$v.txt; then
    printf "   ok    v%s identical across the process boundary (%s bytes)\n" \
        "$v" "$(stat -c %s $W/v$v.txt)"
  else
    printf "   FAIL  v%s DIFFERS after reload\n" "$v"; fails=$((fails+1))
  fi
done

# rollback 0 must reproduce v0 exactly
if cmp -s $WN/v0.txt $WN/r_rollback0.txt; then
  printf "   ok    rollback to v0 after restart is byte-exact vs the original v0 (%s bytes)\n" \
      "$(stat -c %s $WN/v0.txt)"
else
  printf "   FAIL  rollback after restart does not match v0\n"; fails=$((fails+1))
fi

# and v0 must equal the original ingested file
if cmp -s sample_ml_dataset.jsonl $WN/r_v0.txt; then
  printf "   ok    reloaded v0 == the original ingested dataset, byte for byte\n"
else
  printf "   FAIL  reloaded v0 != original dataset\n"; fails=$((fails+1))
fi

echo
if [ $fails -eq 0 ]; then echo "CURATE PERSISTENCE: PASS (0 failures)"; else echo "CURATE PERSISTENCE: $fails FAILURES"; fi
exit $fails
