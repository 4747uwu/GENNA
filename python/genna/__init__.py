"""Genna — versioned datasets that don't rewrite themselves.

    import genna

    ds = genna.Dataset.from_jsonl("train.jsonl")
    ds.dedup()                      # exact
    ds.near_dedup(0.85)             # semantic, real embeddings
    ds.save("train.genna")          # whole history, one file

    ds.versions                     # every step, rollback-able
    ds.rollback(0)                  # back to the original, byte-exact

Every curation step is a byte-exact versioned edit costing O(log n), so the
full history of what you did to a dataset is kept for roughly the size of the
edits rather than a copy per step.
"""
from __future__ import annotations

__version__ = "0.1.0"

from .core import (
    Engine,
    GennaError,
    Object,
    Stats,
    Version,
    open_store,
    format_version,
    store_format,
)
from .dataset import Dataset, Record
from .table import Schema, Table

__all__ = [
    "Engine",
    "Object",
    "Version",
    "Stats",
    "GennaError",
    "Dataset",
    "Table",
    "Schema",
    "Record",
    "open_store",
    "format_version",
    "store_format",
    "open",
    "__version__",
]


def open(path):  # noqa: A001 - deliberately shadows builtins inside genna.*
    """Open a saved store: `genna.open("train.genna")`.

    Returns a Dataset if the store holds a single object (the common case),
    otherwise the Engine so you can pick.
    """
    eng = open_store(path)
    if len(eng) == 1:
        return Dataset(_engine=eng, _object=eng.objects[0])
    return eng

# ---------------------------------------------------------------- stability --
# Every name in __all__ appears here, marked stable or unstable. There is no
# unmarked name: `test_api_stability.py` fails if one appears, if a stale entry
# lingers, or if anything marked stable has no test exercising it. A promise
# with nothing behind it is worse than no promise.
#
#   stable    covered by a test; will not change shape within 0.x
#   unstable  may change or vanish in any 0.x release. Usable, but pin the
#             version. When in doubt a name goes here -- unstable-and-honest
#             beats stable-and-regretted.
__stability__ = {
    # the core engine surface
    "Engine":         "stable",
    "Object":         "stable",
    "Version":        "stable",
    "GennaError":     "stable",
    "open_store":     "stable",
    "open":           "stable",
    # on-disk format, small and pinned by a fixture test
    "format_version": "stable",
    "store_format":   "stable",
    "__version__":    "stable",
    # curation
    "Dataset":        "stable",
    "Record":         "unstable",   # field set still moving
    # columnar; the lazy-op surface is still changing shape
    "Table":          "unstable",
    "Schema":         "unstable",
    # introspection struct, expected to grow fields
    "Stats":          "unstable",
}
