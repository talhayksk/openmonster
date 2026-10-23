#!/usr/bin/env bash
# OpenMonster Arch paketi uretici.
# Kullanim: ./scripts/build-package.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="$(sed -n 's/^pkgver=//p' packaging/PKGBUILD)"
REL="$(sed -n 's/^pkgrel=//p' packaging/PKGBUILD)"
NAME="openmonster-$VERSION"

echo "==> monsterctl derleniyor"
make

WORK="$(mktemp -d "${TMPDIR:-/tmp}/openmonster-build.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

SRC="$WORK/$NAME"
mkdir -p "$SRC"
echo "==> kaynak arsivi hazirlaniyor"
tar -C "$ROOT" \
    --exclude='./.git' \
    --exclude='./dist' \
    --exclude='./pkg' \
    --exclude='./monsterctl' \
    --exclude='./__pycache__' \
    --exclude='*/__pycache__' \
    --exclude='*.pyc' \
    --exclude='*.pkg.tar.zst' \
    -cf - . | tar -xf - -C "$SRC"

tar -czf "$WORK/$NAME.tar.gz" -C "$WORK" "$NAME"
cp packaging/PKGBUILD packaging/openmonster.install "$WORK/"

echo "==> makepkg"
( cd "$WORK" && makepkg -f --nodeps --noconfirm )

mkdir -p "$ROOT/dist"
PKG="$(find "$WORK" -maxdepth 1 -name '*.pkg.tar.zst' -print -quit)"
if [ -z "$PKG" ]; then
    echo "HATA: paket uretilmedi" >&2
    exit 1
fi
cp "$PKG" "$ROOT/dist/"
BASE="$(basename "$PKG")"
( cd "$ROOT/dist" && sha256sum "$BASE" > "$BASE.sha256" )

echo
echo "Hazir: dist/$BASE"
echo "Kurulum: sudo pacman -U dist/$BASE"
