#!/usr/bin/env bash
# The whole correctness suite: pre-existing tests first, then the new ones.
# CC/CFLAGS are overridable so the identical suite runs under sanitizers.
source "$(dirname "$0")/env.sh"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:-"-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"}
LDFLAGS=${LDFLAGS:-}
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"
LDFLAGS="${LDFLAGS:-} -lz -lzstd"
CORPUS=${CORPUS:-sample_ml_dataset.jsonl}
FUZZ_ITERS=${FUZZ_ITERS:-2000}
CRASH_ROUNDS=${CRASH_ROUNDS:-6}
TAG=${TAG:-plain}
W=/tmp/gn_suite_$TAG; mkdir -p $W; rm -rf $W/*; mkdir -p $W

pass=0; fail=0
res() { # name, exitcode
  if [ "$2" -eq 0 ]; then printf "  PASS  %s\n" "$1"; pass=$((pass+1));
  else printf "  FAIL  %s (exit %s)\n" "$1" "$2"; fail=$((fail+1)); fi
}

build() { # out, src...
  local out=$1; shift
  if ! $CC $CFLAGS "$@" $ENGINE -o "$W/$out" $LDFLAGS 2>"$W/$out.buildlog"; then
    echo "  BUILD FAILED: $out"; sed -n '1,25p' "$W/$out.buildlog"; return 1
  fi
  # surface warnings, they are findings too
  if grep -qE 'warning:' "$W/$out.buildlog"; then
    echo "  (warnings building $out:)"; grep -E 'warning:' "$W/$out.buildlog" | head -6
  fi
  return 0
}

echo "############ GENNA SUITE [$TAG] ############"
echo "CC     = $CC"
echo "CFLAGS = $CFLAGS"
echo "corpus = $CORPUS"
echo

echo "===== 1. test_genna (pre-existing: 11 correctness assertions + demo numbers) ====="
if build test_genna.exe tests/test_genna.c; then
  "$W/test_genna.exe"; res "test_genna" $?
else res "test_genna(build)" 1; fi
echo

echo "===== 2. edge (pre-existing: 19 boundary cases) ====="
if build edge.exe tests/edge.c; then
  "$W/edge.exe"; res "edge" $?
else res "edge(build)" 1; fi
echo

echo "===== 3. fuzz (new: splice / search / gc-refcount / persistence) ====="
if build fuzz_test.exe tests/fuzz_test.c; then
  "$W/fuzz_test.exe" 20260811 "$FUZZ_ITERS" "$W/fz.gn"; res "fuzz_test" $?
else res "fuzz_test(build)" 1; fi
echo

echo "===== 4. persist round-trip (new: byte-exact across a process boundary) ====="
if build persist_test.exe tests/persist_test.c; then
  "$W/persist_test.exe" write "$CORPUS" "$W/p.gn" "$W/p.ref" && \
  "$W/persist_test.exe" verify "$W/p.gn" "$W/p.ref"; res "persist_test" $?
else res "persist_test(build)" 1; fi
echo

echo "===== 5. crash recovery (new: SIGKILL mid-edit) ====="
if build crash_test.exe tests/crash_test.c; then
  "$W/crash_test.exe" parent "$W/c.gn" "$W/c.witness" "$CORPUS" "$CRASH_ROUNDS" 350
  res "crash_test" $?
else res "crash_test(build)" 1; fi
echo

echo "############ SUITE [$TAG]: $pass passed, $fail failed ############"
exit $fail
