"""Build the Genna native library and ship it inside the wheel.

This is deliberately not a CPython extension. The library is a plain C shared
object loaded with ctypes, so one build works across interpreters and there is
no Python-ABI matching to get wrong.

Users are not expected to run this: CI builds wheels for Linux, macOS and
Windows with cibuildwheel, so `pip install genna` downloads a binary and needs
no compiler. This path exists for the sdist and for source checkouts.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import setuptools.dist
from setuptools import setup
from setuptools.command.build_py import build_py as _build_py

HERE = Path(__file__).resolve().parent      # repo root
SRC = HERE / "src"
INC = HERE / "include"
PKG = HERE / "python" / "genna"

SOURCES = [
    SRC / "genna_engine3.c",
    SRC / "genna_ext.c",
    SRC / "genna_dict2.c",
    SRC / "genna_persist.c",
    SRC / "genna_capi.c",
    SRC / "genna_bin.c",
    SRC / "genna_merge.c",
]

# genna_agg.c is deliberately NOT here. Aggregates only do anything when the
# engine is compiled with -DGN_NODE_AGG, which widens every treap node from 56
# to 64 bytes for everyone -- including the majority who never register a
# monoid. Shipping the file without the define would give the wheel an API
# that always fails. Build from source with GENNA_NODE_AGG=1 to enable it.
if os.environ.get("GENNA_NODE_AGG") == "1":
    SOURCES.append(SRC / "genna_agg.c")

PY64 = sys.maxsize > 2**32


def _libname() -> str:
    if sys.platform == "win32":
        return "genna.dll"
    if sys.platform == "darwin":
        return "libgenna.dylib"
    return "libgenna.so"


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
        "\nYou should not normally need one: `pip install genna` installs a\n"
        "prebuilt wheel. You are seeing this because no wheel matched your\n"
        "platform, so pip fell back to building from source.\n"
        "\nInstall a compiler and/or set GENNA_CC:\n"
        "  Linux : apt install build-essential   (or: yum groupinstall 'Development Tools')\n"
        "  macOS : xcode-select --install\n"
        "  Windows: install MSYS2, then\n"
        "           pacman -S mingw-w64-x86_64-gcc\n"
        "           set GENNA_CC=C:\\msys64\\mingw64\\bin\\gcc.exe"
        % (64 if PY64 else 32, ", ".join(tried))
    )


# The probe MUST call a real symbol from each library. A probe body of
# `int probe(void){return 0;}` compiles and links against anything, because
# the linker has no undefined symbol to resolve and so never extracts the
# archive member -- which is precisely where a missing -fPIC lives. The probe
# passed, the real build then died with
#
#   relocation R_X86_64_PC32 ... can not be used when making a shared object
#
# on stock Ubuntu 24.04. Reference a symbol or the probe tests nothing.
_PROBE_CALL = {
    "zstd.h": "size_t probe(void){ return ZSTD_compressBound(64); }",
    "zlib.h": "const char *probe(void){ return zlibVersion(); }",
}


def _probe_link(cc: str, header: str, flags: list, env) -> bool:
    """Can we build a SHARED library that actually CALLS this, with these flags?

    Three things are being checked, and the last is the one that matters.
    The header alone is not enough -- plenty of systems ship zlib.h with no
    linkable libz. Building a shared object rather than an executable is what
    exposes a static archive compiled without -fPIC. And calling a real symbol
    is what forces the linker to pull that archive member in at all.
    """
    body = _PROBE_CALL.get(header)
    if body is None:                       # unknown header: nothing to prove
        return False
    with tempfile.TemporaryDirectory() as td:
        c = Path(td) / "probe.c"
        c.write_text("#include <stddef.h>\n#include <%s>\n%s\n" % (header, body))
        out = Path(td) / ("probe.dll" if sys.platform == "win32" else "probe.so")
        cmd = [cc, "-shared", str(c), "-o", str(out)]
        if sys.platform != "win32":
            cmd.insert(1, "-fPIC")
        cmd += flags
        try:
            r = subprocess.run(cmd, capture_output=True, text=True,
                               timeout=120, env=env)
        except Exception:
            return False
        return r.returncode == 0 and out.exists()


def _compression_flags(cc: str, env) -> list:
    """Pick link flags for zlib/zstd, preferring a SELF-CONTAINED library.

    Linking these dynamically produces a genna.dll that depends on
    libzstd.dll and zlib1.dll from whatever toolchain built it. That loads
    fine from an MSYS2 shell and fails everywhere else with

        Could not find module 'genna.dll' (or one of its dependencies)

    which names the wrong file and sends people hunting. The same applies to
    a .so against a homebrew libzstd. So try static first and only fall back
    to dynamic, where CI's auditwheel/delvewheel step vendors the libraries
    into the wheel instead.
    """
    if os.environ.get("GENNA_NO_COMPRESSION") == "1":
        return []
    found = []
    for header, lib in (("zstd.h", "zstd"), ("zlib.h", "z")):
        chosen = None
        for flags, how in _static_variants(lib):
            if _probe_link(cc, header, flags, env):
                chosen = (header, lib, flags, how)
                break
        if chosen is None and _probe_link(cc, header, ["-l" + lib], env):
            chosen = (header, lib, ["-l" + lib], "dynamic")
        if chosen:
            found.append(chosen)
    return found


def _static_variants(lib: str) -> list:
    """Ways to ask for a STATIC link, in order, per linker.

    `-Wl,-Bstatic` is GNU ld syntax. Apple's ld does not have it and errors
    out with `unknown option`, so on macOS that probe fails for a reason that
    has nothing to do with whether a static libzstd exists -- and the build
    silently falls through to dynamic, producing a .dylib that needs
    Homebrew's libzstd on every machine that loads it. That is the same class
    of bug as the MSYS2 one, just on a different platform. On Darwin the
    portable move is to hand the linker the archive by absolute path.
    """
    if sys.platform == "darwin":
        out = []
        for prefix in ("/opt/homebrew", "/usr/local", "/opt/local", "/usr"):
            arch = Path(prefix) / "lib" / f"lib{lib}.a"
            if arch.exists():
                out.append(([str(arch)], f"static:{arch}"))
        return out
    return [(["-Wl,-Bstatic", "-l" + lib, "-Wl,-Bdynamic"], "static")]


def build_native(dest_dir: Path) -> Path:
    dest_dir.mkdir(parents=True, exist_ok=True)
    out = dest_dir / _libname()

    missing = [str(s) for s in SOURCES if not s.exists()]
    if missing:
        raise SystemExit(
            "Genna C sources not found:\n  " + "\n  ".join(missing) +
            "\nBuild from a checkout or sdist that contains the C engine."
        )

    cc = _compiler()

    # A compiler invoked by absolute path on Windows still needs its own
    # directory on PATH to find libgcc/libwinpthread/libisl etc. Without this
    # gcc.exe fails to start and reports nothing useful.
    env = dict(os.environ)
    ccdir = os.path.dirname(cc)
    if ccdir:
        env["PATH"] = ccdir + os.pathsep + env.get("PATH", "")

    # _GNU_SOURCE because the engine uses strdup/fsync under -std=c11, which
    # is strict ANSI: without it they are implicit declarations, and GCC 14+
    # makes that a hard error rather than a warning.
    cmd = [cc, "-O2", "-std=c11", "-D_GNU_SOURCE", "-fPIC", "-shared",
           f"-I{INC}", "-o", str(out)]

    # Compression is optional at build time but worth a lot: the whole-payload
    # pass is what makes a store 3.85x smaller on text, and it doubles as the
    # delta encoder for near-identical chunks (PERSISTENCE.md 2.6). Detect it
    # rather than assume it, so a machine without the dev packages still gets
    # a working -- just larger -- store.
    libs = []
    for header, lib, flags, how in _compression_flags(cc, env):
        cmd.append("-DGN_HAVE_ZSTD" if lib == "zstd" else "-DGN_HAVE_ZLIB")
        libs += flags
        print("genna: %s linked %s" % (lib, how), flush=True)
    if not libs:
        print("genna: no zlib/zstd found - stores will be UNCOMPRESSED "
              "(functional, but several times larger)", flush=True)

    cmd += [str(s) for s in SOURCES] + libs

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
        out.unlink()
        raise SystemExit(
            f"{cc} produced a library for the wrong architecture: this Python "
            f"is {64 if PY64 else 32}-bit.\n"
            f"Set GENNA_CC to a matching compiler and reinstall."
        )

    # Does it actually LOAD? A library that links but drags in libzstd.dll
    # from the build toolchain installs perfectly and then fails at `import
    # genna` for anyone whose PATH differs from the builder's -- with an
    # error that names genna.dll rather than the dependency it could not
    # find. Catch it here, where the message can say what is wrong.
    try:
        import ctypes
        ctypes.CDLL(str(out))
    except OSError as e:
        raise SystemExit(
            "the library was built but cannot be loaded:\n  %s\n\n"
            "This usually means it depends on a shared library that is only "
            "on the build machine's PATH (libzstd/zlib). setup.py links those "
            "statically when it can; if you reach this, re-run with\n"
            "  GENNA_NO_COMPRESSION=1 pip install .\n"
            "to build without them." % e
        )

    print(f"  -> {out} ({out.stat().st_size} bytes, "
          f"{'arch verified' if arch_ok else 'arch unverified'}, loads OK)",
          flush=True)
    return out


def _lib_is_right_arch(path: Path):
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


class build_py(_build_py):
    """Compile the native library into the package before it is copied."""

    def run(self):
        build_native(PKG)
        super().run()


CMDCLASS = {"build_py": build_py}

# Without this the wheel is tagged py3-none-ANY, because setuptools sees no
# Extension and concludes the package is pure Python. It is not: it contains a
# .so/.dylib/.dll. PyPI would then serve the Windows build to Linux users, who
# would get an ImportError at `import genna` after a "successful" install.
#
# root_is_pure=False forces a platform tag. The ABI stays "none" and the Python
# tag stays "py3" on purpose -- the library is loaded with ctypes, not the
# CPython C-API, so one build genuinely does work across 3.x.
try:                                    # setuptools >= 70.1
    from setuptools.command.bdist_wheel import bdist_wheel as _bdist_wheel
except ImportError:
    try:
        from wheel.bdist_wheel import bdist_wheel as _bdist_wheel
    except ImportError:
        _bdist_wheel = None

if _bdist_wheel is not None:
    class bdist_wheel(_bdist_wheel):
        def finalize_options(self):
            super().finalize_options()
            self.root_is_pure = False

        def get_tag(self):
            _py, _abi, plat = super().get_tag()
            return "py3", "none", plat

    CMDCLASS["bdist_wheel"] = bdist_wheel


class _BinaryDistribution(setuptools.dist.Distribution):
    """Tell setuptools this distribution is NOT pure Python.

    The engine ships as a prebuilt shared library loaded through ctypes rather
    than as a CPython extension module -- that is what lets one wheel serve
    every 3.x instead of one per interpreter. The cost is that setuptools has
    no ext_modules to look at, so it decided the package was pure, installed it
    into purelib, and bdist_wheel filed the library under

        genna-0.1.0.data/purelib/genna/libgenna.so

    Setting `root_is_pure = False` below fixes the METADATA
    (`Root-Is-Purelib: false`) but not the LAYOUT, because the layout is chosen
    earlier by the install step. auditwheel checks the layout, and refused
    every Linux wheel with

        Invalid binary wheel, found the following shared library/libraries in
        purelib folder: libgenna.so

    while delocate and delvewheel, being laxer, accepted the same wheel on
    macOS and Windows -- so only Linux was red. has_ext_modules() is the switch
    that moves the install to platlib and puts the library at the wheel root.
    """

    def has_ext_modules(self):          # noqa: D102 - see class docstring
        return True


setup(cmdclass=CMDCLASS, distclass=_BinaryDistribution)
