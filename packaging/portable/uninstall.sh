#!/bin/sh
# OpenMonster portable uninstaller
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "HATA: sudo ile calistirin: sudo ./uninstall.sh" >&2
    exit 1
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now monster-ctrl-fanprofile.service >/dev/null 2>&1 || true
fi

rm -f /usr/bin/monsterctl /usr/bin/openmonster
rm -rf /usr/share/openmonster
rm -f /usr/share/applications/openmonster.desktop
rm -f /usr/share/icons/hicolor/scalable/apps/openmonster.svg
for s in 16 24 32 48 64 128 256 512; do
    rm -f "/usr/share/icons/hicolor/${s}x${s}/apps/openmonster.png"
done
rm -rf /usr/share/licenses/openmonster
rm -f /etc/modprobe.d/monster-ctrl.conf
rm -f /etc/modules-load.d/monster-ctrl.load
rm -f /etc/systemd/system/monster-ctrl-fanprofile.service

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || true
fi

echo "OpenMonster kaldirildi."
