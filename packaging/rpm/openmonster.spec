%global debug_package %{nil}

Name:           openmonster
Version:        1.1.1
Release:        1%{?dist}
Summary:        Monster/Uniwill laptop control center for Linux

License:        GPL-3.0-or-later
URL:            https://github.com/talhayksk/openmonster
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
Requires:       python3
Requires:       python3-pyqt6
Requires:       polkit
Requires:       kmod
Requires:       systemd
Recommends:     akmod-acpi_call
Recommends:     akmod-nvidia
Recommends:     v4l-utils

%description
OpenMonster is a Linux control center for Monster/Abra (Uniwill/Tongfang ODM)
laptops. It controls fan profiles through the EC universal fan table, keyboard
RGB, battery charge limit and Intel RAPL power limits, and provides live CPU,
GPU, RAM, disk and battery monitoring plus a system cleaner.

The GUI asks for a PolicyKit password once and then uses a privileged helper.
The same functionality is available as the monsterctl command line tool.

%prep
%setup -q

%build
# Kaynak arsivinde host'ta derlenmis bir binary kalirsa kullanma.
make clean
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot} PREFIX=/usr
# Fedora/RPM convention: unit files live in /usr/lib/systemd/system
rm -f %{buildroot}/etc/systemd/system/monster-ctrl-fanprofile.service
rm -f %{buildroot}/etc/systemd/system/monster-ctrl-profile.service
install -Dm0644 etc/monster-ctrl-fanprofile.service \
    %{buildroot}%{_unitdir}/monster-ctrl-fanprofile.service
install -Dm0644 etc/monster-ctrl-profile.service \
    %{buildroot}%{_unitdir}/monster-ctrl-profile.service

%post
systemctl daemon-reload >/dev/null 2>&1 || :
systemctl disable --now monster-ctrl-fanprofile.service >/dev/null 2>&1 || :
systemctl enable --now monster-ctrl-profile.service >/dev/null 2>&1 || :
systemctl restart monster-ctrl-profile.service >/dev/null 2>&1 || :
update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || :
gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor >/dev/null 2>&1 || :

%preun
if [ $1 -eq 0 ]; then
    systemctl disable --now monster-ctrl-profile.service >/dev/null 2>&1 || :
    systemctl disable --now monster-ctrl-fanprofile.service >/dev/null 2>&1 || :
fi

%postun
systemctl daemon-reload >/dev/null 2>&1 || :

%files
%license LICENSE
%doc README.md
/usr/bin/monsterctl
/usr/bin/openmonster
/usr/share/openmonster/monstergui.py
/usr/share/applications/openmonster.desktop
/usr/share/icons/hicolor/*/apps/openmonster.png
/usr/share/icons/hicolor/scalable/apps/openmonster.svg
%config(noreplace) /etc/modprobe.d/monster-ctrl.conf
%config(noreplace) /etc/modules-load.d/monster-ctrl.load
%{_unitdir}/monster-ctrl-fanprofile.service
%{_unitdir}/monster-ctrl-profile.service

%changelog
* Sun Oct 25 2026 Talha Yuksek <talha.yuksek@hotmail.com> - 1.1.1-1
- Load acpi_call and uniwill-laptop modules from the profile service.
- Restart the profile service on package upgrade so fan RPM/duty works
  immediately without a reboot.
- Refresh README screenshots with working fan telemetry.

* Sun Oct 25 2026 Talha Yuksek <talha.yuksek@hotmail.com> - 1.1.0-1
- Webcam controls and persistent automatic profile daemon.
- Settings are applied at boot and on AC/battery changes.

* Fri Oct 23 2026 Talha Yuksek <talha.yuksek@hotmail.com> - 1.0.0-1
- Initial package.
