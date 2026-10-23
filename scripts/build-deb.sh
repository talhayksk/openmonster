#!/usr/bin/env bash
# Debian/Ubuntu .deb paketi uretir (debian:12 konteynerinde derler).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/dist"
VERSION="$(sed -n 's/^pkgver=//p' "$ROOT/packaging/PKGBUILD")"
REL="$(sed -n 's/^pkgrel=//p' "$ROOT/packaging/PKGBUILD")"
IMAGE="${DEB_IMAGE:-docker.io/library/debian:12}"
ENGINE=""
for e in podman docker; do
    if command -v "$e" >/dev/null 2>&1; then ENGINE="$e"; break; fi
done
if [ -z "$ENGINE" ]; then
    echo "HATA: podman veya docker bulunamadi." >&2
    exit 1
fi
mkdir -p "$OUT"

echo "==> .deb derleniyor ($IMAGE)"
"$ENGINE" run --rm \
    -v "$ROOT":/src:ro \
    -v "$OUT":/out \
    -e VERSION="$VERSION" \
    -e REL="$REL" \
    -e ARCH=amd64 \
    "$IMAGE" bash -c '
        set -e
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends gcc make libc6-dev >/dev/null
        bash /src/packaging/debian/build.sh
    '

ls -lh "$OUT"/*.deb
