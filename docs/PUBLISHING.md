# Yayınlama rehberi

Bu proje için farklı dağıtım kanalları var:

| Kanal | Durum | Not |
|---|---|---|
| **AUR** | ⏳ Hazır, hesap bekliyor | `packaging/aur/` hazır; AUR yeni hesap kaydı geçici olarak kapalı. |
| **Debian / Ubuntu** | ✅ Hazır | `.deb`, Debian 12 ve Ubuntu 24.04 üzerinde test edildi. |
| **Fedora** | ✅ Hazır | `.rpm`, Fedora 41 üzerinde test edildi. |
| **Portable** | ✅ Hazır | Statik C yardımcısı + `install.sh`; tüm dağıtımlarda Python/PyQt6 ile çalışır. |
| **Flathub** | ⚠️ Uygun değil | Flatpak sandbox'ı EC/ACPI, `modprobe`, systemd ve root yardımcısı ile uyumlu değil. |

---

## 1. AUR (Arch User Repository)

> **Durum (Ekim 2026):** AUR yeni hesap kaydı geçici olarak kapalı
> (`New account registration is temporarily closed`). Paket dosyaları ve push
> script'i hazır; kayıt yeniden açılınca aşağıdaki adımlar uygulanacak.

### Neden AUR?

OpenMonster zaten Arch/CachyOS üzerinde geliştirildi; `pacman` paketi, `systemd`
servisi, `/etc/modprobe.d` ve `/etc/modules-load.d` dosyaları ile tam uyumlu.
AUR, bu tür donanım araçları için doğru kanal.

### Paket adı kontrolü

`openmonster` adı AUR'da boş:

```bash
curl -s 'https://aur.archlinux.org/rpc/v5/info?arg[]=openmonster'
# {"resultcount":0,...}
```

### Yerel test

```bash
cd packaging/aur
makepkg -si          # paketi indirir, derler, kurar
```

Paket kaynağı GitHub'daki `v1.1.1` etiketidir. Sürüm yükseltirken
`packaging/aur/PKGBUILD` içindeki `pkgver`, `sha256sums` ve source URL'sini
güncelleyin:

```bash
curl -L -o /tmp/openmonster.tar.gz \
  https://github.com/talhayksk/openmonster/archive/refs/tags/v1.1.1.tar.gz
sha256sum /tmp/openmonster.tar.gz
makepkg --printsrcinfo > .SRCINFO
```

### AUR hesabı ve SSH anahtarı

1. https://aur.archlinux.org/ adresinde hesap açın.
2. SSH anahtarı oluşturun (varsa atlayın):

   ```bash
   ssh-keygen -t ed25519 -C "aur" -f ~/.ssh/aur
   cat ~/.ssh/aur.pub
   ```

3. Çıkan public key'i AUR hesabınızdaki **My Account → SSH Public Key**
   alanına ekleyin.
4. Bağlantıyı test edin:

   ```bash
   ssh -T aur@aur.archlinux.org
   # Welcome to AUR, ...
   ```

### İlk gönderim

```bash
git clone ssh://aur@aur.archlinux.org/openmonster.git ~/aur-openmonster
cd ~/aur-openmonster
cp ~/projeler/monster-ctrl/packaging/aur/{PKGBUILD,.SRCINFO,openmonster.install} .
git add PKGBUILD .SRCINFO openmonster.install
git commit -m "Initial import: openmonster 1.1.1-1"
git push
```

İlk push'tan sonra paket AUR'da görünür:
https://aur.archlinux.org/packages/openmonster

Kullanıcılar şu şekilde kurar:

```bash
yay -S openmonster        # veya paru -S openmonster
```

### Sonraki sürümler

```bash
cd ~/aur-openmonster
# PKGBUILD içinde pkgver + sha256sums güncelle
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO
git commit -m "upgpkg: openmonster 1.0.1-1"
git push
```

### Otomatik script

`scripts/publish-aur.sh` bu adımları (klonlama, dosya kopyalama, `.SRCINFO`
üretme, commit ve push) tek komutta yapar:

```bash
./scripts/publish-aur.sh
```

Script, AUR SSH erişimi yoksa hiçbir şey yapmadan uyarı verir.

---

## 2. Debian / Ubuntu (.deb)

Paket, `debian:12` konteynerinde derlenir. C yardımcısı da konteynerde
derlendiği için host'taki yeni glibc'ye bağımlı kalmaz.

```bash
./scripts/build-deb.sh
# dist/openmonster_1.1.1-1_amd64.deb
```

Kurulum:

```bash
sudo apt install ./openmonster_1.1.1-1_amd64.deb
openmonster
```

Bağımlılıklar: `python3`, `python3-pyqt6`, `policykit-1 | pkexec`, `kmod`,
`systemd`. Önerilenler: `acpi-call-dkms`, `nvidia-driver`.

Debian paket dosyaları [`packaging/debian`](../packaging/debian) altında.

## 3. Fedora / RPM (.rpm)

Paket, `fedora:41` konteynerinde `rpmbuild` ile derlenir.

```bash
./scripts/build-rpm.sh
# dist/openmonster-1.1.1-1.fc41.x86_64.rpm
```

Kurulum:

```bash
sudo dnf install ./openmonster-1.1.1-1.fc41.x86_64.rpm
openmonster
```

Bağımlılıklar: `python3`, `python3-pyqt6`, `polkit`, `kmod`, `systemd`.
Önerilenler: `akmod-acpi_call`, `akmod-nvidia` (RPM Fusion).

