#!/bin/sh
# Extract the long-path helpers from ftpsrv_engine.inc (so the test can never
# go stale) and check the prefix rules.
set -e
cd "$(dirname "$0")"
SRC=../ftpsrv_engine.inc
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
awk '/^static const wchar_t kBs/{f=1} f&&/^}/{print; exit} f' "$SRC" > "$TMP/longpath_engine.inc"
echo "extracted $(wc -l < "$TMP/longpath_engine.inc") lines of long-path helpers from $SRC"
cp longpath_test.cpp "$TMP/longpath_test.cpp"
g++ -O1 -std=c++17 -Wall -Wextra -o "$TMP/longpath_test" "$TMP/longpath_test.cpp"
"$TMP/longpath_test"
