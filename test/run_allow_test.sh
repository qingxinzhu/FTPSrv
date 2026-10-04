#!/bin/sh
# Extract the IP allow-list maths (ip4ToU32 / bitsToMask / allowMatchNet) from
# ftpsrv_engine.inc - the block contains no Windows API, so it can be compiled
# and unit tested on its own.
set -e
cd "$(dirname "$0")"
SRC=../ftpsrv_engine.inc
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
awk '/^static std::vector<unsigned int> g_allowNet;/{f=1} f&&/^static void allowParse/{exit} f' "$SRC" > "$TMP/allow_engine.inc"
echo "extracted $(wc -l < "$TMP/allow_engine.inc") lines of allow-list logic from $SRC"
cp allow_test.cpp "$TMP/allow_test.cpp"
g++ -O1 -std=c++17 -Wall -o "$TMP/allow_test" "$TMP/allow_test.cpp"
"$TMP/allow_test"