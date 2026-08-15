#!/usr/bin/env bash
# Run a command and, if it fails, republish the interesting output as a
# GitHub ::error:: annotation.
#
# Why this exists: GitHub returns 403 for workflow LOGS without a token, but
# check-run ANNOTATIONS are readable through the public API. Without this, a
# failing job says "Process completed with exit code 1" to anyone outside the
# repo, and diagnosis becomes guesswork -- which is how two wrong inferences
# got made about t/lazy before it was finally reproduced locally.
#
# With it, the assertion that actually failed travels out with the failure.
#
# usage: tools/ci_run.sh <label> <command> [args...]
set -u
label="$1"; shift

log="$(mktemp)"
"$@" > "$log" 2>&1
rc=$?

cat "$log"

if [ "$rc" -ne 0 ]; then
    # The lines a human reads first. Fall back to the tail if none match, so
    # a crash with no recognisable marker still reports something.
    msg="$(grep -nE 'FAIL|VERDICT|Assertion|assert|Error|error:|Segmentation|abort|Abort' "$log" | head -30)"
    [ -z "$msg" ] && msg="$(tail -30 "$log")"

    # Workflow commands are single-line; %0A is the documented newline escape.
    # Cap the length so a runaway log cannot blow past the annotation limit.
    msg="$(printf '%s' "$msg" | sed 's/%/%25/g; s/\r//g' | awk '{printf "%s%%0A", $0}' | cut -c1-3500)"
    echo "::error title=${label} exited ${rc}::${msg}"
fi

rm -f "$log"
exit "$rc"
