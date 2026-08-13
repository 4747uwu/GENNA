#!/usr/bin/env bash
# Probe what this toolchain can actually do: sanitizers, and the tools the
# benchmarks compare against.
source "$(dirname "$0")/env.sh"

T=$(mktemp -d)
cat > "$T/s.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
int main(void){ int *p = malloc(16); p[9] = 1; printf("%d\n", p[9]); free(p); return 0; }
EOF
cat > "$T/u.c" <<'EOF'
#include <stdio.h>
int main(void){ int x = 1; x <<= 33; printf("%d\n", x); return 0; }
EOF

echo "gcc:      $(gcc --version | head -1)"
echo "target:   $(gcc -dumpmachine)"
echo
echo "== libasan / libubsan present? =="
ls /mingw64/lib | grep -iE '^lib(asan|ubsan|tsan)' || echo "  (none found in /mingw64/lib)"
echo
echo "== -fsanitize=address =="
if gcc -fsanitize=address -g "$T/s.c" -o "$T/sa.exe" 2>"$T/sa.err"; then
  echo "  compile+link: OK"
  "$T/sa.exe" 2>&1 | head -5
  echo "  (exit $?)"
else
  echo "  compile+link: FAILED"; head -3 "$T/sa.err"
fi
echo
echo "== -fsanitize=undefined =="
if gcc -fsanitize=undefined -g "$T/u.c" -o "$T/su.exe" 2>"$T/su.err"; then
  echo "  compile+link: OK"
  "$T/su.exe" 2>&1 | head -5
else
  echo "  compile+link: FAILED"; head -3 "$T/su.err"
fi
echo
echo "== external tools =="
for t in git rsync ffmpeg; do
  printf "  %-8s " "$t"
  command -v $t >/dev/null && $t --version 2>&1 | head -1 || echo "MISSING"
done
rm -rf "$T"
