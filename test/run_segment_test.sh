#!/bin/sh
# Extract the concurrent-upload registry from ftpsrv_engine.inc (so the test can
# never go stale) and check the rules a multi-threaded client depends on.
set -e
cd "$(dirname "$0")"
SRC=../ftpsrv_engine.inc
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
awk '/^\/\/ concurrent upload registry/{f=1} f&&/^static bool isReadonlySession/{exit} f' "$SRC" > "$TMP/segment_engine.inc"
echo "extracted $(wc -l < "$TMP/segment_engine.inc") lines of upload registry from $SRC"
cp segment_test.cpp "$TMP/segment_test.cpp"
g++ -O1 -std=c++17 -Wall -o "$TMP/segment_test" "$TMP/segment_test.cpp"
"$TMP/segment_test"