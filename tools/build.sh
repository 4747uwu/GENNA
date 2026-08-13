#!/usr/bin/env bash
# Compile-check the engine + the new persistence layer with everything on.
source "$(dirname "$0")/env.sh"
set -o pipefail

CFLAGS="-O2 -g -std=c11 -D_GNU_SOURCE -DGN_HAVE_ZLIB -DGN_HAVE_ZSTD -Iinclude -Wall -Wextra"
ENGINE="src/genna_engine3.c src/genna_ext.c src/genna_dict2.c src/genna_persist.c"

echo "== compiling engine + persistence (warnings are errors here) =="
rc=0
for f in $ENGINE; do
  printf "  %-28s " "$f"
  if out=$(gcc $CFLAGS -c "$f" -o /tmp/$(basename "$f").o 2>&1); then
    if [ -n "$out" ]; then echo "WARNINGS"; echo "$out" | head -20; rc=1; else echo "clean"; fi
  else
    echo "FAILED"; echo "$out" | head -30; rc=1
  fi
done
echo
echo "== libc feature probe (mingw-w64 vs glibc) =="
cat > /tmp/feat.c <<'EOF'
#define _GNU_SOURCE
#include <time.h>
#include <string.h>
#include <stdio.h>
int main(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  const char *h="abcdef"; void *p = memmem(h,6,"cd",2);
  printf("clock_gettime ok, memmem=%s\n", p?"ok":"null");
  return 0;
}
EOF
if gcc $CFLAGS /tmp/feat.c -o /tmp/feat.exe 2>/tmp/feat.err; then
  /tmp/feat.exe
else
  echo "  missing:"; grep -oE "'(clock_gettime|memmem|CLOCK_MONOTONIC)'" /tmp/feat.err | sort -u
fi
exit $rc
