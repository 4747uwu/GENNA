#!/usr/bin/env bash
# Re-verify only what the last two edits touched: leak_test.c, leakcheck.c and
# fuzz_test.c (the latter whitespace-only). curate / walfix / ab / bench build
# none of these and were green in the full gate.
source "$(dirname "$0")/env.sh"
LOG=/tmp/gn_final2; mkdir -p $LOG; rm -f $LOG/*

run() {
  local label=$1; shift
  echo "=================================================================="
  echo "### $label"
  echo "=================================================================="
  bash "$@" > "$LOG/$label.log" 2>&1
  local rc=$?
  cat "$LOG/$label.log"
  echo "### $label -> exit $rc"; echo
  return $rc
}

fails=0
run suite      tools/run_suite.sh       || fails=$((fails+1))
run sanitized  tools/run_sanitized.sh   || fails=$((fails+1))
run leak       tools/run_leak.sh        || fails=$((fails+1))
run fuzz12     tools/fuzz_campaign.sh   || fails=$((fails+1))
run corruptfz  tools/run_corruptfuzz.sh || fails=$((fails+1))

echo "### RERUN FINAL: $fails script(s) reported failure"
exit $fails
