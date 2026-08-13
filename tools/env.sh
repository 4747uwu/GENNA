#!/usr/bin/env bash
# Shared environment for every build/test script in this repo.
#
# The repo root is derived from this file's own location rather than
# hardcoded: these scripts previously assumed /d/website/devops/GENNA, so a
# clone anywhere else silently ran against the wrong tree or failed at `cd`.
GENNA="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
export GENNA

# MSYS2's mingw64 toolchain first when it exists (this repo is developed on
# Windows, and gcc there is not otherwise on PATH). On Linux and macOS the
# directory is absent and the system compiler is used as-is.
if [ -d /mingw64/bin ]; then
  export PATH=/mingw64/bin:/usr/bin:$PATH
fi

cd "$GENNA" || exit 1
