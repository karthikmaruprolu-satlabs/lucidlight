#!/bin/sh
# Build the app. All intermediate objects/moc/qrc files live in build/app;
# only the finished binary is placed at build/lucidlight.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DIR="$ROOT/build/app"
mkdir -p "$DIR"
cd "$DIR"

qmake6 "$ROOT/lucidlight.pro" >/dev/null
make -j"$(nproc)"
cp -f "$DIR/lucidlight" "$ROOT/build/lucidlight"

echo "Built $ROOT/build/lucidlight"
