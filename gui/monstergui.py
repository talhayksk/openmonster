#!/usr/bin/env python3
# OpenMonster GUI v1.0.0 (PyQt6): modern koyu tema + ring gostergeli panel
# - Acilista BIR KEZ pkexec sifre penceresi; sonraki komutlar sifresiz (monsterctl helper)
# - Root islemleri proje icindeki guncel binary ile yapilir.
# - Sistem tepsisi: arka planda temizlik / fan / RGB hizli islemleri.
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import re
import glob
import json
import math
import time
import random
import shutil
import subprocess
import sys

from PyQt6.QtCore import Qt, QTimer, QProcess, QRectF, QSize
from PyQt6.QtGui import QFont, QColor, QPainter, QPen, QIcon, QPixmap, QAction
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QFrame, QLabel, QPushButton,
    QVBoxLayout, QHBoxLayout, QGridLayout, QStackedWidget, QSlider,
    QSpinBox, QPlainTextEdit, QButtonGroup, QSizePolicy, QColorDialog,
    QScrollArea, QComboBox, QCheckBox, QSystemTrayIcon, QMenu, QDialog,
    QLineEdit, QFormLayout, QDialogButtonBox, QMessageBox,
)

APP_NAME = "OpenMonster"
APP_ID = "openmonster"
APP_VERSION = "1.0.0"
APP_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_DIR = os.path.join(
    os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), APP_ID)
PROFILES_FILE = os.path.join(CONFIG_DIR, "profiles.json")
SETTINGS_FILE = os.path.join(CONFIG_DIR, "settings.json")
AUTOSTART_FILE = os.path.join(
    os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")),
    "autostart", "openmonster.desktop")

HWMON_DIR = "/sys/class/hwmon"
BAT = "/sys/class/power_supply/BAT0"
RAPL = "/sys/class/powercap/intel-rapl:0"
PROJECT_BIN = os.path.join(APP_DIR, "monsterctl")
INSTALLED_BIN = "/usr/local/bin/monsterctl"
PACKAGED_BIN = "/usr/bin/monsterctl"


def _project_or_system(*parts):
    local = os.path.join(APP_DIR, *parts)
    if os.path.exists(local):
        return local
    return os.path.join("/usr/share", APP_ID, *parts)


def app_icon():
    """Logo: once yerel assets, sonra kurulu ikon temasi."""
    for p in (_project_or_system("assets", "icons", "openmonster-256.png"),
              "/usr/share/icons/hicolor/256x256/apps/openmonster.png",
              _project_or_system("assets", "openmonster.svg"),
              "/usr/share/icons/hicolor/scalable/apps/openmonster.svg"):
        if os.path.exists(p):
            return QIcon(p)
    return QIcon.fromTheme(APP_ID)

C_BG = "#0b0f16"
C_SIDEBAR = "#0e131c"
C_CARD = "#121826"
C_BORDER = "#1e2736"
C_TEXT = "#e6edf7"
C_MUTED = "#8b98ab"
C_ACCENT = "#4f8cff"
C_OK = "#34d399"
C_WARN = "#fbbf24"
C_HOT = "#f87171"
C_TRACK = "#1c2534"


def monsterctl_bin():
    if os.access(PROJECT_BIN, os.X_OK):
        return PROJECT_BIN
    for p in (PACKAGED_BIN, INSTALLED_BIN):
        if os.access(p, os.X_OK):
            return p
    return "monsterctl"


def rread(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def uniwill_hwmon():
    try:
        for d in sorted(os.listdir(HWMON_DIR)):
            if rread(os.path.join(HWMON_DIR, d, "name")) == "uniwill":
                return os.path.join(HWMON_DIR, d)
    except OSError:
        pass
    return None


def temp_color(c):
    if c is None:
        return C_MUTED
    if c >= 80:
        return C_HOT
    if c >= 60:
        return C_WARN
    return C_OK


def _num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def nvidia_stats():
    """NVIDIA GPU: ad, yuk %, bellek, sicaklik, guc, cekirdek frekansi."""
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,utilization.gpu,memory.used,memory.total,"
             "temperature.gpu,power.draw,clocks.sm", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=1.5)
        line = out.stdout.strip().splitlines()[0]
        name, util, mused, mtotal, temp, power, clock = [x.strip() for x in line.split(",")]
        return {"name": name, "util": _num(util), "mem_used": _num(mused),
                "mem_total": _num(mtotal), "temp": _num(temp), "power": _num(power),
                "clock": _num(clock)}
    except (subprocess.SubprocessError, OSError, IndexError, ValueError):
        return None


def cpu_freq_ghz():
    """Cekirdeklerin anlik en yuksek frekansi (GHz)."""
    vals = []
    try:
        for d in os.listdir("/sys/devices/system/cpu"):
            if not re.fullmatch(r"cpu\d+", d):
                continue
            v = rread(f"/sys/devices/system/cpu/{d}/cpufreq/scaling_cur_freq")
            if v:
                try:
                    vals.append(int(v))
                except ValueError:
                    pass
    except OSError:
        pass
    return max(vals) / 1e6 if vals else None


# ---------- disk ----------
DISK = "/dev/nvme0n1p7"
_disk_prev = None


def human(n):
    n = float(n)
    for u in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or u == "TB":
            return f"{n:.0f} {u}" if u != "GB" else f"{n:.1f} {u}"
        n /= 1024


def human_bytes(size):
    size = float(max(0, size))
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} TB"


def disk_info():
    global _disk_prev
    info = {"model": "", "temp": None, "usage": None, "free_gb": None,
            "total_gb": None, "read_mbs": None, "write_mbs": None}
    try:
        info["model"] = (rread("/sys/class/nvme/nvme0/model") or "NVMe").strip()
    except OSError:
        info["model"] = "Disk"
    for p in glob.glob("/sys/class/nvme/nvme0/hwmon*/temp1_input"):
        v = rread(p)
        if v:
            info["temp"] = int(v) / 1000
            break
    try:
        st = os.statvfs("/")
        total = st.f_blocks * st.f_frsize
        free = st.f_bavail * st.f_frsize
        used = total - st.f_bfree * st.f_frsize
        info["usage"] = used * 100.0 / total if total else None
        info["free_gb"] = free / 1e9
        info["total_gb"] = total / 1e9
    except OSError:
        pass
    try:
        with open("/proc/diskstats") as f:
            for line in f:
                parts = line.split()
                if len(parts) > 9 and parts[2] == "nvme0n1":
                    rs, ws = int(parts[5]), int(parts[9])
                    now = time.monotonic()
                    if _disk_prev:
                        dt, prs, pws = _disk_prev
                        if now > dt:
                            info["read_mbs"] = (rs - prs) * 512 / (now - dt) / 1e6
                            info["write_mbs"] = (ws - pws) * 512 / (now - dt) / 1e6
                    _disk_prev = (now, rs, ws)
                    break
    except (OSError, ValueError):
        pass
    return info


def dir_bytes(path, timeout=12):
    if not os.path.exists(path):
        return 0
    try:
        out = subprocess.run(["du", "-sb", path], capture_output=True,
                             text=True, timeout=timeout)
        return int(out.stdout.split()[0])
    except (subprocess.SubprocessError, OSError, ValueError, IndexError):
        return 0


def journal_bytes():
    try:
        out = subprocess.run(["journalctl", "--disk-usage", "--no-pager"],
                             capture_output=True, text=True, timeout=8)
        m = re.search(r"take up\s+([\d.,]+)\s*([KMGT])", out.stdout)
        if not m:
            return 0
        mult = {"K": 1024, "M": 1024 ** 2, "G": 1024 ** 3, "T": 1024 ** 4}[m.group(2)]
        return int(float(m.group(1).replace(",", ".")) * mult)
    except (subprocess.SubprocessError, OSError, ValueError):
        return 0


def rm_children(path):
    freed = 0
    if not os.path.isdir(path):
        return 0
    try:
        names = os.listdir(path)
    except OSError:
        return 0
    for name in names:
        p = os.path.join(path, name)
        try:
            if os.path.isdir(p) and not os.path.islink(p):
                freed += dir_bytes(p)
                shutil.rmtree(p, ignore_errors=True)
            else:
                freed += os.path.getsize(p)
                os.unlink(p)
        except OSError:
            pass
    return freed


def ram_stats():
    """(kullanim %, kullanilan GB, toplam GB) veya None."""
    info = {}
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                k, v = line.split(":", 1)
                info[k] = int(v.split()[0])
    except (OSError, ValueError):
        return None
    total = info.get("MemTotal", 0)
    avail = info.get("MemAvailable", info.get("MemFree", 0))
    if total <= 0:
        return None
    used = total - avail
    return used * 100.0 / total, used / 1024 / 1024, total / 1024 / 1024


def cpu_name_short():
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    name = line.split(":", 1)[1].strip()
                    name = re.sub(r"\s*@\s*[\d.]+GHz", "", name)
                    name = name.replace("Intel(R) ", "").replace("Core(TM) ", "")
                    name = re.sub(r"^\d+(st|nd|rd|th)\s+Gen\s+", "", name)
                    return name.strip()
    except OSError:
        pass
    return ""


def gpu_name_short(name):
    if not name:
        return ""
    for prefix in ("NVIDIA ", "GeForce ", "AMD ", "Radeon "):
        name = name.replace(prefix, "")
    name = name.replace(" Laptop GPU", "").replace(" GPU", "")
    return re.sub(r"\s+", " ", name).strip()


# ---------- kalici ayarlar ----------
DEFAULT_PROFILES = [
    {"name": "Sessiz", "fan": 35, "power": 30, "builtin": True},
    {"name": "Dengeli", "fan": 55, "power": 60, "builtin": True},
    {"name": "Performans", "fan": 85, "power": 100, "builtin": True},
]


def load_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, data):
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        return True
    except OSError:
        return False


