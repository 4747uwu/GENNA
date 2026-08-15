#!/usr/bin/env bash
# The Python binding suite.
#
# TWO THINGS THIS SCRIPT EXISTS TO GET RIGHT, both learned the hard way:
#
# 1. THE RIGHT INTERPRETER. The gate runs from an MSYS2 login shell whose PATH
#    does not include the Windows Store Python where pyarrow/onnxruntime are
#    installed. It found *a* python, ran the tests that need no extras, and
#    silently skipped the rest.
#
# 2. A SKIP IS NOT A PASS. Those three suites printed "SKIP" and exited 0, so
#    the gate reported "0 failures" while a third of the Python suite had not
#    run. Skips are now counted, reported, and fail the run -- because a green
#    dashboard that quietly tested nothing is worse than a red one.
GENNA=/d/website/devops/GENNA
cd "$GENNA/python" || exit 1

# --- pick an interpreter that can actually run the whole suite -------------
pick_python() {
  local cands=()
  [ -n "$PYTHON" ] && cands+=("$PYTHON")
  cands+=(python python3)
  # the Windows Store / system installs the MSYS2 PATH does not expose
  for p in /c/Users/*/AppData/Local/Microsoft/WindowsApps/python.exe \
           /c/Python3*/python.exe /c/Program\ Files/Python3*/python.exe; do
    [ -x "$p" ] && cands+=("$p")
  done
  local best=""
  for p in "${cands[@]}"; do
    command -v "$p" >/dev/null 2>&1 || [ -x "$p" ] || continue
    [ -z "$best" ] && best="$p"
    if "$p" -c "import pyarrow, numpy" >/dev/null 2>&1; then
      echo "$p"; return 0
    fi
  done
  echo "$best"
}

PY=$(pick_python)
if [ -z "$PY" ]; then
  echo "### PYTHON SUITE: no interpreter found"; exit 1
fi
echo "interpreter: $PY"
"$PY" -c "import sys; print('  ', sys.executable); print('   version', sys.version.split()[0])"
"$PY" -c "
import importlib
for m in ('pyarrow','numpy','onnxruntime','tokenizers'):
    try:
        importlib.import_module(m); print(f'   {m:12s} present')
    except Exception:
        print(f'   {m:12s} MISSING -> suites needing it will be reported SKIPPED')
"
echo

fails=0; skipped=0; ran=0
for t in tests/test_core.py tests/test_threads.py tests/test_table.py \
         tests/test_query.py tests/test_lazy.py tests/test_sketch.py \
         tests/test_semantic.py tests/test_demo.py \
         tests/test_ci_wiring.py; do
  echo "=================================================================="
  echo "### $t"
  echo "=================================================================="
  out=$("$PY" "$t" 2>&1); rc=$?
  echo "$out"
  ran=$((ran+1))
  if [ $rc -ne 0 ]; then
    fails=$((fails+1))
  elif echo "$out" | grep -qE '^SKIP|SKIP:|SKIP '; then
    # exited 0 but did not actually test what it claims to
    echo "   >>> COUNTED AS SKIPPED, NOT PASSED <<<"
    skipped=$((skipped+1))
  fi
  echo
done

echo "### PYTHON SUITE: $ran run, $fails failed, $skipped SKIPPED"
if [ $skipped -gt 0 ]; then
  echo "### A skipped suite is not a passing suite. Install the extras for"
  echo "### $PY, or set PYTHON= to an interpreter that has them."
fi
[ $fails -eq 0 ] && [ $skipped -eq 0 ]
