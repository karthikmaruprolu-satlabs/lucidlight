#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
"$ROOT/build.sh"
cd "$ROOT/pkgbuild"
exec makepkg -fsi "$@"
