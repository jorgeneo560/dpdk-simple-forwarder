#!/bin/sh
# Builds flowfwd and copies flowfwd.conf next to it; the binary only reads the config from its own directory.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
BIN_DIR="$ROOT/build"

make -C "$ROOT"

cp "$ROOT/flowfwd.conf" "$BIN_DIR/flowfwd.conf"
echo "built $BIN_DIR/flowfwd and copied flowfwd.conf next to it"
