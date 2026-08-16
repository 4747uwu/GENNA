"""Keep every version of a document, and go back to any of them.

Run it from a checkout:   python examples/time_travel.py
Or, after `pip install genna`, from anywhere:   genna-example

The code lives in `genna/example_time_travel.py`, INSIDE the package, so that
`pip install genna` ships it. Keeping the real thing here and a copy there
would have meant two versions of the same file drifting apart; this way the
README's instructions are true for people who cloned and for people who
installed, and there is one implementation.
"""
import sys

from genna.example_time_travel import main

if __name__ == "__main__":
    sys.exit(main())
