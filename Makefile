CC      := gcc
CFLAGS  := -Wall -Wextra -O2 -std=gnu11
TARGET  := monsterctl
SRC     := src/monsterctl.c
PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin
ICON_SIZES := 16 24 32 48 64 128 256 512

.PHONY: all install uninstall clean gui service package

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

gui: $(TARGET)
	python3 gui/monstergui.py

service: install
	systemctl daemon-reload
	systemctl enable --now monster-ctrl-fanprofile.service
	@echo "Fan profili servisi aktif: systemctl status monster-ctrl-fanprofile"

install: $(TARGET)
	install -Dm0755 $(TARGET) $(DESTDIR)$(BINDIR)/monsterctl
	install -Dm0755 packaging/openmonster $(DESTDIR)$(BINDIR)/openmonster
	install -Dm0644 gui/monstergui.py $(DESTDIR)$(PREFIX)/share/openmonster/monstergui.py
	install -Dm0644 packaging/openmonster.desktop $(DESTDIR)$(PREFIX)/share/applications/openmonster.desktop
	install -Dm0644 assets/openmonster.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/openmonster.svg
	for s in $(ICON_SIZES); do \
		install -Dm0644 assets/icons/openmonster-$$s.png \
			$(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/openmonster.png; \
	done
	install -Dm0644 etc/monster-ctrl.conf $(DESTDIR)/etc/modprobe.d/monster-ctrl.conf
	install -Dm0644 etc/monster-ctrl.load $(DESTDIR)/etc/modules-load.d/monster-ctrl.load
	install -Dm0644 etc/monster-ctrl-fanprofile.service $(DESTDIR)/etc/systemd/system/monster-ctrl-fanprofile.service
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/openmonster/LICENSE
	@echo "OpenMonster kuruldu."
	@echo "Uygulama: openmonster   |   CLI: sudo monsterctl status"
	@echo "Fan profili servisi icin: sudo make service"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/monsterctl $(DESTDIR)$(BINDIR)/openmonster
	rm -f $(DESTDIR)$(PREFIX)/share/openmonster/monstergui.py
	rm -f $(DESTDIR)$(PREFIX)/share/applications/openmonster.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/openmonster.svg
	for s in $(ICON_SIZES); do \
		rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/$${s}x$${s}/apps/openmonster.png; \
	done
	rm -f $(DESTDIR)/etc/modprobe.d/monster-ctrl.conf $(DESTDIR)/etc/modules-load.d/monster-ctrl.load
	rm -f $(DESTDIR)/etc/systemd/system/monster-ctrl-fanprofile.service
	@echo "Servisi kapatmak icin: sudo systemctl disable --now monster-ctrl-fanprofile"

clean:
	rm -f $(TARGET)
