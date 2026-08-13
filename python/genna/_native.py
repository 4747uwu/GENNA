"""Loading and declaring the Genna C library.

Why ctypes and not a compiled CPython extension: a C extension has to be built
against the exact interpreter it runs in. ctypes over a plain C shared library
does not, so one build works for CPython 3.8-3.13, PyPy, conda, and the
Windows Store Python -- which is the difference between "pip install and try it
in a notebook" and "first install a compiler".

Every foreign call declares argtypes and restype. ctypes will happily truncate
a 64-bit pointer to int if you leave restype at its default, which is the
single most common way FFI code corrupts memory on 64-bit.
"""
from __future__ import annotations

import ctypes
import os
import sys
import threading
from ctypes import (
    POINTER, Structure, c_char_p, c_int, c_size_t, c_uint8, c_uint32,
    c_uint64, c_void_p,
)


class BinOpts(Structure):
    """Mirrors gn_bin_opts in include/genna_bin.h. Field order and types must
    match exactly; ctypes cannot check this for you."""
    _fields_ = [
        ("avg_chunk", c_uint32),
        ("min_chunk", c_uint32),
        ("max_chunk", c_uint32),
        ("fixed", c_int),
    ]
from pathlib import Path

__all__ = ["lib", "GENNA_LOCK", "LATEST", "library_path"]

LATEST = 0xFFFFFFFF

#: The engine keeps process-global mutable state (the treap node arena and its
#: free list). ctypes releases the GIL around every foreign call, so two Python
#: threads really can be inside the engine at once -- which would corrupt that
#: arena. Every mutating entry point in this package holds this lock.
#: Documented in README under "Threading".
GENNA_LOCK = threading.RLock()


def _candidate_names() -> list[str]:
    if sys.platform == "win32":
        return ["genna.dll", "libgenna.dll"]
    if sys.platform == "darwin":
        return ["libgenna.dylib", "genna.dylib"]
    return ["libgenna.so", "genna.so"]


def library_path() -> Path:
    """Locate the native library, or raise with somewhere useful to look."""
    override = os.environ.get("GENNA_LIBRARY")
    if override:
        p = Path(override)
        if not p.exists():
            raise FileNotFoundError(f"GENNA_LIBRARY={override} does not exist")
        return p

    here = Path(__file__).resolve().parent
    searched = []
    for d in (here, here / "lib", here.parent, here.parent.parent):
        for name in _candidate_names():
            p = d / name
            searched.append(str(p))
            if p.exists():
                return p
    raise FileNotFoundError(
        "Could not find the Genna native library.\n"
        "Looked in:\n  " + "\n  ".join(searched) + "\n"
        "Build it with:  python setup.py build_native\n"
        "or point GENNA_LIBRARY at an existing build."
    )


def _load() -> ctypes.CDLL:
    path = library_path()
    if sys.platform == "win32":
        # The DLL is built -static-libgcc so it needs nothing else on PATH,
        # but add its directory anyway for anyone who rebuilds it shared.
        try:
            os.add_dll_directory(str(path.parent))
        except (AttributeError, OSError):
            pass
    return ctypes.CDLL(str(path))


lib = _load()

