#!/bin/bash
# Tum Linux dagitimlari icin portable tar.gz uretimi (debian:12 konteyneri).
# C binary statik derlenir; Python/PyQt6 sistemde bulunmalidir.
set -euo pipefail

SRC="${SRC:-/src}"
OUT="${OUT:-/out}"
VERSION="${VERSION:-1.1.0}"

BUILD=/tmp/openmonster-portable
rm -rf "$BUILD"
mkdir -p "$BUILD/src"
tar -C "$SRC" \
    --exclude='./.git' \
    --exclude='./dist' \
    --exclude='./pkg' \
    --exclude='./monsterctl' \
    --exclude='./__pycache__' \
    --exclude='*/__pycache__' \
    --exclude='*.pyc' \
    --exclude='*.pkg.tar.zst' \
    -cf - . | tar -C "$BUILD/src" -xf -

cd "$BUILD/src"
make clean >/dev/null 2>&1 || true
make CFLAGS="-Wall -Wextra -O2 -std=gnu11 -static"

STAGE="$BUILD/stage"
mkdir -p "$STAGE"
make install DESTDIR="$STAGE" PREFIX=/usr

NAME="openmonster-$VERSION-portable"
ROOT="$BUILD/$NAME"
mkdir -p "$ROOT"
mv "$STAGE" "$ROOT/payload"
cp packaging/portable/install.sh packaging/portable/uninstall.sh "$ROOT/"
cp README.md LICENSE "$ROOT/"
chmod 0755 "$ROOT/install.sh" "$ROOT/uninstall.sh"

tar -C "$BUILD" -czf "$OUT/$NAME-x86_64.tar.gz" "$NAME"
echo "Portable arsiv hazir: $OUT/$NAME-x86_64.tar.gz"