def load_profiles():
    data = load_json(PROFILES_FILE, None)
    if not isinstance(data, list) or not data:
        return [dict(p) for p in DEFAULT_PROFILES]
    out = []
    for p in data:
        if not isinstance(p, dict):
            continue
        try:
            name = str(p.get("name", "")).strip()
            fan = max(0, min(100, int(p.get("fan", 50))))
            power = max(5, min(100, int(p.get("power", 60))))
        except (TypeError, ValueError):
            continue
        if name:
            out.append({"name": name, "fan": fan, "power": power,
                        "builtin": bool(p.get("builtin", False))})
    return out or [dict(p) for p in DEFAULT_PROFILES]


def load_settings():
    data = load_json(SETTINGS_FILE, {})
    return data if isinstance(data, dict) else {}


def autostart_enabled():
    return os.path.exists(AUTOSTART_FILE)


def set_autostart(enabled):
    if not enabled:
        try:
            os.unlink(AUTOSTART_FILE)
        except OSError:
            pass
        return True
    os.makedirs(os.path.dirname(AUTOSTART_FILE), exist_ok=True)
    exe = shutil.which("openmonster")
    if exe:
        cmd = f"{exe} --tray"
    else:
        cmd = f"{sys.executable} {os.path.abspath(__file__)} --tray"
    desktop = f"""[Desktop Entry]
Type=Application
Name={APP_NAME}
Comment=Arka planda baslar, fan / RGB / temizlik menusu sistem tepsisinde
Exec={cmd}
Icon={APP_ID}
Terminal=false
X-GNOME-Autostart-enabled=true
"""
    try:
        with open(AUTOSTART_FILE, "w", encoding="utf-8") as f:
            f.write(desktop)
        return True
    except OSError:
        return False


class ProfileDialog(QDialog):
    """Yeni fan profili: ad + fan yuzdesi + guc yuzdesi."""

    def __init__(self, parent=None, profile=None):
        super().__init__(parent)
        self.setWindowTitle("Fan profili")
        self.setModal(True)
        self.setMinimumWidth(340)
        self.name = QLineEdit((profile or {}).get("name", ""))
        self.name.setPlaceholderText("örn. Gece modu")
        self.fan = QSpinBox()
        self.fan.setRange(0, 100)
        self.fan.setSuffix(" %")
        self.fan.setValue(int((profile or {}).get("fan", 50)))
        self.power = QSpinBox()
        self.power.setRange(5, 100)
        self.power.setSuffix(" %")
        self.power.setValue(int((profile or {}).get("power", 60)))
        form = QFormLayout(self)
        form.addRow("Profil adı", self.name)
        form.addRow("Fan hızı", self.fan)
        form.addRow("Güç limiti", self.power)
        note = QLabel("Güç limiti, CPU'nun en yüksek PL değerinin yüzdesidir.")
        note.setObjectName("muted")
        note.setWordWrap(True)
        form.addRow(note)
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok |
            QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._accept)
        buttons.rejected.connect(self.reject)
        form.addRow(buttons)
        self.name.setFocus()

    def _accept(self):
        if not self.name.text().strip():
            QMessageBox.warning(self, "Fan profili", "Profil adı boş olamaz.")
            return
        self.accept()

    def values(self):
        return {"name": self.name.text().strip(),
                "fan": self.fan.value(), "power": self.power.value(),
                "builtin": False}


QSS = f"""
QMainWindow, QWidget#rootRoot {{
    background: {C_BG};
}}
QWidget {{
    color: {C_TEXT};
    font-family: 'Noto Sans', 'DejaVu Sans', sans-serif;
    font-size: 13px;
}}
QScrollArea {{
    background: transparent;
    border: none;
}}
QWidget#pageBody {{
    background: transparent;
}}
QLabel {{
    background: transparent;
}}
QLabel#toast {{
    background: rgba(18, 24, 38, 242);
    border: 1px solid {C_BORDER};
    border-left: 4px solid {C_OK};
    border-radius: 10px;
    padding: 12px 24px;
    font-size: 14px;
    font-weight: 700;
}}
QLabel#toast[ok="false"] {{
    border-left: 4px solid {C_HOT};
}}
QFrame#sidebar {{
    background: {C_SIDEBAR};
    border-right: 1px solid {C_BORDER};
}}
QFrame#card {{
    background: {C_CARD};
    border: 1px solid {C_BORDER};
    border-radius: 14px;
}}
QLabel#brand {{
    font-size: 15px;
    font-weight: 700;
    color: {C_TEXT};
    letter-spacing: 1px;
}}
QLabel#brandsub {{
    font-size: 10px;
    color: {C_MUTED};
    letter-spacing: 2px;
}}
QLabel#pageTitle {{
    font-size: 22px;
    font-weight: 700;
}}
QLabel#pageSub {{
    font-size: 12px;
    color: {C_MUTED};
}}
QLabel#cardTitle {{
    font-size: 11px;
    color: {C_MUTED};
    letter-spacing: 1px;
    font-weight: 600;
}}
QLabel#bigValue {{
    font-size: 26px;
    font-weight: 700;
}}
QLabel#muted {{
    color: {C_MUTED};
    font-size: 11px;
}}
QLabel#pillOk {{
    color: {C_OK};
    background: rgba(52, 211, 153, 0.10);
    border: 1px solid rgba(52, 211, 153, 0.35);
    border-radius: 10px;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 600;
}}
QLabel#pillBad {{
    color: {C_HOT};
    background: rgba(248, 113, 113, 0.10);
    border: 1px solid rgba(248, 113, 113, 0.35);
    border-radius: 10px;
    padding: 4px 10px;
    font-size: 11px;
    font-weight: 600;
}}
QPushButton#nav {{
    text-align: left;
    padding: 11px 16px;
    border: none;
    border-radius: 10px;
    color: {C_MUTED};
    font-size: 13px;
    font-weight: 600;
    background: transparent;
}}
QPushButton#nav:hover {{
    background: #141c29;
    color: {C_TEXT};
}}
QPushButton#nav:checked {{
    background: #16233a;
    color: #9cc2ff;
}}
QPushButton#primary {{
    background: {C_ACCENT};
    color: #ffffff;
    border: none;
    border-radius: 9px;
    padding: 9px 18px;
    font-weight: 600;
}}
QPushButton#primary:hover {{ background: #6a9dff; }}
QPushButton#primary:disabled {{ background: #23314a; color: #6d7c93; }}
QPushButton#ghost {{
    background: transparent;
    color: {C_TEXT};
    border: 1px solid {C_BORDER};
    border-radius: 9px;
    padding: 8px 16px;
    font-weight: 600;
}}
QPushButton#ghost:hover {{ background: #171f2d; border-color: #2b3950; }}
QPushButton#seg {{
    background: #0f1520;
    color: {C_MUTED};
    border: 1px solid {C_BORDER};
    border-radius: 8px;
    padding: 7px 14px;
    font-weight: 600;
}}
QPushButton#segS {{
    background: #0f1520;
    color: {C_MUTED};
    border: 1px solid {C_BORDER};
    border-radius: 8px;
    padding: 5px 9px;
    font-size: 12px;
    font-weight: 600;
}}
QPushButton#segS:hover {{ color: {C_TEXT}; border-color: #2b3950; }}
QComboBox {{
    background: #0f1520;
    color: {C_TEXT};
    border: 1px solid {C_BORDER};
    border-radius: 8px;
    padding: 5px 10px;
    min-width: 90px;
}}
QComboBox QAbstractItemView {{
    background: {C_CARD};
    color: {C_TEXT};
    selection-background-color: #1d2b45;
    border: 1px solid {C_BORDER};
    outline: none;
}}
QComboBox::drop-down {{ border: none; width: 20px; }}
QComboBox QAbstractItemView::item {{ padding: 4px 8px; }}
QCheckBox {{ spacing: 8px; }}
QCheckBox::indicator {{
    width: 16px;
    height: 16px;
    border-radius: 4px;
    border: 1px solid #2b3950;
    background: #0f1520;
}}
QCheckBox::indicator:hover {{ border-color: {C_ACCENT}; }}
QCheckBox::indicator:checked {{ background: {C_ACCENT}; border-color: {C_ACCENT}; }}
QPushButton#seg:hover {{ color: {C_TEXT}; }}
QPushButton#seg:checked {{
    background: #1b2c49;
    color: #9cc2ff;
    border-color: #2f4f7f;
}}
QSlider::groove:horizontal {{
    height: 6px;
    background: {C_TRACK};
    border-radius: 3px;
}}
QSlider::sub-page:horizontal {{
    background: {C_ACCENT};
    border-radius: 3px;
}}
QSlider::handle:horizontal {{
    background: #ffffff;
    width: 16px;
    height: 16px;
    margin: -5px 0;
    border-radius: 8px;
}}
QSpinBox {{
    background: #0f1520;
    border: 1px solid {C_BORDER};
    border-radius: 8px;
    padding: 5px 8px;
    selection-background-color: {C_ACCENT};
}}
QSpinBox:focus {{ border-color: {C_ACCENT}; }}
QPlainTextEdit {{
    background: #070b12;
    border: 1px solid {C_BORDER};
    border-radius: 10px;
    color: #9fd0ff;
    font-family: 'JetBrains Mono', 'DejaVu Sans Mono', monospace;
    font-size: 12px;
    padding: 8px;
}}
QScrollBar:vertical {{
    background: transparent; width: 8px; margin: 2px;
}}
QScrollBar::handle:vertical {{
    background: #26313f; border-radius: 4px; min-height: 24px;
}}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QLineEdit {{
    background: #0f1520;
    border: 1px solid {C_BORDER};
    border-radius: 8px;
    padding: 6px 9px;
    selection-background-color: {C_ACCENT};
}}
QLineEdit:focus {{ border-color: {C_ACCENT}; }}
QMenu {{
    background: {C_CARD};
    color: {C_TEXT};
    border: 1px solid {C_BORDER};
    border-radius: 8px;
    padding: 6px;
}}
QMenu::item {{
    padding: 7px 22px 7px 14px;
    border-radius: 6px;
}}
QMenu::item:selected {{ background: #1d2b45; }}
QMenu::separator {{ height: 1px; background: {C_BORDER}; margin: 5px 8px; }}
"""


