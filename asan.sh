#!/bin/sh
# Rebuild Lucidlight with AddressSanitizer into build/asan/ and run it.
# Usage: bin/asan            (build only)
#        bin/asan <args>     (build, then run ./build/asan/lucidlight <args>)
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DIR="$ROOT/build/asan"
mkdir -p "$DIR"
cd "$DIR"
qmake6 "$ROOT/lucidlight.pro" "CONFIG+=debug sanitizer address" >/dev/null
make -j"$(nproc)" >/dev/null
echo "ASan build ready: $DIR/lucidlight"
if [ $# -gt 0 ]; then
    exec "$DIR/lucidlight" "$@"
fi
