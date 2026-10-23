#!/bin/sh
# OpenMonster portable installer (Debian/Ubuntu/Fedora/openSUSE/Arch...)
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "HATA: Bu kurulum sistem dosyalarina yaziyor. sudo ile calistirin:" >&2
    echo "  sudo ./install.sh" >&2
    exit 1
fi

HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$HERE"

if [ ! -d payload ]; then
    echo "HATA: payload dizini bulunamadi." >&2
    exit 1
fi

echo "==> OpenMonster dosyalari / altina kopyalaniyor"
cp -a payload/. /

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
  CLI:      sudo monsterctl status
  Fan profili servisi (istege bagli):
      sudo systemctl enable --now monster-ctrl-fanprofile.service

Gereksinimler:
  - Python 3 + PyQt6
  - PolicyKit (pkexec)
  - kmod + systemd
  - Fan/batarya icin acpi_call modulu
  - NVIDIA verileri icin NVIDIA surucusu

Kaldirmak icin: sudo ./uninstall.sh
NOTE
