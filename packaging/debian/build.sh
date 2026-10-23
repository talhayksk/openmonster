#!/bin/bash
# Debian paketi uretimi (debian:12 konteyneri icinde calisir).
# Host tarafindan cagrilir: scripts/build-deb.sh
set -euo pipefail

SRC="${SRC:-/src}"
OUT="${OUT:-/out}"
VERSION="${VERSION:-1.0.0}"
REL="${REL:-1}"
ARCH="${ARCH:-amd64}"

BUILD=/tmp/openmonster-deb
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
make

PKG="$BUILD/pkg"
mkdir -p "$PKG"
make install DESTDIR="$PKG" PREFIX=/usr

DOC="$PKG/usr/share/doc/openmonster"
mkdir -p "$DOC" "$PKG/DEBIAN"
cp README.md "$DOC/README.md"

cat > "$DOC/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: openmonster
Source: https://github.com/talhayksk/openmonster

Files: *
Copyright: 2026 Talha Yuksek <talha.yuksek@hotmail.com>
License: GPL-3.0-or-later
EOF
sed 's/^/ /' LICENSE >> "$DOC/copyright"

cat > "$DOC/changelog.Debian" <<EOF
openmonster ($VERSION-$REL) unstable; urgency=medium

  * Initial release.

 -- Talha Yuksek <talha.yuksek@hotmail.com>  Fri, 23 Oct 2026 00:00:00 +0300
EOF
gzip -9 -n "$DOC/changelog.Debian"

SIZE=$(du -sk "$PKG/usr" | cut -f1)
cat > "$PKG/DEBIAN/control" <<EOF
Package: openmonster
Version: $VERSION-$REL
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: Talha Yuksek <talha.yuksek@hotmail.com>
Installed-Size: $SIZE
Depends: python3, python3-pyqt6, policykit-1 | pkexec, kmod, systemd
Recommends: acpi-call-dkms, nvidia-driver
Homepage: https://github.com/talhayksk/openmonster
Description: Monster/Uniwill laptop control center for Linux
 Fan curve, keyboard RGB, battery charge limit, CPU power limits and system
 cleanup for Monster/Abra (Uniwill/Tongfang ODM) laptops.
 .
 The GUI uses a one-time PolicyKit prompt and then talks to a privileged
 helper process. The CLI is provided as monsterctl.
EOF

cat > "$PKG/DEBIAN/conffiles" <<'EOF'
/etc/modprobe.d/monster-ctrl.conf
/etc/modules-load.d/monster-ctrl.load
EOF

cat > "$PKG/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

cat <<'NOTE'
OpenMonster kuruldu.

  Uygulama: openmonster
  CLI:      monsterctl (sudo monsterctl status)
  Fan profili servisi (istege bagli):
      sudo systemctl enable --now monster-ctrl-fanprofile.service

  Fan/batarya icin acpi_call modulu gerekir. NVIDIA verileri icin
  nvidia surucusu onerilir.
NOTE
exit 0
EOF

cat > "$PKG/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v systemctl >/dev/null 2>&1; then
        systemctl disable --now monster-ctrl-fanprofile.service >/dev/null 2>&1 || true
    fi
fi
exit 0
EOF

cat > "$PKG/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi
exit 0
EOF

chmod 0755 "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/prerm" "$PKG/DEBIAN/postrm"

( cd "$PKG" && find . -type f ! -path './DEBIAN/*' -print0 |
    sort -z | xargs -0 md5sum | sed 's|  \./|  |' > DEBIAN/md5sums )

mkdir -p "$OUT"
dpkg-deb --build --root-owner-group "$PKG" \
    "$OUT/openmonster_${VERSION}-${REL}_${ARCH}.deb"
echo "DEB hazir: $OUT/openmonster_${VERSION}-${REL}_${ARCH}.deb"
