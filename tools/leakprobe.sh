#!/usr/bin/env bash
# Does ASan on Windows detect leaks? (LeakSanitizer is a separate component
# from ASan and is not available on every platform.) Probe with a known leak
# rather than assuming either way.
export PATH=/clang64/bin:/usr/bin:$PATH
T=$(mktemp -d)
cat > "$T/l.c" <<'EOF'
#include <stdlib.h>
int main(void){ volatile void *p = malloc(1234); (void)p; return 0; }
EOF
clang -fsanitize=address -g "$T/l.c" -o "$T/l.exe" -lwinpthread || exit 1
echo "== detect_leaks=1 on a program that leaks 1234 bytes =="
ASAN_OPTIONS=detect_leaks=1 "$T/l.exe" 2>&1 | head -12
echo "   (exit ${PIPESTATUS[0]})"
rm -rf "$T"
