#!/usr/bin/env bash
# AUR'a openmonster paketini gonderir/yeniler.
# Onkosul: AUR hesabi + ~/.ssh/aur (veya ssh-agent) ile aur@aur.archlinux.org erisimi.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="openmonster"
SRC_DIR="$ROOT/packaging/aur"
WORK="${XDG_CACHE_HOME:-$HOME/.cache}/$PKG-aur"

if [ ! -f "$SRC_DIR/PKGBUILD" ] || [ ! -f "$SRC_DIR/.SRCINFO" ]; then
    echo "HATA: $SRC_DIR icinde PKGBUILD/.SRCINFO yok." >&2
    exit 1
fi

echo "==> AUR SSH erisimi kontrol ediliyor"
if ! ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new \
        -T aur@aur.archlinux.org >/dev/null 2>&1; then
    cat >&2 <<'EOF'
AUR SSH erisimi yok.

  1) ssh-keygen -t ed25519 -C "aur" -f ~/.ssh/aur
  2) cat ~/.ssh/aur.pub  -> https://aur.archlinux.org/account/ adresine ekleyin
  3) ssh -T aur@aur.archlinux.org  ile test edin
EOF
    exit 1
fi

if [ -d "$WORK/.git" ]; then
    echo "==> Mevcut AUR calisma kopyasi guncelleniyor"
    git -C "$WORK" fetch origin || true
    if git -C "$WORK" rev-parse --verify -q origin/master >/dev/null 2>&1; then
        git -C "$WORK" rebase origin/master
    fi
else
    echo "==> AUR deposu klonlaniyor"
    mkdir -p "$(dirname "$WORK")"
    git clone "ssh://aur@aur.archlinux.org/$PKG.git" "$WORK"
fi

cp "$SRC_DIR/PKGBUILD" "$SRC_DIR/.SRCINFO" "$SRC_DIR/openmonster.install" "$WORK/"

# .SRCINFO'yu her zaman kaynaktan yeniden uret
( cd "$WORK" && makepkg --printsrcinfo > .SRCINFO )

cd "$WORK"
git add PKGBUILD .SRCINFO openmonster.install
if ! git diff --cached --quiet; then
    VERSION="$(sed -n 's/^pkgver=//p' PKGBUILD)"
    REL="$(sed -n 's/^pkgrel=//p' PKGBUILD)"
    git commit -m "upgpkg: $PKG $VERSION-$REL"
fi

git push -u origin HEAD:master
echo "==> AUR guncellendi: https://aur.archlinux.org/packages/$PKG"