class Gauge(QWidget):
    """Yuvarlak (ring) gosterge: deger + alt metin + baslik."""

    def __init__(self, title, unit="", vmax=100.0, parent=None):
        super().__init__(parent)
        self.title = title
        self.unit = unit
        self.vmax = vmax
        self.value = 0.0
        self.text = "-"
        self.sub = ""
        self.color = QColor(C_ACCENT)
        self.setMinimumSize(140, 140)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)

    def set_title(self, title):
        if title != self.title:
            self.title = title
            self.update()

    def set_sub(self, sub):
        if sub != self.sub:
            self.sub = sub
            self.update()

    def set(self, value, text=None, color=None, sub=None):
        if value is not None:
            self.value = max(0.0, min(float(value), self.vmax))
        self.text = text if text is not None else f"{self.value:.0f}"
        if color:
            self.color = QColor(color)
        self.sub = sub if sub is not None else self.sub
        self.update()

    def sizeHint(self):
        return QSize(230, 230)

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setClipRect(self.rect())

        # Halkayi her zaman kartin icine sigdir: kucuk kenardan pay birak.
        side = max(48, min(self.width(), self.height()) - 28)
        rect = QRectF((self.width() - side) / 2.0, (self.height() - side) / 2.0,
                      side, side)

        w = max(7, min(18, side // 14))
        # arka halka
        p.setPen(QPen(QColor(C_TRACK), w, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawArc(rect, 0, 360 * 16)
        # deger yayi (ustten saat yonunun tersine)
        span = int(-360 * 16 * (self.value / self.vmax)) if self.vmax else 0
        if span:
            p.setPen(QPen(self.color, w, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            p.drawArc(rect, 90 * 16, span)

        # buyuk deger - tam ekranda da mantikli bir ust sinirla
        p.setPen(QColor(C_TEXT))
        f = QFont(self.font())
        f.setPixelSize(max(16, min(58, side // 4)))
        f.setWeight(QFont.Weight.DemiBold)
        p.setFont(f)
        p.drawText(QRectF(rect.left(), rect.top() + side * 0.23, rect.width(), side * 0.34),
                   Qt.AlignmentFlag.AlignCenter, self.text)

        # birim / alt bilgi - uzun metin kirpilmak yerine "..." ile kisaltilir
        p.setPen(QColor(C_MUTED))
        f2 = QFont(self.font())
        f2.setPixelSize(max(9, min(16, side // 14)))
        p.setFont(f2)
        fm2 = p.fontMetrics()
        sub = fm2.elidedText(self.sub or self.unit, Qt.TextElideMode.ElideRight,
                             int(rect.width() * 0.86))
        p.drawText(QRectF(rect.left(), rect.top() + side * 0.57, rect.width(), side * 0.17),
                   Qt.AlignmentFlag.AlignCenter, sub)

        # baslik
        p.setPen(QColor(C_MUTED))
        f3 = QFont(self.font())
        f3.setPixelSize(max(9, min(15, side // 15)))
        f3.setWeight(QFont.Weight.DemiBold)
        p.setFont(f3)
        fm = p.fontMetrics()
        title = fm.elidedText(self.title, Qt.TextElideMode.ElideRight,
                              int(rect.width() * 0.90))
        p.drawText(QRectF(rect.left(), rect.bottom() - side * 0.25, rect.width(), side * 0.18),
                   Qt.AlignmentFlag.AlignCenter, title)


class ProgressBar(QFrame):
    """Ince yuvarlak dolum cubugu (batarya)."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.pct = 0
        self.color = C_OK
        self.setFixedHeight(6)
        self.setStyleSheet(f"background:{C_TRACK}; border-radius:3px;")
        self.fill = QFrame(self)
        self.fill.setFixedHeight(6)
        self.fill.setStyleSheet(f"background:{self.color}; border-radius:3px;")
        self._fit()

    def set_pct(self, pct=None, color=None):
        if pct is not None:
            self.pct = max(0, min(100, int(pct)))
        if color:
            self.color = color
            self.fill.setStyleSheet(f"background:{self.color}; border-radius:3px;")
        self._fit()

    def resizeEvent(self, ev):
        super().resizeEvent(ev)
        self._fit()

    def _fit(self):
        self.fill.resize(int(self.width() * self.pct / 100), 6)


class Card(QFrame):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("card")


def make_title(text):
    l = QLabel(text)
    l.setObjectName("cardTitle")
    return l


class Helper(QProcess):
    """monsterctl helper ile kalici root baglantisi (tek sifre)."""

    READY = "helper ready"

    def __init__(self, app):
        super().__init__()
        self.app = app
        self.buf = ""
        self.active = False
        self.pending = []       # (title, args)
        self.current = None
        self.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.readyReadStandardOutput.connect(self._read)
        self.finished.connect(self._finished)

    # ---- yasam dongusu ----
    def start_job(self):
        if self.state() != QProcess.ProcessState.NotRunning:
            return
        self.app.set_root_status(None, "baglaniyor...")
        self.app.log("Root yardimci baslatiliyor (sifre bir kez sorulur)...")
        self.buf = ""
        self.start("pkexec", [monsterctl_bin(), "helper"])

    def _finished(self, code, status):
        self.active = False
        self.current = None
        self.app.set_root_status(False)
        self.app.log(f"Root yardimci kapandi (kod {code}). "
                     "'Root'a baglan' ile tekrar deneyin.")
        self.pending.clear()

    def _read(self):
        data = bytes(self.readAllStandardOutput()).decode("utf-8", "replace")
        self.buf += data
        while "\n" in self.buf:
            line, self.buf = self.buf.split("\n", 1)
            self._line(line.rstrip("\r"))

    def _line(self, line):
        if line == self.READY:
            self.active = True
            self.app.set_root_status(True)
            self.app.log("Root baglantisi hazir — artik sifre sorulmayacak.")
            self._next()
            return
        if line.startswith("EVENT "):
            self.app.on_helper_event(line[6:].strip())
            return
        if line.startswith("RC "):
            rc = line[3:].strip()
            if self.current:
                title, lines, cb = self.current
                if title:
                    out = "\n".join(lines) if lines else "tamam"
                    self.app.log(f"[{title}] (kod {rc})\n{out}")
                    self.app.toast(("✓ " if rc == "0" else f"✗ (kod {rc}) ") + title, rc == "0")
                elif cb:
                    cb(lines)
                self.current = None
            self._next()
            return
        if self.current:
            self.current[1].append(line)
        elif line.strip():
            self.app.log(line)

    # ---- komut gonderimi ----
    def send(self, args, title=None, cb=None):
        """title=None -> sessiz (log'a yazmaz); cb(lines) yanit gelince cagrilir."""
        if not self.active:
            if title:
                self.app.log(f"[{title}] root baglantisi yok — once 'Root'a baglan'.")
            return
        self.pending.append((title, [str(a) for a in args], cb))
        self._next()

    def _next(self):
        if self.current or not self.pending or not self.active:
            return
        title, args, cb = self.pending.pop(0)
        self.current = (title, [], cb)
        self.write((" ".join(args) + "\n").encode())
        self.waitForBytesWritten(100)


class Main(QMainWindow):
    def __init__(self, start_hidden=False):
        super().__init__()
        self.setWindowTitle("Monster Kontrol Merkezi")
        self.setWindowIcon(app_icon())
        QApplication.instance().setWindowIcon(app_icon())
        self.resize(1080, 700)
        self.setMinimumSize(880, 580)
        self.helper = Helper(self)
        self.start_hidden = start_hidden
        self.profiles = load_profiles()
        self.settings = load_settings()
        self._waiting = []          # root baglantisi kurulunca calisacak isler
        self._quitting = False
        self._tray_note_shown = False
        self._max_pl_w = None
        self._cpu_title = None
        self._gpu_title = None
        self._pkg_w = None
        self._pkg_busy = False
        self._tick = 0
        self._profile_idx = -1
        self._anim_timer = None
        self._anim_hue = 0
        self._anim_breath = 0.0
        self._anim_mode = "rainbow"
        self._anim_t0 = 0.0

        root = QWidget()
        root.setObjectName("rootRoot")
        self.setCentralWidget(root)
        lay = QHBoxLayout(root)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)

        lay.addWidget(self._sidebar())

        body = QWidget()
        bl = QVBoxLayout(body)
        bl.setContentsMargins(22, 18, 22, 28)
        bl.setSpacing(12)

        # ust baslik
        head = QHBoxLayout()
        titles = QVBoxLayout()
        titles.setSpacing(0)
        self.page_title = QLabel("Izleme")
        self.page_title.setObjectName("pageTitle")
        self.page_sub = QLabel("Canli durum — 2 saniyede bir yenilenir")
        self.page_sub.setObjectName("pageSub")
        titles.addWidget(self.page_title)
        titles.addWidget(self.page_sub)
        head.addLayout(titles)
        head.addStretch()
        self.profile_lbl = QLabel("Profil: —")
        self.profile_lbl.setObjectName("muted")
        head.addWidget(self.profile_lbl)
        self.pill = QLabel("● Root pasif")
        self.pill.setObjectName("pillBad")
        head.addWidget(self.pill)
        self.fs_btn = QPushButton("Tam ekran")
        self.fs_btn.setObjectName("segS")
        self.fs_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.fs_btn.setToolTip("Tam ekran (F11) — çıkmak için Esc")
        self.fs_btn.clicked.connect(self._toggle_fullscreen)
        head.addWidget(self.fs_btn)
        bl.addLayout(head)

        self.stack = QStackedWidget()
        self.stack.addWidget(self._page_monitor())
        self.stack.addWidget(self._page_controls())
        self.stack.addWidget(self._page_system())
        self.stack.addWidget(self._page_log())
        bl.addWidget(self.stack, 1)
        lay.addWidget(body, 1)

        if os.environ.get("MONSTER_GUI_NO_ROOT") != "1" and not start_hidden:
            QTimer.singleShot(600, self.helper.start_job)
        self._init_toast()
        self._init_tray()
        start = int(os.environ.get("MONSTER_GUI_PAGE", "0") or 0)
        if 0 <= start <= 3:
            btn = self.nav_group.button(start)
            if btn:
                btn.setChecked(True)
            self._go(start)
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(2000)
        self.refresh()
        if self.start_hidden and getattr(self, "tray", None) and self.tray.isVisible():
            self.tray.showMessage(APP_NAME, "Arka planda çalışıyor. Menü için simgeye sağ tıklayın.",
                                  QSystemTrayIcon.MessageIcon.Information, 3500)

    # ---------- merkezi bildirim ----------
    def _init_toast(self):
        self.toast_lbl = QLabel(self.centralWidget())
        self.toast_lbl.setObjectName("toast")
        self.toast_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.toast_lbl.hide()
        self._toast_timer = QTimer(self)
        self._toast_timer.setSingleShot(True)
        self._toast_timer.timeout.connect(self.toast_lbl.hide)

    def _place_toast(self):
        cw = self.centralWidget()
        self.toast_lbl.move(max(0, (cw.width() - self.toast_lbl.width()) // 2),
                            max(0, int(cw.height() * 0.36)))

    def toast(self, text, ok=True):
        if not self.isVisible() and getattr(self, "tray", None) and self.tray.isVisible():
            self.tray.showMessage(
                APP_NAME, text,
                QSystemTrayIcon.MessageIcon.Information if ok
                else QSystemTrayIcon.MessageIcon.Warning, 2500)
            return
        self.toast_lbl.setProperty("ok", "true" if ok else "false")
        self.toast_lbl.style().unpolish(self.toast_lbl)
        self.toast_lbl.style().polish(self.toast_lbl)
        self.toast_lbl.setText(text)
        self.toast_lbl.adjustSize()
        self._place_toast()
        self.toast_lbl.show()
        self.toast_lbl.raise_()
        self._toast_timer.start(1800)

    def resizeEvent(self, ev):
        super().resizeEvent(ev)
        if getattr(self, "toast_lbl", None) and self.toast_lbl.isVisible():
            self._place_toast()

    # ---------- sistem tepsisi (arka plan) ----------
    def _init_tray(self):
        self.tray = QSystemTrayIcon(app_icon(), self)
        self.tray.setToolTip(f"{APP_NAME} — Monster Kontrol Merkezi")
        self.tray.activated.connect(self._tray_activated)
        self.tray_menu = QMenu(self)
        self._rebuild_tray_menu()
        if QSystemTrayIcon.isSystemTrayAvailable():
            self.tray.show()
        else:
            self.tray.hide()

    def _tray_activated(self, reason):
        if reason in (QSystemTrayIcon.ActivationReason.Trigger,
                      QSystemTrayIcon.ActivationReason.DoubleClick):
            self.show_window()

    def show_window(self):
        if self.isMinimized():
            self.showNormal()
        else:
            self.show()
        self.raise_()
        self.activateWindow()
        if (os.environ.get("MONSTER_GUI_NO_ROOT") != "1"
                and not self.helper.active
                and self.helper.state() == QProcess.ProcessState.NotRunning):
            QTimer.singleShot(250, self.helper.start_job)

    def _color_icon(self, color):
        pm = QPixmap(16, 16)
        pm.fill(QColor(color))
        return QIcon(pm)

    def _rebuild_tray_menu(self):
        m = self.tray_menu
        m.clear()
        m.addAction("Kontrol merkezini aç", self.show_window)
        m.addSeparator()
        m.addAction("RAM temizle", self.tray_clean_ram)
        m.addAction("Sistem temizle", self.tray_clean_system)

        km = m.addMenu("Klavye rengi")
        for name, rgb in (("Beyaz", (255, 255, 255)), ("Mavi", (0, 80, 255)),
                          ("Yeşil", (0, 255, 120)), ("Kırmızı", (255, 40, 40)),
                          ("Turkuaz", (0, 220, 255)), ("Mor", (200, 0, 255))):
            act = km.addAction(self._color_icon(QColor(*rgb)), name)
            act.triggered.connect(
                lambda _=False, r=rgb[0], g=rgb[1], b=rgb[2], nm=name:
                self.run_root(["kbd", "color", r, g, b], f"Klavye {nm}"))
        km.addSeparator()
        km.addAction("Kapat", lambda: self.run_root(["kbd", "off"], "Klavye kapalı"))

        am = m.addMenu("Klavye animasyonu")
        labels = ("Gökkuşağı", "Nefes", "Kalp atışı", "Ateş", "Renk geçişi", "Çakar")
        for label, mode in zip(labels, self.ANIM_MODES):
            act = am.addAction(label)
            act.triggered.connect(lambda _=False, md=mode, lb=label:
                                  self._start_anim(md, lb))
        am.addSeparator()
        am.addAction("Durdur", self._stop_anim)

        fm = m.addMenu("Fan profili")
        for prof in self.profiles:
            act = fm.addAction(f"{prof['name']} · fan %{prof['fan']}")
            act.triggered.connect(lambda _=False, p=dict(prof):
                                  self.apply_profile(p))
        fm.addSeparator()
        fm.addAction("Otomatik (EC)", lambda: self.run_root(["fan", "auto"], "Fan otomatik"))

        m.addSeparator()
        auto = m.addAction("Açılışta arka planda başlat")
        auto.setCheckable(True)
        auto.setChecked(autostart_enabled())
        auto.triggered.connect(self._toggle_autostart)
        m.addAction(f"Hakkında · v{APP_VERSION}", self._about)
        m.addSeparator()
        m.addAction("Çıkış", self.quit_app)
        self.tray.setContextMenu(m)

    def _toggle_autostart(self, checked):
        if set_autostart(checked):
            self.toast("Açılışta arka planda başlayacak" if checked
                       else "Otomatik başlatma kapatıldı")
        else:
            self.toast("Otomatik başlatma ayarlanamadı", ok=False)
        self._rebuild_tray_menu()

    def _about(self):
        QMessageBox.information(
            self, f"{APP_NAME} hakkında",
            f"<b>{APP_NAME}</b> v{APP_VERSION}<br>"
            "Monster/Uniwill laptoplar için Linux kontrol merkezi.<br><br>"
            "Fan, klavye RGB, batarya limiti, CPU güç limitleri ve sistem "
            "temizliği tek yerden.<br><br>"
            f"<a href='https://github.com/talhayksk/openmonster'>"
            "github.com/talhayksk/openmonster</a>")

    def run_root(self, args, title=None, cb=None):
        """Root gerektiren islemi baglanti varsa hemen, yoksa baglaninca calistir."""
        if self.helper.active:
            self.helper.send(args, title, cb=cb)
            return
        self._waiting.append(([str(a) for a in args], title, cb))
        self.log("İşlem kuyruğa alındı; root bağlantısı kurulunca çalışacak.")
        if self.helper.state() == QProcess.ProcessState.NotRunning:
            self.helper.start_job()

    def _flush_waiting(self):
        waiting, self._waiting = self._waiting, []
        for args, title, cb in waiting:
            self.helper.send(args, title, cb=cb)

    def tray_clean_ram(self):
        self.run_root(["cleanup", "ram"], None, cb=self._ram_done)

    def tray_clean_system(self):
        self.run_cleanup(keys=["cache", "thumb", "trash", "pkg", "journal", "syscache"])

    def _toggle_fullscreen(self):
        if self.isFullScreen():
            self.showNormal()
        else:
            self.showFullScreen()

    def keyPressEvent(self, ev):
        if ev.key() == Qt.Key.Key_F11:
            self._toggle_fullscreen()
            return
        if ev.key() == Qt.Key.Key_Escape and self.isFullScreen():
            self.showNormal()
            return
        super().keyPressEvent(ev)

    def quit_app(self):
        self._quitting = True
        self._stop_anim()
        try:
            if self.helper.state() != QProcess.ProcessState.NotRunning:
                self.helper.write(b"quit\n")
                self.helper.waitForFinished(400)
                if self.helper.state() != QProcess.ProcessState.NotRunning:
                    self.helper.kill()
        except (OSError, RuntimeError):
            pass
        if hasattr(self, "tray"):
            self.tray.hide()
        QApplication.instance().quit()

    # ---------- sidebar ----------
    def _sidebar(self):
        s = QFrame()
        s.setObjectName("sidebar")
        s.setFixedWidth(200)
        l = QVBoxLayout(s)
        l.setContentsMargins(14, 20, 14, 16)
        l.setSpacing(6)

        brandrow = QHBoxLayout()
        brandrow.setSpacing(9)
        logo = QLabel()
        logo.setPixmap(app_icon().pixmap(34, 34))
        logo.setFixedSize(34, 34)
        brandbox = QVBoxLayout()
        brandbox.setSpacing(0)
        brand = QLabel("MONSTER")
        brand.setObjectName("brand")
        sub = QLabel("KONTROL MERKEZİ")
        sub.setObjectName("brandsub")
        brandbox.addWidget(brand)
        brandbox.addWidget(sub)
        brandrow.addWidget(logo)
        brandrow.addLayout(brandbox)
        brandrow.addStretch()
        l.addLayout(brandrow)
        l.addSpacing(22)

        self.nav_group = QButtonGroup(self)
        self.nav_group.setExclusive(True)
        names = ["  İzleme", "  Kontroller", "  Sistem", "  Çıktı"]
        for i, name in enumerate(names):
            b = QPushButton(name)
            b.setObjectName("nav")
            b.setCheckable(True)
            b.setCursor(Qt.CursorShape.PointingHandCursor)
            b.clicked.connect(lambda _=False, idx=i: self._go(idx))
            self.nav_group.addButton(b, i)
            l.addWidget(b)
            if i == 0:
                b.setChecked(True)
        l.addStretch()

        self.autostart_cb = QCheckBox("Açılışta arka planda")
        self.autostart_cb.setChecked(autostart_enabled())
        self.autostart_cb.setToolTip("Oturum açıldığında sistem tepsisinde başlat")
        self.autostart_cb.toggled.connect(self._toggle_autostart)
        l.addWidget(self.autostart_cb)

        self.connect_btn = QPushButton("Root'a bağlan")
        self.connect_btn.setObjectName("ghost")
        self.connect_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.connect_btn.clicked.connect(self.helper.start_job)
        l.addWidget(self.connect_btn)
        note = QLabel("Şifre yalnızca bir kez sorulur.")
        note.setObjectName("muted")
        note.setWordWrap(True)
        l.addWidget(note)
        return s

    def _go(self, idx):
        self.stack.setCurrentIndex(idx)
        self.page_title.setText(["Izleme", "Kontroller", "Sistem", "Çıktı"][idx])
        self.page_sub.setText([
            "Canlı durum — 2 saniyede bir yenilenir",
            "Fan, klavye ışığı, batarya ve güç limitleri",
            "Disk sağlığı ve seçmeli sistem temizliği",
            "Root işlemlerinin günlüğü",
        ][idx])

    # ---------- Izleme ----------
    def _page_monitor(self):
        content = QWidget()
        content.setObjectName("pageBody")
        l = QVBoxLayout(content)
        l.setContentsMargins(0, 0, 0, 0)
        l.setSpacing(14)

        grid = QGridLayout()
        grid.setSpacing(14)
        self.g_cpu = Gauge("CPU", "°C", 100)
        self.g_gpu = Gauge("GPU", "°C", 100)
        self.g_gpu_load = Gauge("GPU YÜK", "%", 100)
        self.g_fan1 = Gauge("FAN 1 · CPU", "rpm", 100)
        self.g_fan2 = Gauge("FAN 2 · GPU", "rpm", 100)
        self.g_ram = Gauge("RAM", "%", 100)
        gauges = (self.g_cpu, self.g_gpu, self.g_gpu_load,
                  self.g_fan1, self.g_fan2, self.g_ram)
        for i, g in enumerate(gauges):
            card = Card()
            card.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
            cl = QVBoxLayout(card)
            cl.setContentsMargins(10, 10, 10, 10)
            cl.addWidget(g, 1)
            if g is self.g_ram:
                rb = QPushButton("RAM temizle")
                rb.setObjectName("ghost")
                rb.setCursor(Qt.CursorShape.PointingHandCursor)
                rb.clicked.connect(self.clean_ram)
                cl.addWidget(rb)
            grid.addWidget(card, i // 3, i % 3)
        for c in range(3):
            grid.setColumnStretch(c, 1)
        for r in range(2):
            grid.setRowStretch(r, 1)
        l.addLayout(grid, 1)

        row = QHBoxLayout()
        row.setSpacing(14)
        row.addWidget(self._battery_card(), 1)
        row.addWidget(self._pl_card(), 1)
        l.addLayout(row)

        # Kucuk ekranlarda da tasmasin: kaydirilabilir sayfa
        sa = QScrollArea()
        sa.setWidgetResizable(True)
        sa.setFrameShape(QFrame.Shape.NoFrame)
        sa.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        sa.setWidget(content)
        return sa

    def _battery_card(self):
        card = Card()
        l = QVBoxLayout(card)
        l.setContentsMargins(16, 14, 16, 14)
        l.addWidget(make_title("BATARYA"))
        h = QHBoxLayout()
        self.bat_val = QLabel("%–")
        self.bat_val.setObjectName("bigValue")
        self.bat_state = QLabel("okunuyor...")
        self.bat_state.setObjectName("muted")
        h.addWidget(self.bat_val)
        h.addStretch()
        h.addWidget(self.bat_state)
        l.addLayout(h)
        self.bat_bar = ProgressBar()
        l.addWidget(self.bat_bar)
        return card

    def _pl_card(self):
        card = Card()
        l = QVBoxLayout(card)
        l.setContentsMargins(16, 14, 16, 14)
        l.addWidget(make_title("GÜÇ LİMİTLERİ"))
        h = QHBoxLayout()
        h.setSpacing(22)
        self.pl1_val = QLabel("PL1 –")
        self.pl2_val = QLabel("PL2 –")
        for w in (self.pl1_val, self.pl2_val):
            w.setStyleSheet("font-size: 20px; font-weight: 700;")
            h.addWidget(w)
        h.addStretch()
        l.addLayout(h)
        self.pl_note = QLabel("intel-rapl / package-0")
        self.pl_note.setObjectName("muted")
        l.addWidget(self.pl_note)
        return card

    # ---------- Kontroller ----------
    def _page_controls(self):
        page = QWidget()
        page.setObjectName("pageBody")
        grid = QGridLayout(page)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setSpacing(14)

        # Fan
        fan = Card()
        fl = QVBoxLayout(fan)
        fl.setContentsMargins(18, 16, 18, 16)
        fl.addWidget(make_title("FAN"))
        seg = QHBoxLayout()
        seg.setSpacing(8)
        self.fan_group = QButtonGroup(self)
        for i, (val, txt) in enumerate((("main", "Ana"), ("secondary", "İkincil"), ("both", "Her ikisi"))):
            b = QPushButton(txt)
            b.setObjectName("seg")
            b.setCheckable(True)
            b.setCursor(Qt.CursorShape.PointingHandCursor)
            b.setProperty("val", val)
            self.fan_group.addButton(b, i)
            seg.addWidget(b)
            if i == 2:
                b.setChecked(True)
        fl.addLayout(seg)
        self.fan_slider = QSlider(Qt.Orientation.Horizontal)
        self.fan_slider.setRange(0, 100)
        self.fan_slider.setValue(60)
        self.fan_val = QLabel("%60")
        self.fan_val.setStyleSheet("font-size: 18px; font-weight: 700;")
        fl.addWidget(self.fan_slider)
        h = QHBoxLayout()
        h.addWidget(self.fan_val)
        h.addStretch()
        b1 = QPushButton("Uygula")
        b1.setObjectName("primary")
        b1.setCursor(Qt.CursorShape.PointingHandCursor)
        b1.clicked.connect(self.fan_apply)
        b2 = QPushButton("Otomatik (EC)")
        b2.setObjectName("ghost")
        b2.setCursor(Qt.CursorShape.PointingHandCursor)
        b2.clicked.connect(lambda: self._cmd(["fan", "auto"], "Fan otomatik"))
        h.addWidget(b2)
        h.addWidget(b1)
        fl.addLayout(h)
        fl.addWidget(self._hint("Manuel modda %30 altına inmek ısınma riski taşır."))
        prof = QHBoxLayout()
        prof.setSpacing(8)
        self.profile_combo = QComboBox()
        self.profile_combo.setSizePolicy(QSizePolicy.Policy.Expanding,
                                         QSizePolicy.Policy.Fixed)
        self.profile_combo.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.profile_combo.customContextMenuRequested.connect(self._profile_menu)
        prof.addWidget(self.profile_combo, 1)
        self._rebuild_profile_buttons()
        applyp = QPushButton("Uygula")
        applyp.setObjectName("segS")
        applyp.setCursor(Qt.CursorShape.PointingHandCursor)
        applyp.setToolTip("Seçili profili uygula")
        applyp.clicked.connect(self._apply_selected_profile)
        prof.addWidget(applyp)
        addp = QPushButton("＋")
        addp.setObjectName("segS")
        addp.setFixedWidth(32)
        addp.setCursor(Qt.CursorShape.PointingHandCursor)
        addp.setToolTip("Yeni fan profili ekle")
        addp.clicked.connect(self._add_profile)
        prof.addWidget(addp)
        morep = QPushButton("⋯")
        morep.setObjectName("segS")
        morep.setFixedWidth(32)
        morep.setCursor(Qt.CursorShape.PointingHandCursor)
        morep.setToolTip("Seçili profili düzenle / sil")
        morep.clicked.connect(self._profile_menu)
        prof.addWidget(morep)
        fl.addLayout(prof)
        hint = self._hint("Fiziksel profil tuşu profiller arasında dolaşır. "
                          "Profil listesine sağ tıklayarak düzenleyebilirsiniz.")
        fl.addWidget(hint)
        self.fan_slider.valueChanged.connect(lambda v: self.fan_val.setText(f"%{v}"))
        grid.addWidget(fan, 0, 0)

        # Klavye
        kb = Card()
        kl = QVBoxLayout(kb)
        kl.setContentsMargins(18, 16, 18, 16)
        kl.addWidget(make_title("KLAVYE IŞIĞI"))
        self.kb_slider = QSlider(Qt.Orientation.Horizontal)
        self.kb_slider.setRange(0, 100)
        self.kb_slider.setValue(75)
        self.kb_val = QLabel("%75")
        self.kb_val.setStyleSheet("font-size: 18px; font-weight: 700;")
        self.kb_slider.valueChanged.connect(lambda v: self.kb_val.setText(f"%{v}"))
        kl.addWidget(self.kb_slider)
        hh = QHBoxLayout()
        hh.addWidget(self.kb_val)
        hh.addStretch()
        kb_apply = QPushButton("Parlaklık")
        kb_apply.setObjectName("primary")
        kb_apply.setCursor(Qt.CursorShape.PointingHandCursor)
        kb_apply.clicked.connect(self.kb_apply)
        hh.addWidget(kb_apply)
        kl.addLayout(hh)
        colors = QHBoxLayout()
        colors.setSpacing(8)
        for (r, g, b, name) in ((255, 255, 255, "Beyaz"), (0, 80, 255, "Mavi"),
                                (0, 255, 120, "Yeşil"), (255, 40, 40, "Kırmızı"),
                                (0, 220, 255, "Turkuaz"), (200, 0, 255, "Mor")):
            b_ = QPushButton()
            b_.setFixedSize(30, 30)
            b_.setCursor(Qt.CursorShape.PointingHandCursor)
            b_.setToolTip(name)
            b_.setStyleSheet(
                f"QPushButton {{ background: rgb({r},{g},{b}); border-radius: 15px;"
                f" border: 2px solid #1e2736; }}"
                f"QPushButton:hover {{ border-color: #ffffff; }}")
            b_.clicked.connect(lambda _=False, r=r, g=g, b=b, nm=name:
                               self._cmd(["kbd", "color", r, g, b], f"Klavye {nm}"))
            colors.addWidget(b_)
        colors.addStretch()
        kl.addLayout(colors)
        krow = QHBoxLayout()
        krow.setSpacing(8)
        pick = QPushButton("Renk seç…")
        pick.setObjectName("ghost")
        pick.setCursor(Qt.CursorShape.PointingHandCursor)
        pick.clicked.connect(self.pick_color)
        krow.addWidget(pick)
        krow.addStretch()
        off = QPushButton("Kapat")
        off.setObjectName("ghost")
        off.clicked.connect(lambda: self._cmd(["kbd", "off"], "Klavye kapalı"))
        krow.addWidget(off)
        kl.addLayout(krow)
        arow = QHBoxLayout()
        arow.setSpacing(8)
        alab = QLabel("Animasyon:")
        alab.setObjectName("muted")
        arow.addWidget(alab)
        self.anim_combo = QComboBox()
        for label in ("Gökkuşağı", "Nefes", "Kalp atışı", "Ateş", "Renk geçişi", "Çakar"):
            self.anim_combo.addItem(label)
        arow.addWidget(self.anim_combo, 1)
        kl.addLayout(arow)
        abrow = QHBoxLayout()
        abrow.setSpacing(8)
        abrow.addStretch()
        ab = QPushButton("Başlat")
        ab.setObjectName("segS")
        ab.setCursor(Qt.CursorShape.PointingHandCursor)
        ab.clicked.connect(self._start_anim_combo)
        abrow.addWidget(ab)
        stop = QPushButton("Durdur")
        stop.setObjectName("segS")
        stop.setCursor(Qt.CursorShape.PointingHandCursor)
        stop.clicked.connect(self._stop_anim)
        abrow.addWidget(stop)
        kl.addLayout(abrow)
        grid.addWidget(kb, 0, 1)

        # Batarya
        bat = Card()
        bl = QVBoxLayout(bat)
        bl.setContentsMargins(18, 16, 18, 16)
        bl.addWidget(make_title("BATARYA ŞARJ LİMİTİ"))
        hb = QHBoxLayout()
        self.bat_spin = QSpinBox()
        self.bat_spin.setRange(10, 100)
        self.bat_spin.setValue(80)
        self.bat_spin.setSuffix(" %")
        hb.addWidget(self.bat_spin)
        hb.addStretch()
        bl.addLayout(hb)
        hb2 = QHBoxLayout()
        hb2.setSpacing(8)
        hb2.addStretch()
        offb = QPushButton("Limiti kapat")
        offb.setObjectName("ghost")
        offb.setCursor(Qt.CursorShape.PointingHandCursor)
        offb.clicked.connect(lambda: self._cmd(["battery", "off"], "Batarya limiti kapat"))
        applyb = QPushButton("Uygula")
        applyb.setObjectName("primary")
        applyb.setCursor(Qt.CursorShape.PointingHandCursor)
        applyb.clicked.connect(lambda: self._cmd(["battery", self.bat_spin.value()], "Batarya limiti"))
        hb2.addWidget(offb)
        hb2.addWidget(applyb)
        bl.addLayout(hb2)
        bl.addWidget(self._hint("Batarya ömrü için %60–80 aralığı idealdir."))
        grid.addWidget(bat, 1, 0)

        # Guc
        pw = Card()
        pl = QVBoxLayout(pw)
        pl.setContentsMargins(18, 16, 18, 16)
        pl.addWidget(make_title("CPU GÜÇ LİMİTLERİ"))
        hp = QHBoxLayout()
        self.pl1_spin = QSpinBox()
        self.pl1_spin.setRange(5, 255)
        self.pl1_spin.setValue(115)
        self.pl1_spin.setPrefix("PL1  ")
        self.pl1_spin.setSuffix(" W")
        self.pl2_spin = QSpinBox()
        self.pl2_spin.setRange(5, 255)
        self.pl2_spin.setValue(115)
        self.pl2_spin.setPrefix("PL2  ")
        self.pl2_spin.setSuffix(" W")
        hp.addWidget(self.pl1_spin)
        hp.addWidget(self.pl2_spin)
        hp.addStretch()
        pl.addLayout(hp)
        hp2 = QHBoxLayout()
        hp2.addStretch()
        ab = QPushButton("Uygula")
        ab.setObjectName("primary")
        ab.setCursor(Qt.CursorShape.PointingHandCursor)
        ab.clicked.connect(lambda: self._cmd(
            ["power", self.pl1_spin.value(), self.pl2_spin.value()], "Güç limitleri"))
        hp2.addWidget(ab)
        pl.addLayout(hp2)
        pl.addWidget(self._hint("PL1 sürekli, PL2 kısa süreli (boost) tavanıdır."))
        grid.addWidget(pw, 1, 1)

        grid.setColumnStretch(0, 1)
        grid.setColumnStretch(1, 1)
        grid.setRowStretch(0, 0)
        grid.setRowStretch(1, 0)
        grid.setRowStretch(2, 1)
        return self._wrap_scroll(page)

    def _wrap_scroll(self, content):
        content.setObjectName("pageBody")
        sa = QScrollArea()
        sa.setWidgetResizable(True)
        sa.setFrameShape(QFrame.Shape.NoFrame)
        sa.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        sa.setWidget(content)
        return sa

    def _hint(self, text):
        l = QLabel(text)
        l.setObjectName("muted")
        l.setWordWrap(True)
        return l

    # ---------- Sistem ----------
    CLEAN_ITEMS = (
        ("pkg", "Paket önbelleği (tümü, yeniden indirilebilir)", "root"),
        ("journal", "Journal logları (20 MB'a indir)", "root"),
        ("syscache", "Sistem önbellekleri (debtap, pkgfile, fwupd)", "root"),
        ("trash", "Çöp kutusu", "user"),
        ("thumb", "Küçük resim önbelleği", "user"),
        ("cache", "Uygulama önbelleği (~/.cache)", "user"),
    )

    SYS_CACHES = ("/var/cache/debtap", "/var/cache/pkgfile", "/var/cache/fwupd")

    def _clean_path(self, key):
        return {
            "trash": os.path.expanduser("~/.local/share/Trash/files"),
            "thumb": os.path.expanduser("~/.cache/thumbnails"),
            "cache": os.path.expanduser("~/.cache"),
        }.get(key)

    def _page_system(self):
        page = QWidget()
        page.setObjectName("pageBody")
        l = QVBoxLayout(page)
        l.setContentsMargins(0, 0, 0, 0)
        l.setSpacing(14)

        top = QHBoxLayout()
        top.setSpacing(14)
        self.g_diskuse = Gauge("DISK KULLANIM", "%", 100)
        self.g_disktemp = Gauge("DISK SICAKLIK", "°C", 100)
        for g in (self.g_diskuse, self.g_disktemp):
            card = Card()
            cl = QVBoxLayout(card)
            cl.setContentsMargins(8, 8, 8, 8)
            cl.addWidget(g)
            top.addWidget(card, 1)

        sp = Card()
        sl = QVBoxLayout(sp)
        sl.setContentsMargins(18, 16, 18, 16)
        sl.addWidget(make_title("DISK"))
        self.disk_model = QLabel("NVMe")
        self.disk_model.setStyleSheet("font-size: 15px; font-weight: 700;")
        self.disk_model.setWordWrap(True)
        sl.addWidget(self.disk_model)
        row = QHBoxLayout()
        row.setSpacing(28)
        for cap in ("OKUMA", "YAZMA"):
            box = QVBoxLayout()
            box.setSpacing(0)
            c = QLabel(cap)
            c.setObjectName("muted")
            v = QLabel("–")
            v.setStyleSheet("font-size: 22px; font-weight: 700;")
            box.addWidget(c)
            box.addWidget(v)
            row.addLayout(box)
            if cap == "OKUMA":
                self.disk_read = v
            else:
                self.disk_write = v
        row.addStretch()
        sl.addLayout(row)
        self.disk_free = QLabel("Boş alan: –")
        self.disk_free.setObjectName("muted")
        sl.addWidget(self.disk_free)
        sl.addStretch()
        top.addWidget(sp, 1)
        l.addLayout(top)

        cc = Card()
        cl = QVBoxLayout(cc)
        cl.setContentsMargins(18, 16, 18, 16)
        cl.setSpacing(8)
        cl.addWidget(make_title("SİSTEM TEMİZLEYİCİ"))
        cl.addWidget(self._hint("Temizlenecek öğeleri seçin; boyutlar 'Tara' ile hesaplanır. "
                                "Paket önbelleği ve journal için root bağlantısı gerekir."))
        self.clean_rows = {}
        for key, label, _kind in self.CLEAN_ITEMS:
            r = QHBoxLayout()
            cb = QCheckBox(label)
            cb.setChecked(True)
            cb.setCursor(Qt.CursorShape.PointingHandCursor)
            size = QLabel("–")
            size.setObjectName("muted")
            r.addWidget(cb)
            r.addStretch()
            r.addWidget(size)
            cl.addLayout(r)
            self.clean_rows[key] = (cb, size)
        cl.addStretch()
        brow = QHBoxLayout()
        self.clean_total = QLabel("Toplam: –")
        self.clean_total.setObjectName("muted")
        scan = QPushButton("Tara")
        scan.setObjectName("ghost")
        scan.setCursor(Qt.CursorShape.PointingHandCursor)
        scan.clicked.connect(self.scan_cleanup)
        run = QPushButton("Seçilenleri temizle")
        run.setObjectName("primary")
        run.setCursor(Qt.CursorShape.PointingHandCursor)
        run.clicked.connect(lambda: self.run_cleanup())
        brow.addWidget(self.clean_total)
        brow.addStretch()
        brow.addWidget(scan)
        brow.addWidget(run)
        cl.addLayout(brow)
        l.addWidget(cc, 1)
        QTimer.singleShot(1500, self.scan_cleanup)
        return self._wrap_scroll(page)

    def scan_cleanup(self):
        sizes = {}
        for key, _label, _kind in self.CLEAN_ITEMS:
            if key == "pkg":
                sizes[key] = dir_bytes("/var/cache/pacman/pkg")
            elif key == "journal":
                sizes[key] = journal_bytes()
            elif key == "syscache":
                sizes[key] = sum(dir_bytes(p) for p in self.SYS_CACHES)
            else:
                sizes[key] = dir_bytes(self._clean_path(key))
            self.clean_rows[key][1].setText(human_bytes(sizes[key]))
        self.clean_total.setText(f"Toplam: {human_bytes(sum(sizes.values()))}")

    ROOT_CLEAN = {"pkg": "pkgcache", "journal": "journal", "syscache": "syscache"}

    def run_cleanup(self, keys=None):
        if keys is None:
            sel = [k for k, (cb, _) in self.clean_rows.items() if cb.isChecked()]
        else:
            sel = list(keys)
        if not sel:
            self.toast("Temizlenecek öğe seçilmedi", ok=False)
            return
        freed = 0
        for key in sel:
            if key in ("trash", "thumb", "cache"):
                freed += rm_children(self._clean_path(key))
                if key == "trash":
                    rm_children(os.path.expanduser("~/.local/share/Trash/info"))
        if freed:
            self.log(f"Kullanıcı önbelleklerinden {human_bytes(freed)} temizlendi.")
            self.toast(f"{human_bytes(freed)} kullanıcı önbelleği temizlendi")

        self._pending_root = 0
        for key in sel:
            if key not in self.ROOT_CLEAN:
                continue
            args = ["cleanup", self.ROOT_CLEAN[key]]
            if key == "journal":
                args.append("20")
            self._pending_root += 1
            self.run_root(args, None,
                          cb=lambda lines, k=key: self._clean_done(k, lines))

        if self._pending_root:
            self.toast(f"Temizlik yapılıyor… ({self._pending_root} işlem)")
        else:
            QTimer.singleShot(700, self.scan_cleanup)
        QTimer.singleShot(900, self.refresh)

    def _clean_done(self, key, lines):
        name = {"pkg": "Paket önbelleği", "journal": "Journal", "syscache": "Sistem önbellekleri"}[key]
        shown = [ln for ln in lines if ln.strip()]
        self.log(f"[{name}]\n" + ("\n".join(shown[-8:]) if shown else "tamam"))
        self._pending_root = max(0, getattr(self, "_pending_root", 1) - 1)
        if self._pending_root == 0:
            self.toast(f"{name} temizlendi")
            self.scan_cleanup()
            QTimer.singleShot(800, self.refresh)

    def clean_ram(self):
        self.toast("RAM önbelleği temizleniyor…")
        self.run_root(["cleanup", "ram"], None, cb=self._ram_done)

    def _ram_done(self, lines):
        msg = "RAM temizlendi"
        for ln in lines:
            if ln.strip():
                self.log(ln)
            m = re.search(r"\(\+(-?\d+) MB\)", ln)
            if m:
                freed = int(m.group(1))
                msg = (f"RAM: +{freed} MB kullanılabilir" if freed >= 0
                       else f"RAM temizlendi ({freed} MB)")
        self.toast(msg)
        self.refresh()

    # ---------- Cikti ----------
    def _page_log(self):
        page = QWidget()
        l = QVBoxLayout(page)
        l.setContentsMargins(0, 0, 0, 0)
        row = QHBoxLayout()
        diag = QPushButton("Fan tanısı (diag)")
        diag.setObjectName("ghost")
        diag.setCursor(Qt.CursorShape.PointingHandCursor)
        diag.clicked.connect(lambda: self._cmd(["fan", "diag"], "Fan tanı"))
        row.addWidget(diag)
        rel = QPushButton("Root yardımcıyı yenile")
        rel.setObjectName("ghost")
        rel.setCursor(Qt.CursorShape.PointingHandCursor)
        rel.clicked.connect(lambda: self.helper.send(["reload"]))
        row.addWidget(rel)
        row.addStretch()
        l.addLayout(row)
        self.logview = QPlainTextEdit()
        self.logview.setReadOnly(True)
        self.logview.setMaximumBlockCount(2000)
        l.addWidget(self.logview, 1)
        self.log("monster-gui hazir. Root yardımı arka planda başlatılacak.")
        return page

    # ---------- komut / durum ----------
    def _cmd(self, args, title=None):
        self.helper.send(args, title)

    def _got_pkg(self, lines):
        self._pkg_busy = False
        for ln in lines:
            m = re.match(r"\s*([\d.]+)\s*W", ln)
            if m:
                self._pkg_w = float(m.group(1))
                break

    def fan_apply(self):
        btn = self.fan_group.checkedButton()
        sel = btn.property("val") if btn else "both"
        self._cmd(["fan", self.fan_slider.value(), "--fan", sel], "Fan")

    def apply_profile(self, profile):
        name = profile.get("name", "Profil")
        fan = int(profile.get("fan", 55))
        power = int(profile.get("power", 60))
        self.profile_lbl.setText(f"Profil: {name}")
        self.fan_slider.setValue(fan)
        combo = getattr(self, "profile_combo", None)
        if combo is not None:
            idx = combo.findData(name)
            if idx >= 0:
                combo.blockSignals(True)
                combo.setCurrentIndex(idx)
                combo.blockSignals(False)
        both = self.fan_group.button(2)
        if both:
            both.setChecked(True)
        self.run_root(["fan", fan, "--fan", "both"], f"Profil · fan %{fan}")
        if self._max_pl_w:
            watts = max(5, int(round(self._max_pl_w * power / 100.0)))
            self.pl1_spin.setValue(watts)
            self.pl2_spin.setValue(watts)
            self.run_root(["power", watts, watts], f"Profil · güç {watts} W")
        self.toast(f"Profil: {name}", ok=True)

    # ---- ozel fan profilleri ----
    def _save_profiles(self):
        if not save_json(PROFILES_FILE, self.profiles):
            self.toast("Profil dosyası yazılamadı", ok=False)
        if hasattr(self, "tray_menu"):
            self._rebuild_tray_menu()

    def _rebuild_profile_buttons(self):
        combo = getattr(self, "profile_combo", None)
        if combo is None:
            return
        current = combo.currentData()
        combo.blockSignals(True)
        combo.clear()
        for prof in self.profiles:
            combo.addItem(prof["name"], prof["name"])
        if current:
            idx = combo.findData(current)
            if idx >= 0:
                combo.setCurrentIndex(idx)
        combo.blockSignals(False)

    def _selected_profile(self):
        combo = getattr(self, "profile_combo", None)
        if combo is None or not self.profiles:
            return None
        name = combo.currentData()
        for prof in self.profiles:
            if prof["name"] == name:
                return prof
        idx = combo.currentIndex()
        return self.profiles[idx] if 0 <= idx < len(self.profiles) else None

    def _apply_selected_profile(self):
        prof = self._selected_profile()
        if prof:
            self.apply_profile(prof)

    def _profile_menu(self, *_args):
        prof = self._selected_profile()
        if not prof:
            return
        menu = QMenu(self)
        edit = menu.addAction("Düzenle")
        delete = None if prof.get("builtin") else menu.addAction("Sil")
        menu.addSeparator()
        reset = menu.addAction("Varsayılan profillere dön")
        anchor = self.profile_combo.mapToGlobal(self.profile_combo.rect().bottomLeft())
        act = menu.exec(anchor)
        if act is None:
            return
        if act is edit:
            dlg = ProfileDialog(self, prof)
            if dlg.exec() == QDialog.DialogCode.Accepted:
                new = dlg.values()
                new["builtin"] = bool(prof.get("builtin"))
                for i, p in enumerate(self.profiles):
                    if p is prof or p["name"] == prof["name"]:
                        self.profiles[i] = new
                        break
                self._save_profiles()
                self._rebuild_profile_buttons()
                self.toast(f"Profil güncellendi: {new['name']}")
        elif delete is not None and act is delete:
            if QMessageBox.question(
                    self, "Profili sil",
                    f"'{prof['name']}' profili silinsin mi?") != QMessageBox.StandardButton.Yes:
                return
            self.profiles = [p for p in self.profiles if p["name"] != prof["name"]]
            self._save_profiles()
            self._rebuild_profile_buttons()
            self.toast(f"Profil silindi: {prof['name']}")
        elif act is reset:
            self.profiles = [dict(p) for p in DEFAULT_PROFILES]
            self._save_profiles()
            self._rebuild_profile_buttons()
            self.toast("Varsayılan profiller geri yüklendi")

    def _add_profile(self):
        dlg = ProfileDialog(self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        prof = dlg.values()
        if any(p["name"].lower() == prof["name"].lower() for p in self.profiles):
            self.toast("Bu adda bir profil zaten var", ok=False)
            return
        self.profiles.append(prof)
        self._save_profiles()
        self._rebuild_profile_buttons()
        combo = getattr(self, "profile_combo", None)
        if combo is not None:
            combo.setCurrentIndex(combo.findData(prof["name"]))
        self.toast(f"Yeni profil eklendi: {prof['name']}")

    # ---- fiziksel fan/profil tusu ----
    def on_helper_event(self, name):
        if name == "fankey":
            self._cycle_profile()
        elif name:
            self.log(f"Fiziksel tuş olayı: {name}")

    def _cycle_profile(self):
        if not self.profiles:
            return
        self._profile_idx = (self._profile_idx + 1) % len(self.profiles)
        prof = self.profiles[self._profile_idx]
        self.log(f"Fiziksel tuş → profil: {prof['name']}")
        self.apply_profile(prof)

    # ---- klavye animasyonlari ----
    ANIM_MODES = ("rainbow", "breathe", "heartbeat", "fire", "fade", "strobe")

    def _start_anim_combo(self):
        idx = max(0, min(self.anim_combo.currentIndex(), len(self.ANIM_MODES) - 1))
        self._start_anim(self.ANIM_MODES[idx])

    def _start_anim(self, mode, label=None):
        if self._anim_timer:
            self._anim_timer.stop()
            self._anim_timer = None
        if not self.helper.active:
            self._pending_anim = (mode, label)
            self.log("Animasyon root bağlantısı kurulunca başlayacak.")
            self.run_root(["kbd", "status"], "Root bağlantısı")
            return
        self._anim_mode = mode
        self._anim_t0 = time.monotonic()
        self._anim_timer = QTimer(self)
        self._anim_timer.timeout.connect(self._anim_tick)
        self._anim_timer.start(100)
        self.log(f"Klavye animasyonu: {label or self.anim_combo.currentText()}")

    def _stop_anim(self):
        if self._anim_timer:
            self._anim_timer.stop()
            self._anim_timer = None
            self.log("Klavye animasyonu durdu.")

    def _anim_tick(self):
        if not self.helper.active:
            self._stop_anim()
            return
        m = self._anim_mode
        t = time.monotonic() - self._anim_t0
        if m == "rainbow":
            self._anim_hue = (self._anim_hue + 9) % 360
            c = QColor.fromHsv(self._anim_hue, 255, 255)
            self.helper.send(["kbd", "color", c.red(), c.green(), c.blue()])
        elif m == "breathe":
            v = int((math.sin(t * 2.2) + 1) * 50)
            self.helper.send(["kbd", max(5, min(100, v))])
        elif m == "heartbeat":
            ph = t % 1.1
            b1 = math.exp(-((ph - 0.10) * 14) ** 2)
            b2 = math.exp(-((ph - 0.32) * 14) ** 2) * 0.7
            self.helper.send(["kbd", max(5, int(8 + 92 * max(b1, b2)))])
        elif m == "fire":
            c = QColor.fromHsv(random.randint(5, 38), 255, random.randint(150, 255))
            self.helper.send(["kbd", "color", c.red(), c.green(), c.blue()])
        elif m == "fade":
            k = (math.sin(t * 1.4) + 1) / 2
            c1, c2 = QColor("#00e5ff"), QColor("#c026d3")
            c = QColor(int(c1.red() * (1 - k) + c2.red() * k),
                       int(c1.green() * (1 - k) + c2.green() * k),
                       int(c1.blue() * (1 - k) + c2.blue() * k))
            self.helper.send(["kbd", "color", c.red(), c.green(), c.blue()])
        else:  # strobe
            self.helper.send(["kbd", 100 if (t % 0.5) < 0.25 else 0])

    def kb_apply(self):
        self._cmd(["kbd", self.kb_slider.value()], "Klavye parlaklık")

    def pick_color(self):
        c = QColorDialog.getColor(QColor(C_ACCENT), self, "Klavye rengi seç")
        if c.isValid():
            self._cmd(["kbd", "color", c.red(), c.green(), c.blue()], "Klavye renk")

    def log(self, text):
        self.logview.appendPlainText(text)

    def set_root_status(self, active, text=None):
        if active is None:
            self.pill.setText(f"● {text or 'baglaniyor...'}")
            self.pill.setObjectName("pillBad")
        elif active:
            self.pill.setText("● Root aktif")
            self.pill.setObjectName("pillOk")
            self.connect_btn.setDisabled(True)
            self._flush_waiting()
            pending = getattr(self, "_pending_anim", None)
            if pending:
                self._pending_anim = None
                QTimer.singleShot(150, lambda: self._start_anim(*pending))
        else:
            self.pill.setText("● Root pasif")
            self.pill.setObjectName("pillBad")
            self.connect_btn.setDisabled(False)
            self._pkg_busy = False
            self._pkg_w = None
            self._waiting.clear()
        self.pill.style().unpolish(self.pill)
        self.pill.style().polish(self.pill)

    def closeEvent(self, ev):
        if (not self._quitting and getattr(self, "tray", None)
                and self.tray.isVisible()):
            ev.ignore()
            self.hide()
            if not self._tray_note_shown:
                self._tray_note_shown = True
                self.tray.showMessage(
                    APP_NAME, "Arka planda çalışıyor. Menü için sistem tepsisindeki "
                    "simgeye sağ tıklayın.", QSystemTrayIcon.MessageIcon.Information, 3500)
            return
        self._stop_anim()
        super().closeEvent(ev)

    # ---------- canli veri ----------
    def refresh(self):
        # Pencere tepsideyken agir okumalari (nvidia-smi vb.) durdur.
        if (getattr(self, "_stats_ready", False) and not self.isVisible()
                and getattr(self, "tray", None) and self.tray.isVisible()):
            return
        self._tick += 1
        if self.helper.active and not self._pkg_busy and self._tick % 3 == 1:
            self._pkg_busy = True
            self.helper.send(["power", "stat"], cb=self._got_pkg)

        freq = cpu_freq_ghz()
        cpu_sub = f"{freq:.1f} GHz" if freq else ""
        if self._pkg_w is not None:
            cpu_sub = (cpu_sub + " · " if cpu_sub else "") + f"{self._pkg_w:.0f} W"

        hw = uniwill_hwmon()
        if hw:
            t1 = rread(os.path.join(hw, "temp1_input"))
            t2 = rread(os.path.join(hw, "temp2_input"))
            f1 = rread(os.path.join(hw, "fan1_input"))
            f2 = rread(os.path.join(hw, "fan2_input"))
            d1 = int(rread(os.path.join(hw, "pwm1")) or 0)
            d2 = int(rread(os.path.join(hw, "pwm2")) or 0)
            if t1:
                c = float(t1) / 1000
                self.g_cpu.set(c, f"{c:.0f}", temp_color(c), cpu_sub)
            if t2:
                g = float(t2) / 1000
                self.g_gpu.set(g, f"{g:.0f}", temp_color(g), self.g_gpu.sub)
            if f1:
                duty = d1 * 100 / 200
                self.g_fan1.set(duty, f"{int(f1):,}", C_ACCENT, f"%{duty:.0f}")
            if f2:
                duty = d2 * 100 / 200
                self.g_fan2.set(duty, f"{int(f2):,}", C_ACCENT, f"%{duty:.0f}")

        nv = nvidia_stats()
        if self._cpu_title is None:
            cn = cpu_name_short()
            if cn:
                self._cpu_title = f"CPU · {cn}"
                self.g_cpu.set_title(self._cpu_title)
        if nv and nv.get("name") and self._gpu_title is None:
            gn = gpu_name_short(nv["name"])
            if gn:
                self._gpu_title = f"GPU · {gn}"
                self.g_gpu.set_title(self._gpu_title)
        if nv:
            parts = []
            if nv.get("clock"):
                parts.append(f"{nv['clock'] / 1000:.1f} GHz")
            if nv.get("power"):
                parts.append(f"{nv['power']:.0f} W")
            if parts:
                self.g_gpu.set_sub(" · ".join(parts))
        if nv and nv["util"] is not None:
            util = nv["util"]
            mu = nv["mem_used"] or 0
            mt = nv["mem_total"] or 0
            sub = f"{mu / 1024:.1f}/{mt / 1024:.0f} GB"
            if nv["temp"] is not None:
                sub = f"{nv['temp']:.0f}°C · " + sub
            self.g_gpu_load.set(util, f"{util:.0f}",
                                C_HOT if util >= 90 else C_ACCENT, sub)
        else:
            self.g_gpu_load.set(0, "–", C_MUTED, "nvidia-smi yok")

        rs = ram_stats()
        if rs:
            pct, used, total = rs
            self.g_ram.set(pct, f"{pct:.0f}", C_WARN if pct >= 80 else C_ACCENT,
                           f"{used:.1f}/{total:.0f} GB")
        else:
            self.g_ram.set(0, "–", C_MUTED, "")

        di = disk_info()
        if di:
            if di["usage"] is not None:
                self.g_diskuse.set(di["usage"], f"{di['usage']:.0f}",
                                   C_WARN if di["usage"] >= 90 else C_ACCENT,
                                   f"{(di['free_gb'] or 0):.0f} GB boş")
            if di["temp"] is not None:
                self.g_disktemp.set(di["temp"], f"{di['temp']:.0f}",
                                    temp_color(di["temp"]), "")
            else:
                self.g_disktemp.set(0, "–", C_MUTED, "")
            self.disk_model.setText(di["model"].strip() or "Disk")
            if di["read_mbs"] is not None:
                self.disk_read.setText(human(di["read_mbs"] * 1e6) + "/s")
                self.disk_write.setText(human((di["write_mbs"] or 0) * 1e6) + "/s")
            self.disk_free.setText(f"Boş alan: {(di['free_gb'] or 0):.0f} GB / "
                                   f"{(di['total_gb'] or 0):.0f} GB")

        cap = rread(BAT + "/capacity")
        stat = rread(BAT + "/status")
        if cap:
            pct = int(cap)
            self.bat_val.setText(f"%{pct}")
            self.bat_state.setText(stat or "?")
            self.bat_bar.set_pct(pct, C_OK if pct > 40 else C_WARN)

        idxmap = {}
        for i in range(4):
            n = rread(f"{RAPL}/constraint_{i}_name")
            if n:
                idxmap[n] = i
        for kind, lbl, w in (("long_term", "PL1", self.pl1_val), ("short_term", "PL2", self.pl2_val)):
            if kind in idxmap:
                uw = rread(f"{RAPL}/constraint_{idxmap[kind]}_power_limit_uw")
                if uw:
                    w.setText(f"{lbl} {int(uw) / 1e6:.0f} W")
                maxuw = rread(f"{RAPL}/constraint_{idxmap[kind]}_max_power_limit_uw")
                base = int(maxuw) / 1e6 if maxuw else (int(uw) / 1e6 if uw else None)
                if base:
                    self._max_pl_w = max(self._max_pl_w or 0, base)
        self._stats_ready = True


def main():
    app = QApplication(sys.argv)
    app.setApplicationName(APP_ID)
    app.setApplicationVersion(APP_VERSION)
    app.setApplicationDisplayName(APP_NAME)
    app.setDesktopFileName(APP_ID)
    app.setWindowIcon(app_icon())
    app.setStyle("Fusion")
    app.setStyleSheet(QSS)
    app.setQuitOnLastWindowClosed(False)
    hidden = "--tray" in sys.argv or "--minimized" in sys.argv
    win = Main(start_hidden=hidden)
    if not hidden or not win.tray.isVisible():
        win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
