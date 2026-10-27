#!/bin/sh
# Builds flowfwd and copies flowfwd.conf next to it; the binary only reads the config from its own directory.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="$ROOT/build"

echo "[1/2] building (warnings are errors)"
make -C "$ROOT" --no-print-directory

echo "[2/2] copying flowfwd.conf next to the binary"
cp "$ROOT/flowfwd.conf" "$BIN_DIR/flowfwd.conf"

echo "done: $BIN_DIR/flowfwd"