# --------------------------------------------------------------------------
# Signatures. Grouped as in the C headers so they can be diffed against them.
# --------------------------------------------------------------------------
_SIGS = [
    # --- genna_capi.c -----------------------------------------------------
    ("gn_capi_version",       c_char_p, []),
    ("gn_capi_abi",           c_uint32, []),
    ("gn_object_versions",    c_uint32, [c_void_p]),
    ("gn_object_name",        c_char_p, [c_void_p]),
    ("gn_object_bytes",       c_uint64, [c_void_p, c_uint32]),
    ("gn_object_tokens",      c_uint64, [c_void_p, c_uint32]),
    ("gn_object_dict_version", c_uint64, [c_void_p, c_uint32]),
    ("gn_object_has_version", c_int,    [c_void_p, c_uint32]),
    ("gn_stats_flat",         None,     [c_void_p, POINTER(c_uint64)]),
    ("gn_store_chunk_count",  c_uint64, [c_void_p]),
    ("gn_store_token_bytes",  c_uint64, [c_void_p]),
    ("gn_dict_entries",       c_uint32, [c_void_p]),
    ("gn_train",              c_int,    [c_void_p, c_char_p, c_size_t,
                                         c_uint32, c_uint32, c_uint32]),
    ("gn_search_flat",        c_size_t, [c_void_p, c_char_p, c_size_t,
                                         POINTER(c_uint64), POINTER(c_uint32),
                                         c_size_t]),
    ("gn_range_changed",      c_int,    [c_void_p, c_uint32, c_uint32,
                                         c_uint64, c_uint64]),
    ("gn_range_history",      c_uint32, [c_void_p, c_uint64, c_uint64,
                                         POINTER(c_uint32), c_uint32]),
    ("gn_version_new_leaves", c_uint64, [c_void_p, c_uint32, c_uint32]),
    ("gn_arena_release",      None,     []),

    # --- genna_bin.c: binary objects, content-defined chunking ------------
    ("gn_create_binary",      c_void_p, [c_void_p, c_char_p, c_char_p,
                                         c_size_t, POINTER(BinOpts)]),
    ("gn_bin_opts_default",   None,     [POINTER(BinOpts)]),
    ("gn_bin_chunk_count",    c_uint32, [c_void_p, c_uint32]),
    ("gn_morton_order",       c_int,    [c_void_p, c_uint32, POINTER(c_uint32)]),
    ("gn_arena_live_nodes",   c_uint64, []),
    ("gn_arena_node_bytes",   c_uint64, []),

    # --- genna.h ----------------------------------------------------------
    ("gn_engine_new",         c_void_p, []),
    ("gn_engine_free",        None,     [c_void_p]),
    ("gn_create",             c_void_p, [c_void_p, c_char_p, c_char_p, c_size_t]),
    ("gn_object_open",        c_void_p, [c_void_p, c_char_p]),
    ("gn_read",               c_size_t, [c_void_p, c_void_p, c_uint64,
                                         c_size_t, c_char_p]),
    ("gn_read_version",       c_size_t, [c_void_p, c_void_p, c_uint32, c_uint64,
                                         c_size_t, c_char_p]),
    ("gn_update",             c_int,    [c_void_p, c_void_p, c_uint64, c_uint64,
                                         c_char_p, c_size_t]),
    ("gn_delete",             c_int,    [c_void_p, c_char_p]),
    ("gn_trim_history",       c_uint32, [c_void_p, c_void_p, c_uint32]),
    ("gn_graft",              c_int,    [c_void_p, c_void_p, c_uint64,
                                         c_void_p, c_uint64, c_uint64]),
    ("gn_cut",                c_int,    [c_void_p, c_void_p, c_uint64, c_uint64]),

    # --- genna_persist.h --------------------------------------------------
    ("gn_save",               c_int,    [c_void_p, c_char_p]),
    ("gn_open",               c_void_p, [c_char_p]),
    ("gn_close",              None,     [c_void_p]),
    ("gn_wal_active",         c_int,    [c_void_p]),
    ("gn_wal_ok",             c_int,    [c_void_p]),
    ("gn_wal_set_sync",       c_int,    [c_void_p, c_int]),
    ("gn_wal_records",        c_uint64, [c_void_p]),
    ("gn_wal_bytes",          c_uint64, [c_void_p]),
    ("gn_wal_replayed",       c_uint64, [c_void_p]),
    ("gn_store_path",         c_char_p, [c_void_p]),
    ("gn_engine_objects",     c_uint32, [c_void_p]),
    ("gn_engine_object",      c_void_p, [c_void_p, c_uint32]),
]

_missing = []
for _name, _res, _args in _SIGS:
    _fn = getattr(lib, _name, None)
    if _fn is None:
        _missing.append(_name)
        continue
    _fn.restype = _res
    _fn.argtypes = _args

if _missing:
    raise ImportError(
        "The Genna library at %s is missing: %s\n"
        "It is probably an older build -- rebuild it."
        % (library_path(), ", ".join(_missing))
    )

_ABI_EXPECTED = 1
if lib.gn_capi_abi() != _ABI_EXPECTED:
    raise ImportError(
        "Genna C ABI mismatch: library reports %d, this package expects %d. "
        "Rebuild the native library." % (lib.gn_capi_abi(), _ABI_EXPECTED)
    )
