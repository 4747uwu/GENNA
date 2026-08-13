#!/usr/bin/env bash
# The Makefile is the documented entry point; make sure its targets still parse
# and name real files after the persistence changes.
source "$(dirname "$0")/env.sh"
for t in test bench persist fuzz all; do
  echo "===== make -n $t ====="
  make -n "$t" CC=gcc 2>&1 | head -6
  echo
done