RPM spec dosyası [`packaging/rpm/openmonster.spec`](../packaging/rpm/openmonster.spec)
altında.

## 4. Portable arşiv (tüm dağıtımlar)

Python 3 + PyQt6, PolicyKit, `kmod` ve `systemd` bulunan her x86_64 Linux
dağıtımında çalışır. C yardımcısı statik derlenir.

```bash
./scripts/build-portable.sh
# dist/openmonster-1.1.1-portable-x86_64.tar.gz
```

Kurulum/kaldırma:

```bash
tar -xzf openmonster-1.1.1-portable-x86_64.tar.gz
cd openmonster-1.1.1-portable
sudo ./install.sh
# kaldırmak için: sudo ./uninstall.sh
```

Portable dosyaları [`packaging/portable`](../packaging/portable) altında.

## 5. Flathub

### Kısa cevap: bu uygulama için Flathub uygun değil

Flathub uygulamaları **sandbox** içinde çalışır. OpenMonster'ın çalışması için
gereken şeyler sandbox dışında:

- `/proc/acpi/call` üzerinden EC okuma/yazma (root)
- `acpi_call` ve `uniwill-laptop` çekirdek modüllerinin yüklenmesi (`modprobe`)
- `/sys/class/powercap/intel-rapl` yazma (root)
- `/sys/class/leds/uniwill...` klavye RGB yazma
- `pkexec` ile root yardımcı süreci
- systemd fan profili servisi
- `/etc/modprobe.d` ve `/etc/modules-load.d` dosyaları

Flatpak bunların hiçbirini güvenli/izinli biçimde sağlamaz. `--filesystem=host`,
`--device=all` veya `flatpak-spawn --host` ile zorlanabilir ama Flathub inceleme
kuralları bu tür geniş izinleri ve host'ta komut çalıştırmayı kabul etmez.
Ayrıca Flatpak paketi kurulum sırasında `/etc` ve systemd servisi yazamaz.

Yani **fan/RGB/batarya/güç kontrolleri** Flatpak içinde çalışmaz; uygulama
yalnızca "salt okunur izleme"ye indirgenir. Bu da mevcut ürünle aynı şey değil.

### Yine de denemek isterseniz: resmî adımlar

1. Manifest hazırlayın (`io.github.talhayksk.OpenMonster.yml`):
   - `app-id: io.github.talhayksk.OpenMonster`
   - runtime: `org.kde.Platform//6.8` (PyQt6 için) veya PyQt6'yı ayrı module
     olarak derleyin
   - `command: openmonster`
   - `finish-args` en az: `--share=ipc`, `--socket=wayland`, `--socket=fallback-x11`,
     `--device=dri`
   - `modules`: kaynak + `gcc`, PyQt6 bağımlılığı
2. Yerel derleyin:

   ```bash
   flatpak-builder --user --install --force-clean build-dir \
     io.github.talhayksk.OpenMonster.yml
   flatpak run io.github.talhayksk.OpenMonster
   ```

3. https://github.com/flathub/flathub adresinde **New App Submission** issue
   açın (veya `flathub/flathub` reposuna manifest PR'ı gönderin).
4. Flathub ekibi inceleyene kadar bekleyin.

Ancak yukarıdaki donanım kısıtları nedeniyle bu başvurunun kabul edilmesi
beklenmez. Bunun yerine:

- **Arch/CachyOS:** AUR (hazır)
- **Diğer dağıtımlar:** dağıtımın kendi paket formatı (deb/rpm) veya GitHub
  Releases üzerinden taşınabilir arşiv
- **Taşınabilir:** AppImage (root yardımcısı ve `modprobe` yapılandırması için
  yine host tarafında küçük bir kurulum gerekir)

önerilir.

### Flatpak için tek gerçekçi seçenek

Ayrı bir **"OpenMonster Monitor"** uygulaması yayınlamak:

- yalnızca sıcaklık/frekans/RAM/disk/batarya okur,
- fan/RGB/PL/batarya limiti yazmaz,
- root istemez,
- Flatpak izinleri minimumda kalır.

Böyle bir sürüm istenirse ayrı bir manifest ve ayrı bir uygulama kimliği ile
hazırlanabilir.

---

## 6. GitHub Releases ve otomatik paketleme

`.github/workflows/packages.yml` üç işi yapar:

1. `workflow_dispatch` ile manuel veya yeni bir release yayınlandığında çalışır.
2. `.deb`, `.rpm` ve portable arşivi Docker konteynerlerinde derler.
3. `SHA256SUMS-linux` üretir; workflow artifact'i olarak yükler ve release
   yayınıysa GitHub release'ine ekler.

Uygulamanın **Güncelleme** sayfası GitHub Releases API'sindeki en son sürümü
kullanır. Release'e en az bir dağıtım paketi (`.deb`, `.rpm`, `.pkg.tar.zst`
veya `-portable-x86_64.tar.gz`) ve mutlaka `SHA256SUMS-linux` eklenmelidir;
dosya adları workflow çıktısıyla aynı olmalıdır. Güncelleyici SHA-256
doğrulaması olmadan kurulum yapmaz.

Yerel olarak aynı çıktıları üretmek için:

```bash
./scripts/build-deb.sh
./scripts/build-rpm.sh
./scripts/build-portable.sh
sha256sum dist/openmonster_*.deb dist/openmonster-*.rpm \
  dist/openmonster-*portable*.tar.gz > dist/SHA256SUMS-linux
```

Arch paketi `./scripts/build-package.sh` ile ayrıca üretilir.
