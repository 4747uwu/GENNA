"""Build the Genna native library and ship it inside the wheel.

This is deliberately not a CPython extension. The library is a plain C shared
object loaded with ctypes, so a single build works across interpreters and
there is no Python-ABI matching to get wrong. setuptools is used only to
compile it and place it next to the package.
"""
from __future__ import annotations

import os
import platform
import subprocess
import sys
from pathlib import Path

from setuptools import setup
from setuptools.command.build_py import build_py as _build_py

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                      # the Genna C repo
SRC = ROOT / "src"
INC = ROOT / "include"

SOURCES = [
    SRC / "genna_engine3.c",
    SRC / "genna_ext.c",
    SRC / "genna_dict2.c",
    SRC / "genna_persist.c",
    SRC / "genna_capi.c",
    SRC / "genna_bin.c",
]


def _libname() -> str:
    if sys.platform == "win32":
        return "genna.dll"
    if sys.platform == "darwin":
        return "libgenna.dylib"
    return "libgenna.so"


PY64 = sys.maxsize > 2**32


def _cc_is_right_arch(cc: str) -> bool:
    """Does this compiler target the same word size as this interpreter?

    On Windows it is common to have a 32-bit MinGW first on PATH while Python
    is 64-bit. Building with it produces a library that loads with a bare
    "not a valid Win32 application" much later, which is a miserable way to
    discover the problem.
    """
    try:
        out = subprocess.run([cc, "-dumpmachine"], capture_output=True,
                             text=True, timeout=20)
    except Exception:
        return False
    if out.returncode != 0:
        return False
    triple = out.stdout.strip().lower()
    is64 = ("x86_64" in triple or "amd64" in triple or "aarch64" in triple
            or "arm64" in triple)
    return is64 == PY64


def _compiler() -> str:
    # An explicit choice always wins, even if it looks wrong.
    for env in ("GENNA_CC", "CC"):
        cc = os.environ.get(env)
        if cc:
            return cc

    candidates = ["cc", "gcc", "clang"]
    if sys.platform == "win32":
        # Prefer a known-good 64-bit toolchain over whatever is first on PATH.
        candidates = [r"C:\msys64\mingw64\bin\gcc.exe",
                      r"C:\msys64\clang64\bin\clang.exe"] + candidates

    tried = []
    for cc in candidates:
        if _cc_is_right_arch(cc):
            return cc
        tried.append(cc)

    raise SystemExit(
        "No C compiler targeting %d-bit was found.\n"
        "Tried: %s\n"
        "Install one and/or set GENNA_CC:\n"
        "  Linux : apt install build-essential\n"
        "  macOS : xcode-select --install\n"
        "  Windows: install MSYS2, then\n"
        "           pacman -S mingw-w64-x86_64-gcc\n"
        "           set GENNA_CC=C:\\msys64\\mingw64\\bin\\gcc.exe"
        % (64 if PY64 else 32, ", ".join(tried))
    )


def _lib_is_right_arch(path: Path) -> bool | None:
    """Inspect the built binary's header. None if the format is unknown."""
    try:
        with open(path, "rb") as f:
            head = f.read(0x200)
    except OSError:
        return None
    if head[:2] == b"MZ":                                   # PE
        if len(head) < 0x40:
            return None
        pe = int.from_bytes(head[0x3C:0x40], "little")
        if pe + 6 > len(head):
            return None
        machine = int.from_bytes(head[pe + 4:pe + 6], "little")
        return (machine in (0x8664, 0xAA64)) == PY64        # x64 / arm64
    if head[:4] == b"\x7fELF":
        return (head[4] == 2) == PY64                       # EI_CLASS
    if head[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
        return PY64                                         # Mach-O 64
    if head[:4] in (b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce"):
        return not PY64                                     # Mach-O 32
    return None


def build_native(dest_dir: Path) -> Path:
    dest_dir.mkdir(parents=True, exist_ok=True)
    out = dest_dir / _libname()

    missing = [str(s) for s in SOURCES if not s.exists()]
    if missing:
        raise SystemExit(
            "Genna C sources not found:\n  " + "\n  ".join(missing) +
            "\nBuild from a checkout that contains the C engine."
        )

    cc = _compiler()
    cmd = [cc, "-O2", "-std=c11", "-fPIC", "-shared",
           f"-I{INC}", "-o", str(out)] + [str(s) for s in SOURCES]

    if sys.platform == "win32":
        # -static-libgcc so the DLL depends on nothing but the system CRT;
        # otherwise importing genna requires MSYS2 on PATH.
        # --export-all-symbols because MinGW auto-exports only when NOTHING is
        # explicitly marked, and genna_capi.c uses __declspec(dllexport).
        cmd.insert(1, "-static-libgcc")
        cmd.insert(2, "-Wl,--export-all-symbols")
        cmd.remove("-fPIC")             # meaningless on Windows, warns
    elif sys.platform == "darwin":
        cmd += ["-Wl,-install_name,@rpath/" + _libname()]
    else:
        cmd += ["-Wl,-soname," + _libname()]

    # A compiler invoked by absolute path on Windows still needs its own
    # directory on PATH to find libgcc/libwinpthread/libisl etc. Without this
    # gcc.exe fails to start and reports nothing useful.
    env = dict(os.environ)
    ccdir = os.path.dirname(cc)
    if ccdir:
        env["PATH"] = ccdir + os.pathsep + env.get("PATH", "")

    print("building Genna native library:\n  " + " ".join(cmd), flush=True)
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit(
            f"C compiler {cc!r} not found. Install one (build-essential / "
            f"Xcode CLT / MSYS2 mingw-w64) or set GENNA_CC."
        )
    # Never swallow the compiler's own diagnosis: a build that fails silently
    # is the difference between a five-minute try and a lost afternoon.
    if proc.stdout:
        print(proc.stdout, flush=True)
    if proc.stderr:
        print(proc.stderr, file=sys.stderr, flush=True)
    if proc.returncode != 0:
        raise SystemExit(
            f"native build failed (exit {proc.returncode}) using {cc!r}.\n"
            f"See the compiler output above."
        )
    if not out.exists():
        raise SystemExit(f"compiler reported success but {out} is missing")

    arch_ok = _lib_is_right_arch(out)
    if arch_ok is False:
        out.unlink(missing_ok=True)
        raise SystemExit(
            f"{cc} produced a library for the wrong architecture: this Python "
            f"is {64 if PY64 else 32}-bit.\n"
            f"Set GENNA_CC to a matching compiler and reinstall."
        )

    print(f"  -> {out} ({out.stat().st_size} bytes, "
          f"{'arch verified' if arch_ok else 'arch unverified'})", flush=True)
    return out


class build_py(_build_py):
    """Compile the native library into the package before it is copied."""

    def run(self):
        build_native(HERE / "genna")
        super().run()


setup(
    packages=["genna"],
    package_dir={"genna": "genna"},
    package_data={"genna": ["*.dll", "*.so", "*.dylib"]},
    include_package_data=True,
    cmdclass={"build_py": build_py},
    zip_safe=False,
)
