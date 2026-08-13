#!/usr/bin/env bash
# The ENTIRE suite under -fsanitize=address,undefined.
#
# mingw-w64 gcc ships no libasan/libubsan, so this uses clang, whose
# compiler-rt does provide both on Windows. Verified working by
# tools/santest2.sh (it catches a real heap-buffer-overflow and real UB).
export PATH=/clang64/bin:/usr/bin:$PATH
export GENNA=/d/website/devops/GENNA
cd "$GENNA" || exit 1

export CC=clang
export CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -g -O1 -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -I/mingw64/include -Wall -Wextra"
# zlib/zstd come from the mingw64 prefix; clang64 does not ship them.
export LIBRARY_PATH="/mingw64/lib:${LIBRARY_PATH:-}"
# clang64's clock_gettime is an inline wrapper around clock_gettime64, which
# lives in libwinpthread; gcc links it by default, clang does not.
export LDFLAGS="-lwinpthread"
export TAG=asan-ubsan
# halt_on_error: a sanitizer report must fail the test, not scroll past.
export ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:detect_leaks=0:print_stats=0
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

# ASan is heavy; keep the crash test's kill window long enough that the child
# still gets real work done, and trim the 100MB demo out of scope by using
# fewer fuzz iterations. Coverage is unchanged, volume is lower.
export FUZZ_ITERS=${FUZZ_ITERS:-600}
export CRASH_ROUNDS=${CRASH_ROUNDS:-3}

exec bash tools/run_suite.sh
