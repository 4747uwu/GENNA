#!/usr/bin/env bash
# Second sanitizer probe: LLVM/clang on Windows ships compiler-rt, which is
# where a working ASan for this platform would come from.
export PATH=/clang64/bin:/mingw64/bin:/usr/bin:$PATH

T=$(mktemp -d)
cat > "$T/s.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
int main(void){ int *p = malloc(16); p[9] = 1; printf("%d\n", p[9]); free(p); return 0; }
EOF
cat > "$T/u.c" <<'EOF'
#include <stdio.h>
int main(void){ int x = 1; int s = 33; x <<= s; printf("%d\n", x); return 0; }
EOF

echo "clang:  $(clang --version 2>/dev/null | head -1)"
echo "target: $(clang -dumpmachine 2>/dev/null)"
echo
echo "== compiler-rt sanitizer runtimes shipped =="
find /clang64 -iname '*asan*' 2>/dev/null | head -8
find /clang64 -iname '*ubsan*' 2>/dev/null | head -5
echo
echo "== clang -fsanitize=address =="
if clang -fsanitize=address -g "$T/s.c" -o "$T/sa.exe" 2>"$T/sa.err"; then
  echo "  build: OK -- running (expect a heap-buffer-overflow report):"
  PATH=/clang64/bin:$PATH "$T/sa.exe" 2>&1 | head -8
else
  echo "  build: FAILED"; head -4 "$T/sa.err"
fi
echo
echo "== clang -fsanitize=undefined =="
if clang -fsanitize=undefined -g "$T/u.c" -o "$T/su.exe" 2>"$T/su.err"; then
  echo "  build: OK -- running (expect a shift-exponent report):"
  PATH=/clang64/bin:$PATH "$T/su.exe" 2>&1 | head -8
else
  echo "  build: FAILED"; head -4 "$T/su.err"
fi
rm -rf "$T"
