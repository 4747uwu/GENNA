#!/usr/bin/env bash
# Can a gcc-built shared library be loaded by the system (MSVC-built) CPython
# via ctypes? That decides whether the bindings can be ctypes-over-a-DLL
# (works with any Python) or must be a compiled CPython extension (must match
# the interpreter's compiler/ABI).
source "$(dirname "$0")/env.sh"
W=/tmp/gn_abi; mkdir -p $W; rm -f $W/*

cat > $W/probe.c <<'EOF'
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
EXPORT int probe_add(int a, int b) { return a + b; }
EXPORT uint64_t probe_u64(uint64_t a) { return a * 2; }
EXPORT size_t probe_fill(unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = (unsigned char)(i & 0xFF);
    return n;
}
EOF

# -static-libgcc so the DLL does not need MSYS2's runtime on PATH
gcc -O2 -shared -static-libgcc $W/probe.c -o $W/genna_probe.dll || exit 1
echo "built: $(ls -la $W/genna_probe.dll | awk '{print $5}') bytes"
echo "deps:"
objdump -p $W/genna_probe.dll 2>/dev/null | grep -i 'DLL Name' | sed 's/^/   /'
echo "$W/genna_probe.dll"
