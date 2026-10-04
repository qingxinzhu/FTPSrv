#!/bin/sh
# Extract the live-progress engine from ftpsrv.cpp (so the test can never go stale),
# run it against a fake ANSI terminal and assert the drawing rules.
#
#   sh run_test.sh        (/sdcard and other FUSE mounts have no exec bit,
#                          so call it with sh if ./run_test.sh is refused)
#
# Needs a host g++ (any platform) - the test does not touch winsock.
# Build + run happen in a temp dir, so a noexec project mount is fine.
set -e
cd "$(dirname "$0")"

SRC=../ftpsrv.cpp

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

awk '/#define PROG_SLOTS/{f=1} f&&/^\/\/ logging/{exit} f' "$SRC" > "$TMP/prog_engine.inc"
echo "extracted $(wc -l < "$TMP/prog_engine.inc") lines of progress engine from $SRC"
awk '/^\/\/ live-transfer accounting/{f=1} f{print} f&&/^\/\/ end of live-transfer accounting/{exit}' \
    ../ftpsrv_engine.inc > "$TMP/xfer_engine.inc"
echo "extracted $(wc -l < "$TMP/xfer_engine.inc") lines of transfer accounting from ../ftpsrv_engine.inc"

cp progress_test.cpp "$TMP/progress_test.cpp"
g++ -O1 -std=c++17 -Wall -o "$TMP/progress_test" "$TMP/progress_test.cpp"
"$TMP/progress_test"