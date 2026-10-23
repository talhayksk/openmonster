#!/bin/bash
# Fedora RPM paketi uretimi (fedora:41 konteyneri icinde calisir).
# Host tarafindan cagrilir: scripts/build-rpm.sh
set -euo pipefail

SRC="${SRC:-/src}"
OUT="${OUT:-/out}"
VERSION="${VERSION:-1.0.0}"
DIST="${DIST:-.fc41}"

BUILD=/tmp/openmonster-rpm
rm -rf "$BUILD"
mkdir -p "$BUILD/rpmbuild/SOURCES" "$BUILD/rpmbuild/SPECS" \
    "$BUILD/rpmbuild/BUILD" "$BUILD/rpmbuild/RPMS" "$BUILD/rpmbuild/SRPMS"
mkdir -p "$BUILD/openmonster-$VERSION"

tar -C "$SRC" \
    --exclude='./.git' \
    --exclude='./dist' \
    --exclude='./pkg' \
    --exclude='./monsterctl' \
    --exclude='./__pycache__' \
    --exclude='*/__pycache__' \
    --exclude='*.pyc' \
    --exclude='*.pkg.tar.zst' \
    -cf - . | tar -C "$BUILD/openmonster-$VERSION" -xf -

tar -C "$BUILD" -czf "$BUILD/rpmbuild/SOURCES/openmonster-$VERSION.tar.gz" \
    "openmonster-$VERSION"

SPEC="$BUILD/openmonster.spec"
sed "s/^Version:.*/Version:        $VERSION/" \
    "$SRC/packaging/rpm/openmonster.spec" > "$SPEC"

rpmbuild -bb "$SPEC" \
    --define "_topdir $BUILD/rpmbuild" \
    --define "dist $DIST"

mkdir -p "$OUT"
cp "$BUILD"/rpmbuild/RPMS/*/*.rpm "$OUT/"
echo "RPM hazir:"
ls -lh "$OUT"/*.rpm
