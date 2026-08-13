#!/usr/bin/env bash
# Final gate: everything, in order, after all changes.
#
# Logs go into the REPO, not /tmp. Two reasons: /tmp is cleared by the OS, so
# the evidence for a claim would evaporate; and a stale log is worse than no
# log -- an earlier run left a bench.log quoting 41.7x after the node-size
# correction had already moved that number to 18.1x.
source "$(dirname "$0")/env.sh"
LOG=$GENNA/test-logs; mkdir -p "$LOG"; rm -f "$LOG"/*.log

run() { # label, script...
  local label=$1; shift
  echo "=================================================================="
  echo "### $label"
  echo "=================================================================="
  bash "$@" > "$LOG/$label.log" 2>&1
  local rc=$?
  cat "$LOG/$label.log"
  echo "### $label -> exit $rc"
  echo
  return $rc
}

fails=0
run suite      tools/run_suite.sh              || fails=$((fails+1))
run sanitized  tools/run_sanitized.sh          || fails=$((fails+1))
run leak       tools/run_leak.sh               || fails=$((fails+1))
run fuzz12     tools/fuzz_campaign.sh          || fails=$((fails+1))
run curate     tools/run_curate.sh             || fails=$((fails+1))
run walfix     tools/verify_walfix.sh          || fails=$((fails+1))
run corruptfz  tools/run_corruptfuzz.sh        || fails=$((fails+1))
run concurrent tools/run_concurrent.sh          || fails=$((fails+1))
run oocore     tools/run_oocore.sh             || fails=$((fails+1))
run agg        tools/run_agg.sh                || fails=$((fails+1))
run merge      tools/run_merge.sh              || fails=$((fails+1))
run deltabench tools/run_deltabench.sh         || fails=$((fails+1))
run ab         tools/ab_regression.sh          || fails=$((fails+1))
run bench      tools/run_bench.sh              || fails=$((fails+1))
run python     tools/run_python_tests.sh       || fails=$((fails+1))

{
  echo "Genna test logs"
  echo "generated: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "host:      $(uname -s) $(uname -m)"
  echo "compiler:  $(gcc --version | head -1)"
  echo "git/rsync/ffmpeg: $(git --version | awk '{print $3}') / \
$(rsync --version | head -1 | awk '{print $3}') / $(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}')"
  echo
  echo "result: $fails script(s) reported failure"
  echo
  # Scrape the FINAL verdict line, not the last thing that looks like one.
  # verify_walfix deliberately runs a broken build first; grepping for the
  # last "N failures" picked that up and reported a passing gate as failing.
  for f in "$LOG"/*.log; do
    # "N run, N failed, N SKIPPED" is matched FIRST for the python suite: a
    # skipped suite once matched "ALL PASS" from an earlier line and the index
    # reported a green run in which a third of the tests had not executed.
    # A runner that ends with an explicit "VERDICT: ..." line wins outright.
    # Without that, a script whose C test prints "ALL PASS" and then fails a
    # LATER shell-side comparison still reported ALL PASS here, because the
    # scrape takes the last thing that looks like a verdict rather than the
    # script's actual result.
    v=$(grep -oE 'VERDICT: [A-Z]+' "$f" | tail -1)
    [ -n "$v" ] || \
    v=$(grep -oE '([0-9]+ run, [0-9]+ failed, [0-9]+ SKIPPED|WALFIX VERDICT: [A-Z]+|ALL PASS|SURVIVED|BYTE-EXACT|CLEAN|[0-9]+ passed, [0-9]+ failed|0 seed\(s\) with problems)' "$f" | tail -1)
    printf '%-16s %8s bytes   %s\n' "$(basename "$f")" "$(stat -c %s "$f")" "$v"
  done
} > "$LOG/INDEX.txt"

echo "=================================================================="
echo "### FINAL: $fails script(s) reported failure"
echo "### logs: $LOG"
cat "$LOG/INDEX.txt"
echo "=================================================================="
exit $fails
