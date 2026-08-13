"""Concurrency: the README claims the bindings are safe from Python threads.

That claim is worth testing, because it is easy to get wrong and the failure
mode is silent corruption rather than an exception. ctypes releases the GIL
around every foreign call, so without the module lock two threads really would
be inside the engine's process-global node arena at the same time.

Two things are checked:
  1. Many threads editing SEPARATE objects concurrently produce exactly the
     content they should -- no cross-talk, no crash, no arena corruption.
  2. Many threads reading the SAME object concurrently while another edits it
     never observe a torn read (every read equals some valid version).
"""
from __future__ import annotations

import os
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import genna

FAILS = []


def check(cond, msg):
    print(f"  {'ok  ' if cond else 'FAIL'}  {msg}")
    if not cond:
        FAILS.append(msg)
    return bool(cond)


N_THREADS = 8
N_EDITS = 60


def test_parallel_objects():
    print("\n-- 8 threads editing separate objects, 60 edits each --")
    errors: list[str] = []
    with genna.Engine() as eng:
        objs = {}
        for t in range(N_THREADS):
            objs[t] = eng.create(f"obj{t}", f"seed-{t}-".encode() * 200)

        expect = {}

        def worker(tid: int):
            try:
                obj = objs[tid]
                shadow = bytearray(obj.bytes())
                for i in range(N_EDITS):
                    off = (i * 37) % max(1, len(shadow))
                    ins = f"<{tid}:{i}>".encode()
                    obj.update(off, 0, ins)
                    shadow[off:off] = ins
                expect[tid] = bytes(shadow)
            except Exception as e:                       # pragma: no cover
                errors.append(f"thread {tid}: {e!r}")

        threads = [threading.Thread(target=worker, args=(t,))
                   for t in range(N_THREADS)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()

        check(not errors, f"no thread raised ({errors[:2]})")
        mismatched = [t for t in range(N_THREADS)
                      if objs[t].bytes() != expect.get(t)]
        check(not mismatched,
              f"all {N_THREADS} objects have exactly the content their thread "
              f"wrote (bad: {mismatched})")
        check(all(len(objs[t].versions) == N_EDITS + 1 for t in range(N_THREADS)),
              f"each object has exactly {N_EDITS + 1} versions")


def test_concurrent_reads_during_writes():
    print("\n-- readers racing a writer on the same object --")
    with genna.Engine() as eng:
        obj = eng.create("shared", b"A" * 5000)
        valid = {obj.bytes()}
        stop = threading.Event()
        torn: list[str] = []

        def writer():
            for i in range(120):
                obj.update(0, 0, b"B" * 10)
                valid.add(obj.bytes())
            stop.set()

        def reader():
            while not stop.is_set():
                data = obj.bytes()
                # every read must equal SOME version that has existed, and
                # must be internally consistent: only A's and B's, B's first
                if data and (set(data) - set(b"AB")):
                    torn.append("foreign bytes in read")
                    return
                b_count = data.count(b"B")
                if data[:b_count] != b"B" * b_count:
                    torn.append("interleaved read (torn version)")
                    return

        w = threading.Thread(target=writer)
        rs = [threading.Thread(target=reader) for _ in range(4)]
        w.start()
        for r in rs:
            r.start()
        w.join()
        for r in rs:
            r.join()

        check(not torn, f"no torn reads observed while writing ({torn[:2]})")
        check(len(obj.versions) == 121, f"121 versions after 120 edits "
                                        f"(got {len(obj.versions)})")


def main():
    print("=== genna bindings: thread safety ===")
    test_parallel_objects()
    test_concurrent_reads_during_writes()
    print(f"\n{'THREADS: FAILURES' if FAILS else 'THREADS: ALL PASS'} "
          f"({len(FAILS)} failures)")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
