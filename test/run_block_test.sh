#!/bin/sh
# Extract the --block helpers (clampBlock / blockBytes) from ftpsrv_engine.inc
# (so the test can never go stale) and check the accepted range.
set -e
cd "$(dirname "$0")"
SRC=../ftpsrv_engine.inc
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
awk '/^\/\/ read\/write block: 16 KB/{f=1} f&&/^\/\/ keep the data-port range/{exit} f' "$SRC" > "$TMP/block_engine.inc"
echo "extracted $(wc -l < "$TMP/block_engine.inc") lines of block helpers from $SRC"
cp block_test.cpp "$TMP/block_test.cpp"
g++ -O1 -std=c++17 -Wall -o "$TMP/block_test" "$TMP/block_test.cpp"
"$TMP/block_test"