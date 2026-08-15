"""Prove the compression link-probe rejects a static archive built without -fPIC.

This exists because the probe had a bug that made it *always* pass. The body
was `int probe(void){return 0;}` -- it named no symbol from the library, so
the linker had nothing to resolve, never extracted the archive member, and
never saw the non-PIC relocation inside it. The probe said "static zstd links
fine", the real build then failed on stock Ubuntu 24.04 with

    relocation R_X86_64_PC32 ... can not be used when making a shared object

A probe that cannot fail is not a probe. So this test builds the failure on
purpose, out of a synthetic library, and asserts three things:

  1. calling a real symbol from a NON-PIC archive fails to link -shared
     (this is the bug reproducing -- it must now be caught)
  2. calling a real symbol from a PIC archive succeeds
     (the fix must not reject libraries that are actually fine)
  3. the OLD bodyless probe passes against that same non-PIC archive
     (this is the proof the fix changed something real, not just churned)

Assertion 1 and 3 only mean something on ELF targets. On Windows all code is
position-independent and there is nothing to catch, so those two are reported
as N/A rather than quietly counted as passes -- a green tick for a check that
never ran is the exact failure this file is about.

Exit code is nonzero on any failure. Prints an explicit VERDICT line.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Import the probe functions out of setup.py WITHOUT needing setuptools.
#
# Two problems to dodge. setup.py calls setup() at import time, and it imports
# setuptools -- which Python 3.12+ no longer installs by default, so on a
# fresh CI interpreter `import setuptools` is an ImportError before this test
# has done anything. Stub what setup.py imports; none of it is ever called
# here, we only want _probe_link/_PROBE_CALL/_compiler.
import types

if "setuptools" not in sys.modules:
    _st = types.ModuleType("setuptools")
    _st.setup = lambda **kw: None

    class _Cmd:                       # stand-in for build_py / bdist_wheel
        def __init__(self, *a, **k):
            pass

    _cmd = types.ModuleType("setuptools.command")
    for _name in ("build_py", "bdist_wheel"):
        _m = types.ModuleType(f"setuptools.command.{_name}")
        setattr(_m, _name, _Cmd)
        setattr(_cmd, _name, _m)
        sys.modules[f"setuptools.command.{_name}"] = _m
    _st.command = _cmd
    sys.modules["setuptools"] = _st
    sys.modules["setuptools.command"] = _cmd
else:
    import setuptools
    setuptools.setup = lambda **kw: None

sys.path.insert(0, str(ROOT))
import importlib.util
_spec = importlib.util.spec_from_file_location("genna_setup", ROOT / "setup.py")
_setup = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_setup)

FAILS, COUNT, NA = [], 0, 0
IS_ELF = sys.platform not in ("win32", "darwin", "cygwin")


def check(cond, msg):
    global COUNT
    COUNT += 1
    if cond:
        print(f"  ok    {msg}")
    else:
        print(f"  FAIL  {msg}")
        FAILS.append(msg)


def na(msg):
    global NA
    NA += 1
    print(f"  n/a   {msg}")


def build_fake_lib(td: Path, cc: str, pic: bool, env) -> bool:
    """A one-symbol static archive, with or without -fPIC.

    The symbol takes the address of a global so the object contains a
    relocation that actually cares about PIC. A leaf function returning a
    constant would not reproduce the failure.
    """
    (td / "faketest.h").write_text("int fake_symbol(void);\n")
    # A GLOBAL array whose address is stored in a global pointer. Under
    # -fno-pic that pointer initialiser needs an absolute relocation
    # (R_X86_64_32S / R_X86_64_PC32), which is exactly what cannot appear in
    # a shared object. A `static` array touched by a leaf function is not
    # enough: modern GCC defaults to -fPIE, so it stays PC-relative, links
    # fine, and the fixture proves nothing.
    (td / "faketest.c").write_text(
        "int fake_table[64];\n"
        "int *fake_ptr = fake_table;\n"
        "int fake_symbol(void){ fake_ptr[3] = 7; return fake_table[3]; }\n"
    )
    cmd = [cc, "-c", str(td / "faketest.c"), "-o", str(td / "faketest.o")]
    if pic:
        cmd.insert(1, "-fPIC")
    else:
        # -fPIE is the default on every distro that matters, so non-PIC has
        # to be asked for explicitly or this fixture is just a PIC one.
        cmd[1:1] = ["-fno-pic", "-fno-PIE"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, env=env)
    if r.returncode != 0:
        print(f"        (cc failed: {r.stderr.strip()[:160]})")
        return False

    # `ar` must come from the same toolchain as the compiler, and on Windows
    # it is not on PATH by default -- resolve it next to cc rather than
    # hoping the ambient one is compatible.
    ar = os.path.join(os.path.dirname(cc), "ar")
    if not (os.path.exists(ar) or os.path.exists(ar + ".exe")):
        ar = "ar"
    r = subprocess.run([ar, "rcs", str(td / "libfaketest.a"),
                        str(td / "faketest.o")],
                       capture_output=True, text=True, timeout=120, env=env)
    if r.returncode != 0:
        print(f"        (ar failed: {r.stderr.strip()[:160]})")
    return r.returncode == 0


def main() -> int:
    cc = _setup._compiler()
    print(f"compiler: {cc}")
    print(f"platform: {sys.platform} (ELF non-PIC check "
          f"{'applies' if IS_ELF else 'does not apply'})\n")

    env = dict(os.environ)
    ccdir = os.path.dirname(cc)
    if ccdir:
        env["PATH"] = ccdir + os.pathsep + env.get("PATH", "")

    print("-- the probe must call a real symbol --")
    body = _setup._PROBE_CALL.get("zstd.h", "")
    check("ZSTD_compressBound" in body,
          f"zstd probe references a real symbol: {body.strip()}")
    body_z = _setup._PROBE_CALL.get("zlib.h", "")
    check("zlibVersion" in body_z,
          f"zlib probe references a real symbol: {body_z.strip()}")
    check(_setup._probe_link(cc, "nosuch.h", [], env) is False,
          "an unknown header is refused rather than probed blind")

    for pic in (False, True):
        label = "PIC" if pic else "NON-PIC"
        print(f"\n-- against a synthetic {label} static archive --")
        with tempfile.TemporaryDirectory() as tds:
            td = Path(tds)
            if not build_fake_lib(td, cc, pic, env):
                check(False, f"could not build the {label} fixture archive")
                continue

            _setup._PROBE_CALL["faketest.h"] = \
                "int probe(void){ return fake_symbol(); }"
            # Apple's ld has no -Bstatic; hand it the archive by path, the
            # same way setup.py._static_variants does on Darwin.
            if sys.platform == "darwin":
                static = [str(td / "libfaketest.a")]
            else:
                static = ["-L" + str(td), "-Wl,-Bstatic", "-lfaketest",
                          "-Wl,-Bdynamic"]
            got = _setup._probe_link(cc, "faketest.h", ["-I" + str(td)] + static,
                                     env)

            if pic:
                check(got is True,
                      "probe ACCEPTS a PIC archive (fix does not over-reject)")
            elif IS_ELF:
                check(got is False,
                      "probe REJECTS a non-PIC archive (the bug is caught)")
            else:
                na(f"probe returned {got} -- Windows/macOS code is already "
                   f"position-independent, nothing to reject")

            # The regression proof: the old bodyless probe against the very
            # same archive. If this ever starts failing, the fixture stopped
            # reproducing the bug and assertion 1 above proves nothing.
            _setup._PROBE_CALL["faketest.h"] = "int probe(void){ return 0; }"
            old = _setup._probe_link(cc, "faketest.h",
                                     ["-I" + str(td)] + static, env)
            if not pic and IS_ELF:
                check(old is True,
                      "the OLD bodyless probe passed here -- fix is load-bearing")
            elif not pic:
                na("old-probe comparison needs ELF to be meaningful")

    print(f"\n{'PROBE: FAILURES' if FAILS else 'PROBE: ALL PASS'} "
          f"({len(FAILS)} failures / {COUNT} checks, {NA} n/a)")
    for f in FAILS:
        print(f"  - {f}")
    print(f"VERDICT: {'FAIL' if FAILS else 'PASS'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
