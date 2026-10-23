#!/usr/bin/env bash
# Fedora/RHEL uyumlu .rpm paketi uretir (fedora:41 konteynerinde derler).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/dist"
VERSION="$(sed -n 's/^pkgver=//p' "$ROOT/packaging/PKGBUILD")"
IMAGE="${RPM_IMAGE:-docker.io/library/fedora:41}"
DIST="${RPM_DIST:-.fc41}"
ENGINE=""
for e in podman docker; do
    if command -v "$e" >/dev/null 2>&1; then ENGINE="$e"; break; fi
done
if [ -z "$ENGINE" ]; then
    echo "HATA: podman veya docker bulunamadi." >&2
    exit 1
fi
mkdir -p "$OUT"

echo "==> .rpm derleniyor ($IMAGE)"
"$ENGINE" run --rm \
    -v "$ROOT":/src:ro \
    -v "$OUT":/out \
    -e VERSION="$VERSION" \
    -e DIST="$DIST" \
    "$IMAGE" bash -c '
        set -e
        dnf install -y -q gcc make rpm-build systemd-rpm-macros >/dev/null
        bash /src/packaging/rpm/build.sh
    '

ls -lh "$OUT"/*.rpm
