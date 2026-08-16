"""Keep every version of a document, and go back to any of them.

Run it:  python examples/time_travel.py

No benchmarks, no comparisons, nothing to install beyond `pip install genna`.
It writes a store to a temporary directory and deletes it on the way out.
"""

import os
import shutil
import tempfile

import genna


def main(argv=None) -> int:
    """Run the tour. Entry point for the `genna-example` console script."""

    workdir = tempfile.mkdtemp(prefix="genna-time-travel-")
    store_path = os.path.join(workdir, "notebook.genna")

    print("A notebook that remembers every draft")
    print("=" * 60)

    # ---------------------------------------------------------------- write ---
    # An engine holds named objects. Each object keeps its own full history, so
    # "version 3 of the notes" is a thing you can ask for later.
    engine = genna.Engine()

    draft = (
        "The kettle boils at ten past six.\n"
        "\n"
        "Outside, the street is still dark, and the only sound is the\n"
        "milk float three doors down. I write the first sentence badly,\n"
        "on purpose, so there is something to fix.\n"
    )
    notes = engine.create("notes", draft)
    engine.save(store_path)

    print(f"\nWrote the first draft: {len(notes)} bytes, version 0.")
    print("-" * 60)
    print(notes.bytes().decode())
    print("-" * 60)

    # ----------------------------------------------------------------- edit ---
    # Every edit appends a NEW version. Nothing is overwritten, so no edit can
    # lose what came before it.

    # 1. fix the deliberately bad sentence
    bad = draft.index("I write the first sentence badly,")
    end = draft.index("so there is something to fix.\n") + len("so there is something to fix.\n")
    notes.update(bad, end - bad, "I write until the light changes.\n")
    print(f"\nEdit 1 - replaced the closing lines.        now version "
          f"{len(notes.versions) - 1}, {len(notes)} bytes")

    # 2. add a title at the very top
    notes.insert(0, "MORNING PAGES\n\n")
    print(f"Edit 2 - added a title at the top.          now version "
          f"{len(notes.versions) - 1}, {len(notes)} bytes")

    # 3. append a line at the end
    notes.append("\nTomorrow: start earlier.\n")
    print(f"Edit 3 - appended a closing line.           now version "
          f"{len(notes.versions) - 1}, {len(notes)} bytes")

    print(f"\nThe notebook now has {len(notes.versions)} versions and it never "
          f"rewrote the file.")

    # ------------------------------------------------------- read the past ---
    # Version 0 is still there, exactly as it was typed. Not a reconstruction
    # from a chain of diffs -- the original bytes.
    original = notes.versions[0].bytes().decode()
    print("\nHere is version 0 again, straight out of the store:")
    print("-" * 60)
    print(original)
    print("-" * 60)
    print(f"Identical to what was written? {original == draft}")

    # --------------------------------------------------------------- diff ----
    # Ask which parts changed between two versions without reading either of
    # them. Genna answers from the shape of the tree: an untouched region is
    # literally the same node in both versions, so this is a pointer comparison.
    #
    # It is a FILTER, not an answer. It never misses a real change, but it can
    # over-report: the unit of sharing is a chunk, so a range that merely shares
    # a chunk with an edited one comes back as "changed". So use it to skip the
    # work, then confirm the survivors by reading them. That is the intended
    # shape of the API and it is worth learning early.
    print("\nWhat changed between version 2 and version 3?")
    v2, v3 = notes.versions[2], notes.versions[3]
    window = 64
    skipped = 0
    for offset in range(0, max(len(v2), len(v3)), window):
        if not notes.range_changed(2, 3, offset, window):
            skipped += 1
            continue                      # provably identical: never read it
        before = v2.read(offset, window)
        after = v3.read(offset, window)
        if before == after:
            continue                      # the filter was conservative here
        preview = after.decode("utf-8", "replace").replace("\n", " ").strip()
        print(f"  bytes {offset:>4}-{offset + window:<4} really changed:  "
              f"{preview[:40]}...")
    print(f"  ({skipped} windows were skipped without reading a single byte)")

    # ------------------------------------------------------------ rollback ---
    # Going back is not a restore from a backup. It appends the old state as a
    # new version, so the journey back is itself part of the history.
    print("\nRolling back to version 1...")
    notes.rollback(1)
    back = notes.bytes().decode()
    same_as_v1 = back == notes.versions[1].bytes().decode()
    print(f"  content now matches version 1 exactly:    {same_as_v1}")
    print(f"  and the history still holds every step:   {len(notes.versions)} versions")

    # ------------------------------------------------------------- reopen ----
    # Close it, forget it, open it again from disk.
    engine.save(store_path)
    engine.close()
    print(f"\nClosed the store. Reopening {os.path.basename(store_path)} from disk...")

    reopened = genna.open_store(store_path)
    again = reopened["notes"]
    print(f"  versions recovered:                       {len(again.versions)}")
    print(f"  version 0 still byte-identical:           "
          f"{again.versions[0].bytes().decode() == draft}")
    print(f"  and it is still writable:                 ", end="")
    again.append("\nPostscript: it survived a round trip.\n")
    print(f"version {len(again.versions) - 1} added")
    reopened.close()

    shutil.rmtree(workdir, ignore_errors=True)
    print("\nThat is the whole idea: edit freely, and nothing you wrote is gone.")

    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
