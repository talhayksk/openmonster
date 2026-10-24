// monster-ctl: Monster/Uniwill notebook kontrol araci (Linux)
// EC erisimi: acpi_call modulu -> /proc/acpi/call -> \_SB.INOU.ECRR / \_SB.INOU.ECRW
// Kaynak adresler: linux/drivers/platform/x86/uniwill/uniwill-laptop.c
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/input.h>

#define ACPI_CALL_PATH   "/proc/acpi/call"
#define EC_METHOD_READ   "\\_SB.INOU.ECRR"
#define EC_METHOD_WRITE  "\\_SB.INOU.ECRW"

// ---- Uniwill EC Register Adresleri (linux kernel kaynakli) ----
#define EC_AP_OEM            0x0741u // bit0: ENABLE_MANUAL_CTRL
#define EC_SUPPORT5          0x0742u // bit4: FAN_TURBO, bit5: FAN_SUPPORT
#define EC_MANUAL_FAN_CTRL   0x0751u // bit7: FAN_MODE_USER
#define EC_PWM_1             0x075Bu // Main/1. fan duty (0..200) (saltokunur yansitma)
#define EC_PWM_2             0x075Cu // Secondary/2. fan duty

// ---- Uniwill "universel fan ctrl" tablo (EC kendisini surer) ----
#define EC_CPU_TEMP_END_TABLE   0x0F00u
#define EC_CPU_TEMP_START_TABLE 0x0F10u
#define EC_CPU_FAN_SPEED_TABLE  0x0F20u
#define EC_GPU_TEMP_END_TABLE   0x0F30u
#define EC_GPU_TEMP_START_TABLE 0x0F40u
#define EC_GPU_FAN_SPEED_TABLE  0x0F50u
#define FAN_TABLE_LEN 16
#define TABLE_BACKUP_PATH "/var/lib/monster-ctrl/fan_tables.bin"
#define EC_CHARGE_CTRL       0x07B9u // bit0..6: charge limit (%), bit7: reached
#define EC_CPU_TEMP          0x043Eu
#define EC_GPU_TEMP          0x044Fu
#define EC_CPU_PL1           0x0783u // CPU surekli guc limiti (PL1), ham bayt
#define EC_CPU_PL2           0x0784u // CPU kisa sureli guc limiti (PL2), ham bayt
#define EC_FAN0_DIRECT       0x1804u // dogrudan fan0 hiz registeri (0..200)
#define EC_FAN1_DIRECT       0x1809u // dogrudan fan1 hiz registeri (0..200)

#define PWM_MAX              200u

// ---- Sistem dosyalari ----
#define HWMON_DIR            "/sys/class/hwmon"
#define BAT_PATH             "/sys/class/power_supply/BAT0"
#define KBD_LED              "/sys/class/leds/uniwill:multicolor:kbd_backlight"
#define RAPL_DIR             "/sys/class/powercap/intel-rapl:0"
#define BACKLIGHT_DIR        "/sys/class/backlight"
#define CPUFREQ_DIR          "/sys/devices/system/cpu/cpufreq"
#define CPU_SYS_DIR          "/sys/devices/system/cpu"
#define AUTO_CONFIG_DIR      "/etc/openmonster"
#define AUTO_CONFIG_PATH     AUTO_CONFIG_DIR "/autoprofile.conf"

#define FAN_SAFETY_TEMP      70
#define FAN_SAFETY_MIN_PCT   40

// ---------- yardimcilar ----------
static void trim(char *s)
{
	char *p = s;
	while (isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
}

static int read_sys(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	if (!fgets(buf, (int)len, f)) { fclose(f); return -1; }
	fclose(f);
	trim(buf);
	return 0;
}

static int write_sys(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");
	if (!f) return -1;
	fputs(content, f);
	fclose(f);
	return 0;
}

static int led_write(const char *file, const char *val)
{
	char path[256];
	snprintf(path, sizeof(path), KBD_LED "/%s", file);
	return write_sys(path, val);
}

static int led_read(const char *file, char *buf, size_t len)
{
	char path[256];
	snprintf(path, sizeof(path), KBD_LED "/%s", file);
	return read_sys(path, buf, len);
}

// hwmon numarasi boot'lar arasi degisebilir; "uniwill" adli hwmon'u bul
static const char *uniwill_hwmon(void)
{
	static char dir[96];
	if (dir[0]) return dir;
	for (int i = 0; i < 64; i++) {
		char p[128], name[64];
		snprintf(p, sizeof(p), HWMON_DIR "/hwmon%d/name", i);
		if (read_sys(p, name, sizeof(name)) == 0 && strcmp(name, "uniwill") == 0) {
			snprintf(dir, sizeof(dir), HWMON_DIR "/hwmon%d", i);
			return dir;
		}
	}
	snprintf(dir, sizeof(dir), HWMON_DIR "/hwmon8"); // eski davranisa dus
	return dir;
}

// acpi_call ile EC oku; basari: 0, val = EC degeri (0..255)
static int ec_read_raw(uint16_t addr, uint8_t *val)
{
	char req[64], res[64];
	FILE *f = fopen(ACPI_CALL_PATH, "w");
	if (!f) return -1;
	snprintf(req, sizeof(req), "%s %u", EC_METHOD_READ, addr);
	fputs(req, f);
	fclose(f);

	f = fopen(ACPI_CALL_PATH, "r");
	if (!f) return -1;
	if (!fgets(res, sizeof(res), f)) { fclose(f); return -1; }
	fclose(f);
	trim(res);

	if (strncmp(res, "0x", 2) != 0) return -1;      // hata / uyumsuz
	unsigned long v = strtoul(res + 2, NULL, 16);
	if (v > 255) return -1;
	*val = (uint8_t)v;
	return 0;
}

// acpi_call ile EC yaz; basari: 0
static int ec_write_raw(uint16_t addr, uint8_t data)
{
	char req[80];
	FILE *f = fopen(ACPI_CALL_PATH, "w");
	if (!f) return -1;
	snprintf(req, sizeof(req), "%s %u %u", EC_METHOD_WRITE, addr, data);
	fputs(req, f);
	fclose(f);

	// yazma geri bildirimi (baz BIOS'larda bos doner): cikti oku-yok say
	f = fopen(ACPI_CALL_PATH, "r");
	if (!f) return -1;
	char res[32];
	if (fgets(res, sizeof(res), f)) { /* no-op */ }
	fclose(f);
	return 0;
}

// throttle korumasi: yazma sonrasi oku-dogrula (istege bagli kullanilir)

static float fan_rpm_sys(int fan)
{
	char path[128], buf[32];
	snprintf(path, sizeof(path), "%s/fan%d_input", uniwill_hwmon(), fan);
	if (read_sys(path, buf, sizeof(buf)) == 0) return (float)atoi(buf);
	return -1.0f;
}

static unsigned int pwm_sys(int fan)
{
	char path[128], buf[32];
	snprintf(path, sizeof(path), "%s/pwm%d", uniwill_hwmon(), fan);
	if (read_sys(path, buf, sizeof(buf)) == 0) return (unsigned int)atoi(buf);
	return 0;
}

static float temp_sys(const char *name)
{
	char path[128], buf[32];
	snprintf(path, sizeof(path), "%s/%s", uniwill_hwmon(), name);
	if (read_sys(path, buf, sizeof(buf)) == 0)
		return atof(buf) / 1000.0f;
	return -1.0f;
}

// ---------- komutlar ----------
static int cmd_status(void)
{
	// hwmon sysfs (uniwill-laptop dongu)
	float cpu_t = temp_sys("temp1_input");
	float gpu_t = temp_sys("temp2_input");

	// EC tarafindan teyit
	uint8_t cpu_ec = 0, gpu_ec = 0;
	unsigned char ec_ok = ec_read_raw(EC_CPU_TEMP, &cpu_ec) == 0 &&
			      ec_read_raw(EC_GPU_TEMP, &gpu_ec) == 0;

	uint8_t ap_oem = 0, man = 0, charge = 0;
	int has_charge = ec_read_raw(EC_CHARGE_CTRL, &charge) == 0;

	printf("==================== monster-ctl ====================\n");
	printf("Sicakliklar:\n");
	printf("  CPU : hwmon %5.1f C | EC %u C\n",
	       cpu_t, ec_ok ? (unsigned)cpu_ec : 0);
	printf("  GPU : hwmon %5.1f C | EC %u C\n",
	       gpu_t, ec_ok ? (unsigned)gpu_ec : 0);
	printf("Fanlar:\n");
	for (int i = 1; i <= 2; i++) {
		float rpm = fan_rpm_sys(i);
		unsigned int duty = pwm_sys(i);
		printf("  Fan%d : rpm=%6.0f  duty(hwmon)=%3u/200  (%d%%)\n",
		       i, rpm, duty, duty * 100 / 200);
	}
	if (ec_read_raw(EC_MANUAL_FAN_CTRL, &man) == 0) {
		printf("  Mod : %s (0x%02x)\n",
		       (man & 0x80) ? "MANUEL (FAN_MODE_USER)" : "EC otomatik", man);
	}
	if (ec_read_raw(EC_AP_OEM, &ap_oem) == 0) {
		printf("  OEM : 0x%02x (manual bit: %s)\n",
		       ap_oem, ap_oem & 1 ? "ACIK" : "kapali");
	}

	printf("Batarya:\n");
	if (has_charge) {
		if (charge & 0x80)
			printf("  Sarj limiti: %u%% (ULASILDI)\n", charge & 0x7F);
		else if ((charge & 0x7F) == 0)
			printf("  Sarj limiti: YOK (full dolduruyor)\n");
		else
			printf("  Sarj limiti: %u%%\n", charge & 0x7F);
	} else {
		printf("  Sarj limiti: EC okunamadi\n");
	}
	char bat[64], val[32];
	read_sys(BAT_PATH "/status", bat, sizeof(bat));
	read_sys(BAT_PATH "/capacity", val, sizeof(val));
	printf("  Durum: %s %%%s\n", bat, val);

	printf("EC bilgi:\n");
	uint8_t supp = 0;
	if (ec_read_raw(EC_SUPPORT5, &supp) == 0)
		printf("  SUPPORT5 : 0x%02x (FAN_SUPPORT=%d FAN_TURBO=%d)\n",
		       supp, !!((supp >> 5) & 1), !!((supp >> 4) & 1));

	printf("=====================================================\n");
	return 0;
}

// ---------- fan tablo yedekleme ----------
typedef struct {
	uint8_t t[96];   // 0:cpu_end 16, 16:cpu_start 16, 32:cpu_speed 16,
			 // 48:gpu_end, 64:gpu_start, 80:gpu_speed (her biri 16)
	uint8_t c5;      // 0x07C5 orijinal
	uint8_t c6;      // 0x07C6 orijinal
} fan_backup_t;

#define EC_UFC 0x07C5u  // SPLIT_TABLES bit7
#define EC_AP_OEM6 0x07C6u // ENABLE_UNIVERSAL_FAN_CTRL bit2

static int ec_read_block_base(uint16_t base, uint8_t out[96])
{
	for (int i = 0; i < 96; i++)
		if (ec_read_raw((uint16_t)(base + (uint16_t)i), &out[i]) != 0)
			return -1;
	return 0;
}

static int ec_write_block_base(uint16_t base, const uint8_t in[96])
{
	for (int i = 0; i < 96; i++)
		if (ec_write_raw((uint16_t)(base + (uint16_t)i), in[i]) != 0)
			return -1;
	return 0;
}

static int fan_backup_if_needed(void)
{
	if (access(TABLE_BACKUP_PATH, F_OK) == 0)
		return 0;
	(void)system("mkdir -p /var/lib/monster-ctrl 2>/dev/null");
	fan_backup_t b;
	if (ec_read_block_base(0x0F00u, b.t) != 0) return -1;
	if (ec_read_raw(EC_UFC, &b.c5) != 0) return -1;
	if (ec_read_raw(EC_AP_OEM6, &b.c6) != 0) return -1;
	FILE *f = fopen(TABLE_BACKUP_PATH, "wb");
	if (!f) return -1;
	fwrite(&b, sizeof(b), 1, f);
	fclose(f);
	printf("Orijinal EC fan durumu yedeklendi: %s\n", TABLE_BACKUP_PATH);
	return 0;
}

static int fan_restore_backup(void)
{
	FILE *f = fopen(TABLE_BACKUP_PATH, "rb");
	if (!f) {
		fprintf(stderr, "Yedek yok (%s)\n", TABLE_BACKUP_PATH);
		return -1;
	}
	fan_backup_t b;
	if (fread(&b, sizeof(b), 1, f) != 1) { fclose(f); return -1; }
	fclose(f);

	if (ec_write_block_base(0x0F00u, b.t) != 0) return -1;
	if (ec_write_raw(EC_UFC, b.c5) != 0) return -1;
	if (ec_write_raw(EC_AP_OEM6, b.c6) != 0) return -1;
	return 0;
}

// yuzde -> EC fan hizi (0..200). Tuxedo surucusuyle ayni esikler:
// %12 alti = dur(1), %12..%24 = min calisma %25, uzeri dogrudan
static uint8_t fan_speed_from_pct(int pct)
{
	int speed = pct * (int)PWM_MAX / 100;
	if (speed < 25 * (int)PWM_MAX / 2 / 100) speed = 0;
	else if (speed < 25 * (int)PWM_MAX / 100) speed = 25 * (int)PWM_MAX / 100;
	if (speed == 0) speed = 1; // EC'de 0 => 3 dk. %30 calisma; 1 => durma
	return (uint8_t)speed;
}

// universal tabloyu tuxedo ile ayni sekilde kur (tek gecerli bolge + dummy)
static int fan_universal_init(uint8_t cpu_speed, uint8_t gpu_speed)
{
	uint8_t v;

	// "full fan mode" kapali olmali (0x0751 bit6)
	if (ec_read_raw(EC_MANUAL_FAN_CTRL, &v) == 0 && (v & 0x40))
		if (ec_write_raw(EC_MANUAL_FAN_CTRL, (uint8_t)(v & ~0x40)) != 0) return -1;

	if (ec_read_raw(EC_UFC, &v) == 0 && !(v & 0x80))
		if (ec_write_raw(EC_UFC, (uint8_t)(v | 0x80)) != 0) return -1;

	for (int i = 0; i < 16; i++) {
		uint8_t start = (i == 0) ? 0 : (uint8_t)(115 + i);
		uint8_t end   = (i == 0) ? 115 : (uint8_t)(116 + i);
		uint8_t spd   = (i == 0) ? cpu_speed : 0xC8;
		uint8_t gspd  = (i == 0) ? gpu_speed : 0xC8;
		uint8_t gend  = (i == 0) ? 120 : (uint8_t)(116 + i);
		if (ec_write_raw((uint16_t)(0x0F00u + i), end) != 0 ||
		    ec_write_raw((uint16_t)(0x0F10u + i), start) != 0 ||
		    ec_write_raw((uint16_t)(0x0F20u + i), spd) != 0 ||
		    ec_write_raw((uint16_t)(0x0F30u + i), gend) != 0 ||
		    ec_write_raw((uint16_t)(0x0F40u + i), start) != 0 ||
		    ec_write_raw((uint16_t)(0x0F50u + i), gspd) != 0)
			return -1;
	}

	if (ec_read_raw(EC_AP_OEM6, &v) == 0 && !(v & 0x04))
		if (ec_write_raw(EC_AP_OEM6, (uint8_t)(v | 0x04)) != 0) return -1;
	return 0;
}

// asil kontrol: dogrudan hiz registeri + tablo giris 0
static int fan_apply_direct(int fan_idx, uint8_t speed)
{
	uint16_t table  = fan_idx == 0 ? EC_CPU_FAN_SPEED_TABLE : EC_GPU_FAN_SPEED_TABLE;
	uint16_t direct = fan_idx == 0 ? EC_FAN0_DIRECT : EC_FAN1_DIRECT;
	if (ec_write_raw(direct, speed) != 0) return -1;
	if (ec_write_raw(table, speed) != 0) return -1;
	return 0;
}

// daemon icin: iki fani da dogrudan sur, bekleme yapmadan
static int fan_set_pct_direct(int pct)
{
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	uint8_t speed = fan_speed_from_pct(pct);
	if (fan_backup_if_needed() != 0) return -1;
	if (fan_universal_init(speed, speed) != 0) return -1;
	if (fan_apply_direct(0, speed) != 0) return -1;
	if (fan_apply_direct(1, speed) != 0) return -1;
	return 0;
}

static void fan_dump(void)
{
	uint8_t t[96];
	if (ec_read_block_base(0x0F00u, t) != 0) {
		fprintf(stderr, "EC tablo okunamadi\n");
		return;
	}
	printf("idx | CPU: end->start->speed | GPU: end->start->speed | duty\n");
	for (int i = 0; i < 16; i++) {
		printf("%2d  | CPU %3d %3d %3d | GPU %3d %3d %3d\n",
		       i, t[i], t[16 + i], t[32 + i],
		       t[48 + i], t[64 + i], t[80 + i]);
	}
}

static int cmd_fan(int argc, char **argv)
{
	if (argc < 2 || strlen(argv[1]) == 0) {
		fprintf(stderr,
			"kullanim: monsterctl fan <0-100> [--fan main|secondary|both]\n"
			"           monsterctl fan auto     (orijinal tabloya geri don)\n"
			"           monsterctl fan dump     (mevcut EC fan tablosunu goster)\n"
			"           monsterctl fan diag     (EC tesisi: mod/flag/register durumu)\n");
		return 2;
	}

	if (strcmp(argv[1], "dump") == 0) {
		fan_dump();
		return 0;
	}

	if (strcmp(argv[1], "diag") == 0) {
		uint8_t v = 0, a = 0, b = 0;
		printf("EC teshis:\n");
		if (ec_read_raw(EC_MANUAL_FAN_CTRL, &v) == 0)
			printf("  0x0751 fan modu     : 0x%02x%s\n", v,
			       (v & 0x40) ? "  [FULL FAN MODU]" : "");
		if (ec_read_raw(EC_UFC, &v) == 0)
			printf("  0x07C5 split tables : 0x%02x%s\n", v,
			       (v & 0x80) ? "  [ACIK]" : "");
		if (ec_read_raw(EC_AP_OEM6, &v) == 0)
			printf("  0x07C6 universal    : 0x%02x%s\n", v,
			       (v & 0x04) ? "  [ACIK]" : "");
		if (ec_read_raw(EC_FAN0_DIRECT, &a) == 0 && ec_read_raw(EC_FAN1_DIRECT, &b) == 0)
			printf("  0x1804 / 0x1809     : %u / %u\n", a, b);
		if (ec_read_raw(EC_CPU_FAN_SPEED_TABLE, &a) == 0 &&
		    ec_read_raw(EC_GPU_FAN_SPEED_TABLE, &b) == 0)
			printf("  0x0F20 / 0x0F50     : %u / %u\n", a, b);
		printf("  hwmon pwm1=%u pwm2=%u | Fan1=%.0f rpm Fan2=%.0f rpm\n",
		       pwm_sys(1), pwm_sys(2), fan_rpm_sys(1), fan_rpm_sys(2));
		return 0;
	}

	if (strcmp(argv[1], "factory") == 0) {
		// EC tablosu sifir + universal kapali = EC kendi iceriksel (BIOS) egrisi
		uint8_t z96[96]; memset(z96, 0, sizeof(z96));
		if (ec_write_block_base(0x0F00u, z96) != 0) { fprintf(stderr, "tablo sifirlanamadi\n"); return 1; }
		ec_write_raw(EC_UFC, 0x00);       // SPLIT_TABLES kapali
		ec_write_raw(EC_AP_OEM6, 0x00);   // UNIVERSAL_FAN_CTRL kapali
		remove(TABLE_BACKUP_PATH);        // yedek artik gecersiz, sil
		sleep(2);
		printf("Fan sanayi ayarlarina dondu (EC BIOS egrisi).\n");
		printf("  duty1=%u duty2=%u | Fan1 rpm=%.0f  Fan2 rpm=%.0f\n",
		       pwm_sys(1), pwm_sys(2), fan_rpm_sys(1), fan_rpm_sys(2));
		return 0;
	}

	if (strcmp(argv[1], "auto") == 0) {
		if (fan_restore_backup() == 0) {
			sleep(2);
			printf("Fan tablolari OEM otomatik davranisina geri yuklendi.\n");
			printf("  duty1=%u duty2=%u | Fan1 rpm=%.0f  Fan2 rpm=%.0f\n",
			       pwm_sys(1), pwm_sys(2), fan_rpm_sys(1), fan_rpm_sys(2));
		}
		return 0;
	}

	int pct = atoi(argv[1]);
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;

	const char *which = "both";
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--fan") == 0 && i + 1 < argc)
			which = argv[++i];
	}
	int write_cpu = !strcmp(which, "main") || !strcmp(which, "both");
	int write_gpu = !strcmp(which, "secondary") || !strcmp(which, "both");

	uint8_t speed = fan_speed_from_pct(pct);

	if (fan_backup_if_needed() != 0) {
		fprintf(stderr, "yedekleme yapilamadi\n");
		return 1;
	}
	if (fan_universal_init(speed, speed) != 0) {
		fprintf(stderr, "universal fan tablosu kurulamadi\n");
		return 1;
	}

	int ok = 1;
	if (write_cpu && fan_apply_direct(0, speed) != 0) ok = 0;
	if (write_gpu && fan_apply_direct(1, speed) != 0) ok = 0;
	if (!ok) {
		fprintf(stderr, "EC hiz yazimi basarisiz (acpi_call/izin?)\n");
		return 1;
	}

	sleep(2);
	printf("Fan -> %d%% (EC hizi=%u/%u)\n", pct, speed, PWM_MAX);
	printf("  EC duty1=%u  duty2=%u | Fan1 rpm=%.0f  Fan2 rpm=%.0f\n",
	       pwm_sys(1), pwm_sys(2), fan_rpm_sys(1), fan_rpm_sys(2));
	printf("Orijinal tabloya donmek: sudo monsterctl fan auto\n");
	return 0;
}

static int cmd_battery(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "kullanim: monsterctl battery <10-100|off|status>\n");
		return 2;
	}
	if (strcmp(argv[1], "status") == 0) {
		uint8_t v = 0;
		if (ec_read_raw(EC_CHARGE_CTRL, &v) == 0) {
			if (v & 0x80) printf("Sarj limiti: %u%%, ULASILDI\n", v & 0x7F);
			else if ((v & 0x7F) == 0) printf("Sarj limiti: yok (100%%)\n");
			else printf("Sarj limiti: %u%%\n", v & 0x7F);
		} else fprintf(stderr, "EC okunamadi\n");
		return 0;
	}
	if (strcmp(argv[1], "off") == 0) {
		ec_write_raw(EC_CHARGE_CTRL, 0x00);
		printf("Sarj limiti kapatildi (100%% dolacak).\n");
		return 0;
	}
	int pct = atoi(argv[1]);
	if (pct < 10 || pct > 100) {
		fprintf(stderr, "Limit 10-100 araliginda olmali.\n");
		return 2;
	}
	if (pct == 100) { ec_write_raw(EC_CHARGE_CTRL, 0x00); printf("Limit kapatildi (100%%).\n"); return 0; }
	ec_write_raw(EC_CHARGE_CTRL, (uint8_t)pct);
	printf("Sarj limiti: %d%% yapildi.\n", pct);
	return 0;
}

// ---------- klavye RGB (multicolor LED sysfs) ----------
static int cmd_kbd(int argc, char **argv)
{
	char buf[96];
	if (led_read("max_brightness", buf, sizeof(buf)) != 0) {
		fprintf(stderr, "Klavye LED bulunamadi: %s\n", KBD_LED);
		return 1;
	}
	int maxb = atoi(buf);
	if (maxb <= 0) maxb = 255;

	if (argc < 2 || strcmp(argv[1], "status") == 0) {
		int bri = 0;
		char col[96] = "-";
		if (led_read("brightness", buf, sizeof(buf)) == 0) bri = atoi(buf);
		if (led_read("multi_intensity", buf, sizeof(buf)) == 0)
			snprintf(col, sizeof(col), "%s", buf);
		printf("Klavye RGB: parlaklik %d/%d (%d%%) | Renk (R G B): %s\n",
		       bri, maxb, bri * 100 / maxb, col);
		return 0;
	}

	if (strcmp(argv[1], "on") == 0) {
		char v[16];
		snprintf(v, sizeof(v), "%d", maxb);
		if (led_write("brightness", v) != 0) { fprintf(stderr, "LED yazilamadi (root?)\n"); return 1; }
		printf("Klavye isigi: TAM (%d/%d)\n", maxb, maxb);
		return 0;
	}
	if (strcmp(argv[1], "off") == 0) {
		if (led_write("brightness", "0") != 0) { fprintf(stderr, "LED yazilamadi (root?)\n"); return 1; }
		printf("Klavye isigi: KAPALI\n");
		return 0;
	}
	if (strcmp(argv[1], "color") == 0) {
		if (argc < 5) {
			fprintf(stderr, "kullanim: monsterctl kbd color <R> <G> <B>  (0-255)\n");
			return 2;
		}
		int r = atoi(argv[2]), g = atoi(argv[3]), b = atoi(argv[4]);
		if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
			fprintf(stderr, "R/G/B degerleri 0-255 araliginda olmali.\n");
			return 2;
		}
		// LED kanallari 0..multi_max_intensity (genelde 0..50) araligini kabul eder;
		// 0-255 degerleri bu araliga olcekle.
		int mr = 50, mg = 50, mb = 50;
		if (led_read("multi_max_intensity", buf, sizeof(buf)) == 0)
			(void)sscanf(buf, "%d %d %d", &mr, &mg, &mb);
		if (mr <= 0) mr = 255;
		if (mg <= 0) mg = 255;
		if (mb <= 0) mb = 255;
		int sr = r * mr / 255, sg = g * mg / 255, sb = b * mb / 255;
		if (r > 0 && sr == 0) sr = 1;   // tamamen kararmasin
		if (g > 0 && sg == 0) sg = 1;
		if (b > 0 && sb == 0) sb = 1;

		char v[32];
		snprintf(v, sizeof(v), "%d %d %d", sr, sg, sb);
		if (led_write("multi_intensity", v) != 0) {
			fprintf(stderr, "LED yazilamadi (root?)\n");
			return 1;
		}
		// isik kapaliysa renk gorunmez; parlakligi ac
		if (led_read("brightness", buf, sizeof(buf)) == 0 && atoi(buf) == 0) {
			char b2[16];
			snprintf(b2, sizeof(b2), "%d", maxb);
			led_write("brightness", b2);
			printf("Parlaklik 0 idi, acildi (%d/%d).\n", maxb, maxb);
		}
		printf("Klavye rengi: R=%d G=%d B=%d -> LED %d %d %d (max %d/%d/%d)\n",
		       r, g, b, sr, sg, sb, mr, mg, mb);
		return 0;
	}

	// parlaklik yuzdesi
	int pct = atoi(argv[1]);
	if (pct < 0 || pct > 100) {
		fprintf(stderr,
			"kullanim: monsterctl kbd <0-100|on|off|status>\n"
			"          monsterctl kbd color <R> <G> <B>\n");
		return 2;
	}
	char v[16];
	snprintf(v, sizeof(v), "%d", maxb * pct / 100);
	if (led_write("brightness", v) != 0) { fprintf(stderr, "LED yazilamadi (root?)\n"); return 1; }
	printf("Klavye parlaklik: %d%% (%d/%d)\n", pct, maxb * pct / 100, maxb);
	return 0;
}

// ---------- ekran parlakligi ----------
static int backlight_find(char *out, size_t len)
{
	DIR *d = opendir(BACKLIGHT_DIR);
	if (!d) return -1;
	struct dirent *e;
	int found = -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char p[256];
		snprintf(p, sizeof(p), BACKLIGHT_DIR "/%.200s/max_brightness", e->d_name);
		if (access(p, R_OK) == 0) {
			snprintf(out, len, BACKLIGHT_DIR "/%.200s", e->d_name);
			found = 0;
			if (strstr(e->d_name, "intel_backlight")) break;
		}
	}
	closedir(d);
	return found;
}

static int backlight_get(const char *path, long *cur, long *max)
{
	char p[320], buf[64];
	snprintf(p, sizeof(p), "%s/max_brightness", path);
	if (read_sys(p, buf, sizeof(buf)) != 0) return -1;
	*max = atol(buf);
	snprintf(p, sizeof(p), "%s/brightness", path);
	if (read_sys(p, buf, sizeof(buf)) != 0) return -1;
	*cur = atol(buf);
	return *max > 0 ? 0 : -1;
}

static int cmd_display(int argc, char **argv)
{
	char bl[256];
	if (backlight_find(bl, sizeof(bl)) != 0) {
		fprintf(stderr, "Ekran parlakligi aygiti bulunamadi (%s).\n", BACKLIGHT_DIR);
		return 1;
	}
	long cur = 0, max = 0;
	if (backlight_get(bl, &cur, &max) != 0) {
		fprintf(stderr, "Parlaklik okunamadi: %s\n", bl);
		return 1;
	}
	int pct_now = (int)(cur * 100 / max);

	if (argc < 2 || strcmp(argv[1], "status") == 0) {
		printf("Ekran parlakligi: %d%% (%ld/%ld) — %s\n", pct_now, cur, max, bl);
		return 0;
	}
	if (strcmp(argv[1], "brightness") != 0 || argc < 3) {
		fprintf(stderr, "kullanim: monsterctl display [status|brightness <1-100>]\n");
		return 2;
	}

	int pct = atoi(argv[2]);
	if (pct < 1 || pct > 100) {
		fprintf(stderr, "Parlaklik 1-100 araliginda olmali.\n");
		return 2;
	}
	long val = max * pct / 100;
	if (val < 1) val = 1;
	char p[320], v[32];
	snprintf(p, sizeof(p), "%s/brightness", bl);
	snprintf(v, sizeof(v), "%ld", val);
	if (write_sys(p, v) != 0) {
		fprintf(stderr, "Parlaklik yazilamadi (root gerekli?).\n");
		return 1;
	}
	printf("Ekran parlakligi: %d%% -> %ld/%ld\n", pct, val, max);
	return 0;
}

// ---------- CPU frekans / cekirdek kontrolu ----------
static int policy_dirs(char dirs[][256], int max)
{
	DIR *d = opendir(CPUFREQ_DIR);
	if (!d) return 0;
	struct dirent *e;
	int n = 0;
	while ((e = readdir(d)) != NULL && n < max) {
		if (strncmp(e->d_name, "policy", 6) != 0) continue;
		snprintf(dirs[n], 256, CPUFREQ_DIR "/%.200s", e->d_name);
		n++;
	}
	closedir(d);
	// policy10, policy2 ... seklinde gelebilir; siralama onemsiz.
	return n;
}

static int list_has_token(const char *list, const char *tok)
{
	size_t tl = strlen(tok);
	const char *p = list;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == '\n') p++;
		const char *s = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
		if ((size_t)(p - s) == tl && strncmp(s, tok, tl) == 0) return 1;
	}
	return 0;
}

static int cpu_apply_choice(const char *attr, const char *avail_attr,
			    const char *value, const char *label)
{
	char dirs[64][256];
	int n = policy_dirs(dirs, 64);
	if (n == 0) {
		fprintf(stderr, "cpufreq politikalari bulunamadi.\n");
		return 1;
	}
	int ok = 0, skipped = 0;
	for (int i = 0; i < n; i++) {
		char p[512], avail[256];
		snprintf(p, sizeof(p), "%.255s/%.48s", dirs[i], avail_attr);
		if (read_sys(p, avail, sizeof(avail)) == 0 && !list_has_token(avail, value)) {
			skipped++;
			continue;
		}
		snprintf(p, sizeof(p), "%.255s/%.48s", dirs[i], attr);
		if (write_sys(p, value) == 0) ok++;
		else skipped++;
	}
	if (ok == 0) {
		fprintf(stderr, "%s yazilamadi (root? desteklenmiyor?).\n", label);
		return 1;
	}
	printf("CPU %s: %s (%d politika%s)\n", label, value, ok,
	       skipped ? ", bazi politikalarda desteklenmiyor" : "");
	return 0;
}

static int cpu_set_max_pct(int pct)
{
	if (pct < 20 || pct > 100) {
		fprintf(stderr, "Maksimum frekans yuzdesi 20-100 araliginda olmali.\n");
		return 2;
	}
	char dirs[64][256];
	int n = policy_dirs(dirs, 64);
	if (n == 0) {
		fprintf(stderr, "cpufreq politikalari bulunamadi.\n");
		return 1;
	}
	int ok = 0;
	long long last = 0;
	for (int i = 0; i < n; i++) {
		char p[512], buf[64];
		long long maxf = 0, minf = 0;
		snprintf(p, sizeof(p), "%.255s/cpuinfo_max_freq", dirs[i]);
		if (read_sys(p, buf, sizeof(buf)) != 0) continue;
		maxf = atoll(buf);
		snprintf(p, sizeof(p), "%.255s/cpuinfo_min_freq", dirs[i]);
		if (read_sys(p, buf, sizeof(buf)) == 0) minf = atoll(buf);
		long long target = maxf * pct / 100;
		if (minf > 0 && target < minf) target = minf;
		snprintf(p, sizeof(p), "%.255s/scaling_max_freq", dirs[i]);
		char v[32];
		snprintf(v, sizeof(v), "%lld", target);
		if (write_sys(p, v) == 0) { ok++; last = target; }
	}
	if (ok == 0) {
		fprintf(stderr, "Maksimum frekans yazilamadi (root? intel_pstate kisiti?).\n");
		return 1;
	}
	printf("CPU maksimum frekansi: %d%% -> %.2f GHz (%d politika)\n",
	       pct, (double)last / 1e6, ok);
	return 0;
}

static int cpu_total_count(void)
{
	long n = sysconf(_SC_NPROCESSORS_CONF);
	return n > 0 ? (int)n : 1;
}

static int cpu_online_count(void)
{
	int total = cpu_total_count(), on = 0;
	for (int i = 0; i < total; i++) {
		char p[256], b[16];
		snprintf(p, sizeof(p), CPU_SYS_DIR "/cpu%d/online", i);
		if (read_sys(p, b, sizeof(b)) != 0) { on++; continue; } // cpu0 online dosyasi yok
		if (atoi(b) == 1) on++;
	}
	return on;
}

static int cpu_set_cores(int n)
{
	int total = cpu_total_count();
	if (n < 1 || n > total) {
		fprintf(stderr, "Cekirdek sayisi 1-%d araliginda olmali.\n", total);
		return 2;
	}
	int ok = 0;
	for (int i = 1; i < total; i++) {
		char p[256], v[8];
		snprintf(p, sizeof(p), CPU_SYS_DIR "/cpu%d/online", i);
		if (access(p, W_OK) != 0 && access(p, F_OK) != 0) continue;
		snprintf(v, sizeof(v), "%d", i < n ? 1 : 0);
		if (write_sys(p, v) == 0) ok++;
	}
	printf("CPU cekirdek sayisi: %d/%d (online: %d)\n", n, total, cpu_online_count());
	return ok > 0 ? 0 : 1;
}

static int cmd_cpu(int argc, char **argv)
{
	if (argc < 2 || strcmp(argv[1], "status") == 0) {
		char dirs[64][256], gov[64] = "-", epp[64] = "-", buf[64];
		int n = policy_dirs(dirs, 64);
		if (n > 0) {
			char p[512];
			snprintf(p, sizeof(p), "%.255s/scaling_governor", dirs[0]);
			(void)read_sys(p, gov, sizeof(gov));
			snprintf(p, sizeof(p), "%.255s/energy_performance_preference", dirs[0]);
			(void)read_sys(p, epp, sizeof(epp));
			long long maxf = 0, curf = 0;
			snprintf(p, sizeof(p), "%.255s/cpuinfo_max_freq", dirs[0]);
			if (read_sys(p, buf, sizeof(buf)) == 0) maxf = atoll(buf);
			snprintf(p, sizeof(p), "%.255s/scaling_cur_freq", dirs[0]);
			if (read_sys(p, buf, sizeof(buf)) == 0) curf = atoll(buf);
			printf("CPU durumu:\n");
			printf("  governor : %s\n", gov);
			printf("  EPP      : %s\n", epp);
			printf("  frekans  : %.2f / %.2f GHz\n",
			       (double)curf / 1e6, (double)maxf / 1e6);
			printf("  cekirdek : %d/%d online\n", cpu_online_count(), cpu_total_count());
			printf("  politika : %d\n", n);
			return 0;
		}
		fprintf(stderr, "cpufreq bilgisi bulunamadi.\n");
		return 1;
	}

	if (strcmp(argv[1], "governor") == 0) {
		if (argc < 3) { fprintf(stderr, "kullanim: monsterctl cpu governor <performance|powersave>\n"); return 2; }
		return cpu_apply_choice("scaling_governor", "scaling_available_governors",
					argv[2], "governor");
	}
	if (strcmp(argv[1], "epp") == 0) {
		if (argc < 3) {
			fprintf(stderr, "kullanim: monsterctl cpu epp <default|performance|balance_performance|balance_power|power>\n");
			return 2;
		}
		return cpu_apply_choice("energy_performance_preference",
					"energy_performance_available_preferences",
					argv[2], "EPP");
	}
	if (strcmp(argv[1], "max") == 0) {
		if (argc < 3) { fprintf(stderr, "kullanim: monsterctl cpu max <20-100>\n"); return 2; }
		return cpu_set_max_pct(atoi(argv[2]));
	}
	if (strcmp(argv[1], "cores") == 0) {
		if (argc < 3) { fprintf(stderr, "kullanim: monsterctl cpu cores <1-%d>\n", cpu_total_count()); return 2; }
		return cpu_set_cores(atoi(argv[2]));
	}

	fprintf(stderr,
		"kullanim: monsterctl cpu status\n"
		"          monsterctl cpu governor <performance|powersave>\n"
		"          monsterctl cpu epp <default|performance|balance_performance|balance_power|power>\n"
		"          monsterctl cpu max <20-100>\n"
		"          monsterctl cpu cores <1-%d>\n", cpu_total_count());
	return 2;
}

// ---------- CPU guc limitleri (PL1/PL2) ----------
// Birincil arayuz intel-rapl (powercap); EC 0x0783/0x0784 yalnizca bilgi amacli okunur.
static int rapl_find_constraint(const char *kind)
{
	for (int i = 0; i < 4; i++) {
		char p[128], name[64];
		snprintf(p, sizeof(p), RAPL_DIR "/constraint_%d_name", i);
		if (read_sys(p, name, sizeof(name)) == 0 && strcmp(name, kind) == 0)
			return i;
	}
	return -1;
}

static long long rapl_read_uw(int idx)
{
	char p[128], buf[64];
	snprintf(p, sizeof(p), RAPL_DIR "/constraint_%d_power_limit_uw", idx);
	if (read_sys(p, buf, sizeof(buf)) != 0) return -1;
	return atoll(buf);
}

static int rapl_set_w(const char *kind, int watts)
{
	int idx = rapl_find_constraint(kind);
	if (idx < 0) {
		fprintf(stderr, "RAPL %s kosulu bulunamadi.\n", kind);
		return 1;
	}

	// teknoloji limitleri (varsa)
	char p[128], buf[64];
	long long max_uw = -1, min_uw = -1;
	snprintf(p, sizeof(p), RAPL_DIR "/constraint_%d_max_power_limit_uw", idx);
	if (read_sys(p, buf, sizeof(buf)) == 0) max_uw = atoll(buf);
	if (max_uw <= 0) {
		snprintf(p, sizeof(p), RAPL_DIR "/constraint_%d_max_power_uw", idx);
		if (read_sys(p, buf, sizeof(buf)) == 0) max_uw = atoll(buf);
	}
	snprintf(p, sizeof(p), RAPL_DIR "/constraint_%d_min_power_limit_uw", idx);
	if (read_sys(p, buf, sizeof(buf)) == 0) min_uw = atoll(buf);

	if (watts < 5) {
		fprintf(stderr, "Guc limiti en az 5 W olmali.\n");
		return 2;
	}
	if (max_uw > 0 && (long long)watts * 1000000 > max_uw) {
		fprintf(stderr, "Limit cok yuksek (maks %.0f W).\n", (double)max_uw / 1e6);
		return 2;
	}
	if (min_uw > 0 && (long long)watts * 1000000 < min_uw) {
		fprintf(stderr, "Limit cok dusuk (min %.0f W).\n", (double)min_uw / 1e6);
		return 2;
	}

	long long cur = rapl_read_uw(idx);
	if (cur >= 0 && (long long)watts * 1000000 == cur) {
		printf("PL (%s) zaten %.0f W.\n", kind, (double)cur / 1e6);
		return 0;
	}

	snprintf(p, sizeof(p), RAPL_DIR "/constraint_%d_power_limit_uw", idx);
	char val[32];
	snprintf(val, sizeof(val), "%lld", (long long)watts * 1000000);
	if (write_sys(p, val) != 0) {
		fprintf(stderr, "RAPL yazma basarisiz (root gerekli ya da kilitli?).\n");
		return 1;
	}

	long long now = rapl_read_uw(idx);
	printf("CPU %s: %.0f W -> %s (%.0f W)\n",
	       kind, (double)cur / 1e6, val, now >= 0 ? (double)now / 1e6 : -1);
	if (now >= 0 && now != (long long)watts * 1000000)
		fprintf(stderr, "UYARI: yazilan deger teyit edilemedi.\n");
	return 0;
}

static int read_ll(const char *path, long long *out)
{
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int ok = fscanf(f, "%lld", out) == 1;
	fclose(f);
	return ok ? 0 : -1;
}

// RAPL enerji sayacindan anlik paket gucu (W)
static double pkg_power_w(void)
{
	long long e1 = 0, e2 = 0, emax = 0;
	if (read_ll(RAPL_DIR "/energy_uj", &e1) != 0) return -1.0;
	(void)read_ll(RAPL_DIR "/max_energy_range_uj", &emax);
	usleep(200000);
	if (read_ll(RAPL_DIR "/energy_uj", &e2) != 0) return -1.0;
	double d = (double)(e2 - e1);
	if (d < 0 && emax > 0) d += (double)emax; // sayac tasmasi
	return d / 0.2 / 1e6;
}

static int cmd_power(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"kullanim: monsterctl power status\n"
			"          monsterctl power stat        (anlik paket gucu, W)\n"
			"          monsterctl power pl1 <W>        (surekli limit)\n"
			"          monsterctl power pl2 <W>        (kisa sureli/boost limit)\n"
			"          monsterctl power pl4 <W>        (peak power)\n"
			"          monsterctl power <pl1> <pl2>    (ikisi birden)\n");
		return 2;
	}

	if (strcmp(argv[1], "status") == 0) {
		struct { const char *kind, *label; } cons[3] = {
			{ "long_term", "PL1 (long_term)" },
			{ "short_term", "PL2 (short_term)" },
			{ "peak_power", "PL4 (peak_power)" },
		};
		printf("CPU guc limitleri (intel-rapl, package-0):\n");
		for (int i = 0; i < 3; i++) {
			int idx = rapl_find_constraint(cons[i].kind);
			if (idx < 0) { printf("  %s: bulunamadi\n", cons[i].label); continue; }
			long long uw = rapl_read_uw(idx);
			if (uw < 0) { printf("  %s: okunamadi\n", cons[i].label); continue; }
			printf("  %s: %.0f W\n", cons[i].label, (double)uw / 1e6);
		}
		// EC tarafi (bilgi)
		uint8_t pl1 = 0, pl2 = 0;
		if (ec_read_raw(EC_CPU_PL1, &pl1) == 0 && ec_read_raw(EC_CPU_PL2, &pl2) == 0)
			printf("  EC (0x0783/0x0784): PL1=%u PL2=%u (ham)\n", pl1, pl2);
		return 0;
	}

	if (strcmp(argv[1], "stat") == 0) {
		double w = pkg_power_w();
		if (w < 0) { fprintf(stderr, "paket gucu okunamadi (root?)\n"); return 1; }
		printf("%.1f W\n", w);
		return 0;
	}

	if (strcmp(argv[1], "pl1") == 0) {
		if (argc < 3) { fprintf(stderr, "kullanim: monsterctl power pl1 <W>\n"); return 2; }
		return rapl_set_w("long_term", atoi(argv[2]));
	}
	if (strcmp(argv[1], "pl2") == 0) {
		if (argc < 3) { fprintf(stderr, "kullanim: monsterctl power pl2 <W>\n"); return 2; }
		return rapl_set_w("short_term", atoi(argv[2]));
	}
	if (strcmp(argv[1], "pl4") == 0) {
		if (argc < 3) { fprintf(stderr, "kullanim: monsterctl power pl4 <W>\n"); return 2; }
		return rapl_set_w("peak_power", atoi(argv[2]));
	}

	// power <pl1> <pl2>
	int p1 = atoi(argv[1]);
	if (argc < 3) { fprintf(stderr, "kullanim: monsterctl power <pl1> <pl2>\n"); return 2; }
	int p2 = atoi(argv[2]);
	int rc = rapl_set_w("long_term", p1);
	rc |= rapl_set_w("short_term", p2);
	return rc;
}


static int cmd_monitor(int argc, char **argv)
{
	int interval = 2;
	if (argc >= 2) interval = atoi(argv[1]);
	if (interval < 1) interval = 1;
	printf("monsterctl monitor: Ctrl+C ile cik (interval=%ds)\n", interval);
	while (1) {
		float c = temp_sys("temp1_input");
		float g = temp_sys("temp2_input");
		unsigned int d1 = pwm_sys(1), d2 = pwm_sys(2);
		printf("CPU %5.1fC | GPU %5.1fC | Fan1 %5.0frpm (duty %3u) | Fan2 %5.0frpm (duty %3u)\r",
		       c, g, fan_rpm_sys(1), d1, fan_rpm_sys(2), d2);
		fflush(stdout);
		sleep((unsigned int)interval);
	}
	return 0;
}

// ---------- otomatik fan profili (M4) ----------
typedef struct {
	int temp; int pct;
} curve_pt_t;

#define DEFAULT_CURVE_LEN 6
static const curve_pt_t DEFAULT_CURVE[DEFAULT_CURVE_LEN] = {
	{ 40, 30 }, { 50, 40 }, { 60, 55 }, { 70, 70 }, { 80, 85 }, { 90, 100 },
};

static volatile sig_atomic_t fp_stop = 0;
static void fp_on_signal(int sig) { (void)sig; fp_stop = 1; }

// "40:30,50:40,..." bicimini cozer; basari: 0, dizi cagiranin odugu
static int parse_curve(const char *spec, curve_pt_t *out, int max_pts, int *out_len)
{
	int n = 0;
	const char *p = spec;
	while (*p && n < max_pts) {
		int t, d;
		char rest[8];
		int got = sscanf(p, "%d:%d%2s", &t, &d, rest);
		if (got < 2 || (got == 3 && rest[0] != ',')) return -1;
		if (t < 20 || t > 100 || d < 0 || d > 100) return -1;
		if (n > 0 && t <= out[n - 1].temp) return -1;
		out[n].temp = t;
		out[n].pct = d;
		n++;
		if (got == 3) {
			const char *q = strchr(p, ',');
			if (!q) break;
			p = q + 1;
			if (!*p) return -1; // sondaki virgul
		} else break;
	}
	if (n == 0) return -1;
	*out_len = n;
	return 0;
}

// egriye gore yuzde bul (caprazlama/lineer.enterpolasyon)
static int curve_pct(const curve_pt_t *c, int len, int temp)
{
	if (temp <= c[0].temp) return c[0].pct;
	for (int i = 1; i < len; i++) {
		if (temp <= c[i].temp) {
			int t0 = c[i - 1].temp, t1 = c[i].temp;
			int d0 = c[i - 1].pct, d1 = c[i].pct;
			return d0 + (d1 - d0) * (temp - t0) / (t1 - t0);
		}
	}
	return c[len - 1].pct;
}

static int cmd_fanprofile(int argc, char **argv)
{
	int interval = 3;
	const char *spec = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
			interval = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--curve") == 0 && i + 1 < argc) {
			spec = argv[++i];
		} else {
			fprintf(stderr,
				"kullanim: monsterctl fanprofile [--interval <sn>] [--curve <T:P,...>]\n"
				"  ornek --curve: 40:30,50:40,60:55,70:70,80:85,90:100\n"
				"  Eger uygulama SIGINT ile kesilirse OEM tablolari geri yuklenir.\n");
			return 2;
		}
	}
	if (interval < 1) interval = 1;

	curve_pt_t curve[32];
	int clen;
	if (spec) {
		if (parse_curve(spec, curve, 32, &clen) != 0) {
			fprintf(stderr, "Egri bicimi gecersiz: %s\n", spec);
			return 2;
		}
	} else {
		memcpy(curve, DEFAULT_CURVE, sizeof(DEFAULT_CURVE));
		clen = DEFAULT_CURVE_LEN;
	}

	// sinyal yakalayicilari: temiz kapanista EC tablolarini geri yukle
	struct sigaction sa = { 0 };
	sa.sa_handler = fp_on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	printf("Fan profili aktif: interval=%ds, egri: ", interval);
	for (int i = 0; i < clen; i++) printf("%dC:%d%% ", curve[i].temp, curve[i].pct);
	printf("\n");

	fan_backup_if_needed();
	uint8_t v0751 = 0;
	(void)ec_read_raw(EC_MANUAL_FAN_CTRL, &v0751);

	int last_target = -1;
	int inited = 0;
	for (;;) {
		float cpu = temp_sys("temp1_input");
		float gpu = temp_sys("temp2_input");
		if (cpu < 0 && gpu < 0) {
			// hwmon yoksa EC sicakliklariyla devam et
			uint8_t c_e = 0, g_e = 0;
			if (ec_read_raw(EC_CPU_TEMP, &c_e) == 0) cpu = (float)c_e;
			if (ec_read_raw(EC_GPU_TEMP, &g_e) == 0) gpu = (float)g_e;
		}
		if (cpu < 0 && gpu < 0) {
			fprintf(stderr, "fanprofile: sicaklik okunamadi, %d sn sonra tekrar denenecek\n", interval);
			sleep((unsigned int)interval);
			if (fp_stop) break;
		}
		float hottest = (cpu > gpu) ? cpu : gpu;
		int target = curve_pct(curve, clen, (int)hottest);

		// TCC ile ayni guvenlik tabani: 70 C uzerinde fan en az %40.
		if (hottest >= FAN_SAFETY_TEMP && target < FAN_SAFETY_MIN_PCT)
			target = FAN_SAFETY_MIN_PCT;

		if (target != last_target) {
			uint8_t speed = fan_speed_from_pct(target);
			int ok = 1;
			if (!inited) {
				ok = fan_universal_init(speed, speed) == 0;
				inited = ok;
			}
			if (ok)
				ok = fan_apply_direct(0, speed) == 0 &&
				     fan_apply_direct(1, speed) == 0;
			if (ok) last_target = target;
			else fprintf(stderr, "fanprofile: EC yazma basarisiz (acpi_call yok?)\n");
		}

		printf("CPU %.1fC | GPU %.1fC | hedef %d%%\n", cpu, gpu, target);
		fflush(stdout);
		sleep((unsigned int)interval);
		if (fp_stop) {
			printf("\nfanprofile: cikiliyor; EC tablolari OEM otomatik moduna geri yukleniyor...\n");
			fan_restore_backup();
			return 0;
		}
	}
	return 0;
}

// ---------- kalici otomatik profil / webcam servisi ----------
#define AUTO_CURVE_MAX 16

typedef struct {
	char name[64];
	char fan_mode[16];      // manual | curve | auto
	int fan;
	curve_pt_t curve[AUTO_CURVE_MAX];
	int curve_len;
	int pl1_w, pl2_w, pl4_w;
	int brightness;
	char governor[32];
	char epp[32];
	int max_freq;
	int cores;
} auto_profile_t;

typedef struct {
	int enabled;            // AC/pil otomasyonu acik mi
	int manual;             // son elle yapilan ayar, kaynak profilini gecersiz kilar
	auto_profile_t last, ac, bat;
	int web_enabled;
	char web_device[64];
	int web_has_brightness, web_brightness;
	int web_has_contrast, web_contrast;
	int web_has_saturation, web_saturation;
	int web_has_gamma, web_gamma;
	int web_has_sharpness, web_sharpness;
	int web_has_wb_auto, web_wb_auto;
	int web_has_wb_temp, web_wb_temp;
	int web_has_exposure_auto, web_exposure_auto;
	int web_has_exposure_time, web_exposure_time;
} auto_config_t;

static void auto_copy_str(char *dst, size_t len, const char *src)
{
	if (len == 0) return;
	snprintf(dst, len, "%s", src ? src : "");
}

static int parse_int_range(const char *value, int lo, int hi, int *out)
{
	if (!value || !*value) return -1;
	char *end = NULL;
	long v = strtol(value, &end, 10);
	if (end == value) return -1;
	while (end && *end && isspace((unsigned char)*end)) end++;
	if (end && *end) return -1;
	if (v < lo || v > hi) return -1;
	*out = (int)v;
	return 0;
}

static void auto_profile_defaults(auto_profile_t *p)
{
	memset(p, 0, sizeof(*p));
}

static void auto_config_defaults(auto_config_t *c)
{
	memset(c, 0, sizeof(*c));
	auto_profile_defaults(&c->last);
	auto_profile_defaults(&c->ac);
	auto_profile_defaults(&c->bat);
}

static void auto_profile_key(auto_profile_t *p, const char *key, const char *val)
{
	int v;
	if (strcmp(key, "name") == 0) {
		auto_copy_str(p->name, sizeof(p->name), val);
	} else if (strcmp(key, "fan_mode") == 0) {
		if (strcmp(val, "manual") == 0 || strcmp(val, "curve") == 0 ||
		    strcmp(val, "auto") == 0)
			auto_copy_str(p->fan_mode, sizeof(p->fan_mode), val);
	} else if (strcmp(key, "fan") == 0) {
		if (parse_int_range(val, 0, 100, &v) == 0) p->fan = v;
	} else if (strcmp(key, "curve") == 0) {
		curve_pt_t tmp[AUTO_CURVE_MAX];
		int n = 0;
		if (parse_curve(val, tmp, AUTO_CURVE_MAX, &n) == 0) {
			memcpy(p->curve, tmp, sizeof(tmp[0]) * (size_t)n);
			p->curve_len = n;
		}
	} else if (strcmp(key, "pl1_w") == 0) {
		if (parse_int_range(val, 0, 500, &v) == 0) p->pl1_w = v;
	} else if (strcmp(key, "pl2_w") == 0) {
		if (parse_int_range(val, 0, 500, &v) == 0) p->pl2_w = v;
	} else if (strcmp(key, "pl4_w") == 0) {
		if (parse_int_range(val, 0, 500, &v) == 0) p->pl4_w = v;
	} else if (strcmp(key, "brightness") == 0) {
		if (parse_int_range(val, 0, 100, &v) == 0) p->brightness = v;
	} else if (strcmp(key, "governor") == 0) {
		auto_copy_str(p->governor, sizeof(p->governor), val);
	} else if (strcmp(key, "epp") == 0) {
		auto_copy_str(p->epp, sizeof(p->epp), val);
	} else if (strcmp(key, "max_freq") == 0) {
		if (parse_int_range(val, 0, 100, &v) == 0) p->max_freq = v;
	} else if (strcmp(key, "cores") == 0) {
		if (parse_int_range(val, 0, 1024, &v) == 0) p->cores = v;
	}
}

static void auto_webcam_key(auto_config_t *c, const char *key, const char *val)
{
	int v;
	if (strcmp(key, "webcam_enabled") == 0) {
		if (parse_int_range(val, 0, 1, &v) == 0) c->web_enabled = v;
	} else if (strcmp(key, "webcam_device") == 0) {
		if (strncmp(val, "/dev/", 5) == 0)
			auto_copy_str(c->web_device, sizeof(c->web_device), val);
	} else if (strcmp(key, "webcam_brightness") == 0) {
		if (parse_int_range(val, -10000, 10000, &v) == 0) {
			c->web_brightness = v; c->web_has_brightness = 1;
		}
	} else if (strcmp(key, "webcam_contrast") == 0) {
		if (parse_int_range(val, -10000, 10000, &v) == 0) {
			c->web_contrast = v; c->web_has_contrast = 1;
		}
	} else if (strcmp(key, "webcam_saturation") == 0) {
		if (parse_int_range(val, -10000, 10000, &v) == 0) {
			c->web_saturation = v; c->web_has_saturation = 1;
		}
	} else if (strcmp(key, "webcam_gamma") == 0) {
		if (parse_int_range(val, -10000, 10000, &v) == 0) {
			c->web_gamma = v; c->web_has_gamma = 1;
		}
	} else if (strcmp(key, "webcam_sharpness") == 0) {
		if (parse_int_range(val, -10000, 10000, &v) == 0) {
			c->web_sharpness = v; c->web_has_sharpness = 1;
		}
	} else if (strcmp(key, "webcam_wb_auto") == 0) {
		if (parse_int_range(val, 0, 1, &v) == 0) {
			c->web_wb_auto = v; c->web_has_wb_auto = 1;
		}
	} else if (strcmp(key, "webcam_wb_temp") == 0) {
		if (parse_int_range(val, 0, 20000, &v) == 0) {
			c->web_wb_temp = v; c->web_has_wb_temp = 1;
		}
	} else if (strcmp(key, "webcam_exposure_auto") == 0) {
		if (parse_int_range(val, 0, 3, &v) == 0) {
			c->web_exposure_auto = v; c->web_has_exposure_auto = 1;
		}
	} else if (strcmp(key, "webcam_exposure_time") == 0) {
		if (parse_int_range(val, 0, 1000000, &v) == 0) {
			c->web_exposure_time = v; c->web_has_exposure_time = 1;
		}
	}
}

static int parse_auto_config(const char *path, auto_config_t *c)
{
	auto_config_defaults(c);
	FILE *f = fopen(path, "r");
	if (!f) return -1;

	char line[8192];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (!line[0] || line[0] == '#') continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		trim(key);
		trim(val);

		if (strcmp(key, "enabled") == 0) {
			int v;
			if (parse_int_range(val, 0, 1, &v) == 0) c->enabled = v;
		} else if (strcmp(key, "manual") == 0) {
			int v;
			if (parse_int_range(val, 0, 1, &v) == 0) c->manual = v;
		} else if (strncmp(key, "last_", 5) == 0) {
			auto_profile_key(&c->last, key + 5, val);
		} else if (strncmp(key, "ac_", 3) == 0) {
			auto_profile_key(&c->ac, key + 3, val);
		} else if (strncmp(key, "bat_", 4) == 0) {
			auto_profile_key(&c->bat, key + 4, val);
		} else if (strncmp(key, "webcam_", 7) == 0) {
			auto_webcam_key(c, key, val);
		}
	}
	fclose(f);
	return 0;
}

static int auto_profile_present(const auto_profile_t *p)
{
	return p->name[0] || p->fan_mode[0] || p->pl1_w || p->pl2_w || p->pl4_w ||
	       p->brightness || p->governor[0] || p->epp[0] ||
	       p->max_freq || p->cores;
}

static auto_profile_t *auto_pick_profile(auto_config_t *c, int ac, int manual)
{
	if (!c->enabled) return NULL;
	if (manual && auto_profile_present(&c->last)) return &c->last;
	auto_profile_t *src = ac ? &c->ac : &c->bat;
	if (auto_profile_present(src)) return src;
	if (auto_profile_present(&c->last)) return &c->last;
	return NULL;
}

static int ac_online_c(void)
{
	static const char *paths[] = {
		"/sys/class/power_supply/AC0/online",
		"/sys/class/power_supply/AC/online",
		"/sys/class/power_supply/ACAD/online",
		"/sys/class/power_supply/ADP1/online",
	};
	char buf[32];
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (read_sys(paths[i], buf, sizeof(buf)) == 0)
			return atoi(buf) == 1 ? 1 : 0;
	}
	return -1;
}

static int auto_curve_tick(const auto_profile_t *p, int *inited, int *last_target)
{
	float cpu = temp_sys("temp1_input");
	float gpu = temp_sys("temp2_input");
	if (cpu < 0 && gpu < 0) {
		uint8_t ce = 0, ge = 0;
		if (ec_read_raw(EC_CPU_TEMP, &ce) == 0) cpu = (float)ce;
		if (ec_read_raw(EC_GPU_TEMP, &ge) == 0) gpu = (float)ge;
	}
	if (cpu < 0 && gpu < 0) return -1;

	float hottest = cpu > gpu ? cpu : gpu;
	int target = curve_pct(p->curve, p->curve_len, (int)hottest);
	if (hottest >= FAN_SAFETY_TEMP && target < FAN_SAFETY_MIN_PCT)
		target = FAN_SAFETY_MIN_PCT;
	if (target == *last_target) return 0;

	uint8_t speed = fan_speed_from_pct(target);
	int ok = 1;
	if (!*inited) {
		if (fan_backup_if_needed() != 0) return -1;
		ok = fan_universal_init(speed, speed) == 0;
		*inited = ok;
	}
	if (ok)
		ok = fan_apply_direct(0, speed) == 0 &&
		     fan_apply_direct(1, speed) == 0;
	if (!ok) {
		fprintf(stderr, "autoprofile: fan egrisi yazilamadi (acpi_call/izin?)\n");
		return -1;
	}
	*last_target = target;
	printf("autoprofile: CPU %.1fC GPU %.1fC -> fan %d%%\n", cpu, gpu, target);
	fflush(stdout);
	return 0;
}

static int auto_apply_profile(const auto_profile_t *p, int *curve_inited,
			      int *curve_last, int *fan_touched)
{
	if (!p) return 0;
	printf("autoprofile: profil '%s' uygulaniyor\n",
	       p->name[0] ? p->name : "son ayarlar");
	fflush(stdout);

	if (strcmp(p->fan_mode, "curve") == 0 && p->curve_len > 0) {
		*curve_inited = 0;
		*curve_last = -1;
		if (auto_curve_tick(p, curve_inited, curve_last) == 0)
			*fan_touched = 1;
	} else if (strcmp(p->fan_mode, "auto") == 0) {
		if (fan_restore_backup() == 0) *fan_touched = 0;
	} else if (strcmp(p->fan_mode, "manual") == 0) {
		if (fan_set_pct_direct(p->fan) == 0) *fan_touched = 1;
		else fprintf(stderr, "autoprofile: fan %d%% yazilamadi\n", p->fan);
	}

	if (p->pl1_w > 0) rapl_set_w("long_term", p->pl1_w);
	if (p->pl2_w > 0) rapl_set_w("short_term", p->pl2_w);
	if (p->pl4_w > 0) rapl_set_w("peak_power", p->pl4_w);

	if (p->brightness > 0) {
		char b[16];
		char *av[] = { (char *)"display", (char *)"brightness", b, NULL };
		snprintf(b, sizeof(b), "%d", p->brightness);
		cmd_display(3, av);
	}
	if (p->governor[0])
		cpu_apply_choice("scaling_governor", "scaling_available_governors",
				 p->governor, "governor");
	if (p->epp[0])
		cpu_apply_choice("energy_performance_preference",
				 "energy_performance_available_preferences",
				 p->epp, "EPP");
	if (p->max_freq >= 20) cpu_set_max_pct(p->max_freq);
	if (p->cores >= 1) cpu_set_cores(p->cores);
	return 0;
}

static int v4l2_run(const char *dev, const char *setarg)
{
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		execlp("v4l2-ctl", "v4l2-ctl", "-d", dev, setarg, (char *)NULL);
		_exit(127);
	}
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int apply_webcam(const auto_config_t *c)
{
	if (!c->web_enabled || strncmp(c->web_device, "/dev/", 5) != 0) return 0;
	if (access(c->web_device, R_OK) != 0) {
		fprintf(stderr, "autoprofile: webcam yok (%s)\n", c->web_device);
		return -1;
	}

	char arg[128];
	int tried = 0, ok = 0;
#define CAM_SET(has, field, key) do { \
	if (c->has) { \
		snprintf(arg, sizeof(arg), "--set-ctrl=%s=%d", key, c->field); \
		tried++; \
		if (v4l2_run(c->web_device, arg) == 0) ok++; \
		else fprintf(stderr, "autoprofile: webcam %s yazilamadi\n", key); \
	} \
} while (0)

	CAM_SET(web_has_brightness, web_brightness, "brightness");
	CAM_SET(web_has_contrast, web_contrast, "contrast");
	CAM_SET(web_has_saturation, web_saturation, "saturation");
	CAM_SET(web_has_gamma, web_gamma, "gamma");
	CAM_SET(web_has_sharpness, web_sharpness, "sharpness");
	CAM_SET(web_has_wb_auto, web_wb_auto, "white_balance_automatic");
	if (c->web_has_wb_temp && (!c->web_has_wb_auto || !c->web_wb_auto))
		CAM_SET(web_has_wb_temp, web_wb_temp, "white_balance_temperature");
	CAM_SET(web_has_exposure_auto, web_exposure_auto, "auto_exposure");
	if (c->web_has_exposure_time &&
	    (!c->web_has_exposure_auto || c->web_exposure_auto == 1))
		CAM_SET(web_has_exposure_time, web_exposure_time, "exposure_time_absolute");
#undef CAM_SET

	if (tried)
		printf("autoprofile: webcam %s -> %d/%d kontrol uygulandi\n",
		       c->web_device, ok, tried);
	if (tried && ok == 0)
		fprintf(stderr, "autoprofile: v4l2-ctl bulunamadi veya kamera izni yok\n");
	fflush(stdout);
	return ok == tried ? 0 : -1;
}

static void auto_print_profile(const char *label, const auto_profile_t *p)
{
	printf("  %s: name='%s' mode=%s fan=%d curve=%d pl1=%d pl2=%d pl4=%d "
	       "brightness=%d governor='%s' epp='%s' max_freq=%d cores=%d\n",
	       label, p->name, p->fan_mode[0] ? p->fan_mode : "-", p->fan,
	       p->curve_len, p->pl1_w, p->pl2_w, p->pl4_w, p->brightness,
	       p->governor, p->epp, p->max_freq, p->cores);
}

static void auto_print_config(const auto_config_t *c)
{
	printf("autoprofile: enabled=%d manual=%d\n", c->enabled, c->manual);
	auto_print_profile("last", &c->last);
	auto_print_profile("ac", &c->ac);
	auto_print_profile("bat", &c->bat);
	printf("  webcam: enabled=%d device='%s' brightness=%s%d contrast=%s%d "
	       "saturation=%s%d gamma=%s%d sharpness=%s%d wb_auto=%s%d wb_temp=%s%d "
	       "exposure_auto=%s%d exposure_time=%s%d\n",
	       c->web_enabled, c->web_device,
	       c->web_has_brightness ? "" : "-", c->web_brightness,
	       c->web_has_contrast ? "" : "-", c->web_contrast,
	       c->web_has_saturation ? "" : "-", c->web_saturation,
	       c->web_has_gamma ? "" : "-", c->web_gamma,
	       c->web_has_sharpness ? "" : "-", c->web_sharpness,
	       c->web_has_wb_auto ? "" : "-", c->web_wb_auto,
	       c->web_has_wb_temp ? "" : "-", c->web_wb_temp,
	       c->web_has_exposure_auto ? "" : "-", c->web_exposure_auto,
	       c->web_has_exposure_time ? "" : "-", c->web_exposure_time);
}

static volatile sig_atomic_t auto_stop = 0;
static volatile sig_atomic_t auto_reload = 0;

static void auto_signal(int sig)
{
	if (sig == SIGHUP) auto_reload = 1;
	else auto_stop = 1;
}

static int cmd_autoprofile(int argc, char **argv)
{
	int interval = 3;
	int check = 0;
	int once = 0;
	const char *path = AUTO_CONFIG_PATH;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
			interval = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
			path = argv[++i];
		} else if (strcmp(argv[i], "--check") == 0) {
			check = 1;
		} else if (strcmp(argv[i], "--once") == 0) {
			once = 1;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("kullanim: monsterctl autoprofile [--interval <sn>] "
			       "[--config <dosya>] [--check] [--once]\n");
			return 0;
		} else {
			fprintf(stderr, "bilinmeyen autoprofile secenegi: %s\n", argv[i]);
			return 2;
		}
	}
	if (interval < 1) interval = 1;

	auto_config_t cfg;
	int loaded = parse_auto_config(path, &cfg) == 0;
	if (check) {
		if (!loaded) {
			fprintf(stderr, "autoprofile: ayar dosyasi okunamadi: %s\n", path);
			return 1;
		}
		auto_print_config(&cfg);
		return 0;
	}

	struct stat st;
	time_t cfg_mtime = 0;
	off_t cfg_size = 0;
	int have_stat = stat(path, &st) == 0;
	if (have_stat) {
		cfg_mtime = st.st_mtime;
		cfg_size = st.st_size;
	}

	int source = ac_online_c();
	if (source < 0) source = 0;
	int curve_inited = 0, curve_last = -1, fan_touched = 0;
	auto_profile_t *current = auto_pick_profile(&cfg, source == 1, cfg.manual);

	if (once) {
		if (!current) {
			fprintf(stderr, "autoprofile: uygulanacak profil yok (%s)\n", path);
			return 1;
		}
		apply_webcam(&cfg);
		auto_apply_profile(current, &curve_inited, &curve_last, &fan_touched);
		return 0;
	}

	struct sigaction sa = { 0 };
	sa.sa_handler = auto_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	printf("autoprofile: servis aktif (interval=%ds, kaynak=%s, dosya=%s)\n",
	       interval, source == 1 ? "AC" : "pil", path);
	fflush(stdout);
	if (loaded) {
		apply_webcam(&cfg);
		if (current) auto_apply_profile(current, &curve_inited, &curve_last,
						&fan_touched);
	}

	unsigned int ac_tick = 0, curve_tick = 0;
	while (!auto_stop) {
		sleep(1);
		if (auto_stop) break;

		// ayar dosyasi degisti mi? (GUI root yardimcisi atomik yazar)
		have_stat = stat(path, &st) == 0;
		int changed = auto_reload;
		if (have_stat && (st.st_mtime != cfg_mtime || st.st_size != cfg_size))
			changed = 1;
		auto_reload = 0;
		if (changed) {
			auto_config_t next;
			int ok = parse_auto_config(path, &next) == 0;
			if (have_stat) {
				cfg_mtime = st.st_mtime;
				cfg_size = st.st_size;
			} else {
				cfg_mtime = 0;
				cfg_size = 0;
			}
			if (ok) {
				cfg = next;
				printf("autoprofile: ayarlar yeniden yuklendi (enabled=%d manual=%d)\n",
				       cfg.enabled, cfg.manual);
				fflush(stdout);
				apply_webcam(&cfg);
				current = auto_pick_profile(&cfg, source == 1, cfg.manual);
				if (current) {
					curve_inited = 0;
					curve_last = -1;
					auto_apply_profile(current, &curve_inited, &curve_last,
							   &fan_touched);
				} else if (fan_touched) {
					fan_restore_backup();
					fan_touched = 0;
				}
			} else {
				auto_config_defaults(&cfg);
				current = NULL;
				if (fan_touched) {
					fan_restore_backup();
					fan_touched = 0;
				}
				printf("autoprofile: ayar dosyasi yok/gecersiz; profil uygulanmiyor\n");
				fflush(stdout);
			}
		}

		ac_tick++;
		if (ac_tick % 2 == 0) {
			int now = ac_online_c();
			if (now >= 0 && now != source) {
				source = now;
				printf("autoprofile: kaynak %s -> profil seciliyor\n",
				       source == 1 ? "AC" : "pil");
				fflush(stdout);
				current = auto_pick_profile(&cfg, source == 1, 0);
				curve_inited = 0;
				curve_last = -1;
				if (current) {
					auto_apply_profile(current, &curve_inited, &curve_last,
							   &fan_touched);
				} else if (fan_touched) {
					fan_restore_backup();
					fan_touched = 0;
				}
			}
		}

		curve_tick++;
		if (current && strcmp(current->fan_mode, "curve") == 0 &&
		    current->curve_len > 0 && curve_tick >= (unsigned int)interval) {
			curve_tick = 0;
			if (auto_curve_tick(current, &curve_inited, &curve_last) == 0)
				fan_touched = 1;
		}
	}

	if (fan_touched) {
		printf("autoprofile: cikiliyor; EC fan tablolari geri yukleniyor...\n");
		fflush(stdout);
		fan_restore_backup();
	}
	return 0;
}

// ---- GUI'nin root yardimci uzerinden ayar yazmasi ----
static int b64_value(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static unsigned char *b64_decode(const char *s, size_t *out_len)
{
	size_t cap = strlen(s) * 3 / 4 + 8;
	unsigned char *out = malloc(cap ? cap : 1);
	if (!out) return NULL;
	size_t o = 0;
	int quad[4], q = 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (isspace(c)) continue;
		if (c == '=') break;
		int v = b64_value((char)c);
		if (v < 0) { free(out); return NULL; }
		quad[q++] = v;
		if (q == 4) {
			out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
			out[o++] = (unsigned char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
			out[o++] = (unsigned char)(((quad[2] & 3) << 6) | quad[3]);
			q = 0;
		}
	}
	if (q == 2) {
		out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
	} else if (q == 3) {
		out[o++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
		out[o++] = (unsigned char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
	} else if (q == 1) {
		free(out);
		return NULL;
	}
	*out_len = o;
	return out;
}

static unsigned char *read_all_file(const char *path, size_t *out_len, size_t max)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	long n = ftell(f);
	if (n < 0 || (size_t)n > max || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	unsigned char *buf = malloc((size_t)n + 1);
	if (!buf) { fclose(f); return NULL; }
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	*out_len = got;
	return buf;
}

static int write_auto_config(const unsigned char *data, size_t len)
{
	if (len == 0 || len > 262144) return -1;
	if (mkdir(AUTO_CONFIG_DIR, 0755) != 0 && errno != EEXIST) {
		perror("autoprofile: " AUTO_CONFIG_DIR);
		return -1;
	}
	char tmp[256];
	snprintf(tmp, sizeof(tmp), AUTO_CONFIG_PATH ".tmp.%ld", (long)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		perror("autoprofile: gecici dosya");
		return -1;
	}
	if (fwrite(data, 1, len, f) != len || fflush(f) != 0) {
		fclose(f);
		unlink(tmp);
		return -1;
	}
	fclose(f);
	chmod(tmp, 0644);
	if (rename(tmp, AUTO_CONFIG_PATH) != 0) {
		perror("autoprofile: rename");
		unlink(tmp);
		return -1;
	}
	chmod(AUTO_CONFIG_PATH, 0644);
	return 0;
}

static int cmd_sysconfig(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "kullanim: monsterctl sysconfig <base64|dosya>\n");
		return 2;
	}
	unsigned char *data = NULL;
	size_t len = 0;
	if (argv[1][0] == '/') {
		data = read_all_file(argv[1], &len, 262144);
	} else {
		data = b64_decode(argv[1], &len);
	}
	if (!data) {
		fprintf(stderr, "autoprofile: ayar verisi cozulemedi.\n");
		return 2;
	}
	int rc = write_auto_config(data, len);
	free(data);
	if (rc != 0) return 1;
	printf("Acilis ayarlari guncellendi: %s\n", AUTO_CONFIG_PATH);
	return 0;
}

static long long mem_available_mb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f) return -1;
	char key[64], unit[16];
	long long val, avail = -1, freeb = -1;
	while (fscanf(f, "%63s %lld %15s", key, &val, unit) == 3) {
		if (strcmp(key, "MemAvailable:") == 0) avail = val;
		else if (strcmp(key, "MemFree:") == 0) freeb = val;
	}
	fclose(f);
	if (avail < 0) avail = freeb;
	return avail < 0 ? -1 : avail / 1024;
}

static int cmd_cleanup(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "kullanim: monsterctl cleanup ram|journal [MB]|pkgcache|syscache\n");
		return 2;
	}
	if (strcmp(argv[1], "ram") == 0) {
		long long before = mem_available_mb();
		sync();
		FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
		if (!f) { fprintf(stderr, "drop_caches yazilamadi (root gerekli)\n"); return 1; }
		fputs("3\n", f);
		fclose(f);
		usleep(300000);
		long long after = mem_available_mb();
		if (before >= 0 && after >= 0)
			printf("RAM onbellegi temizlendi: kullanilabilir %lld MB -> %lld MB (+%lld MB)\n",
			       before, after, after - before);
		else
			printf("RAM onbellegi temizlendi.\n");
		return 0;
	}
	if (strcmp(argv[1], "journal") == 0) {
		int mb = argc > 2 ? atoi(argv[2]) : 20;
		if (mb < 10) mb = 10;
		printf("Journal donduruluyor ve %d MB'a dusuruluyor...\n", mb);
		int rc1 = system("journalctl --rotate 2>&1");
		char cmd[160];
		snprintf(cmd, sizeof(cmd), "journalctl --vacuum-size=%dM 2>&1", mb);
		int rc2 = system(cmd);
		(void)rc1;
		return (rc2 == 0 || rc2 == -1) ? 0 : 1;
	}
	if (strcmp(argv[1], "pkgcache") == 0) {
		// -rk0: son surum dahil TUM onbellek paketlerini sil (yeniden indirilebilir)
		printf("Paket onbellegi tamamen temizleniyor (paccache -rk0)...\n");
		int rc = system("paccache -rk0 -v 2>&1");
		return (rc == 0 || rc == -1) ? 0 : 1;
	}
	if (strcmp(argv[1], "syscache") == 0) {
		// debtap / pkgfile / fwupd on bellekleri; hepsi gerektiginde yeniden olusur
		printf("Sistem on bellekleri temizleniyor (debtap, pkgfile, fwupd)...\n");
		int rc = system("find /var/cache/debtap /var/cache/pkgfile /var/cache/fwupd "
		                "-mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>&1");
		return (rc == 0 || rc == -1) ? 0 : 1;
	}
	fprintf(stderr, "bilinmeyen cleanup alt komutu: %s\n", argv[1]);
	return 2;
}

// ---------- root yardimci (GUI icin, M5) ----------
// pkexec ile BIR KEZ acilir; sonraki komutlar sifresiz calisir.
// Protokol: her satir bir subkomut (orn "fan 60 --fan both", "kbd color 0 80 255").
// Cikti: cmd_* cikti metinleri + her komut islemi sonunda "RC <kod>" satiri.
// Ayrica "Uniwill WMI hotkeys" cihazindan gelen ozel tuslar (fan/profil tusu,
// Tuxedo'da Meta+Alt+F6 -> KEY_F6) "EVENT fankey" satiri olarak bildirilir.

static int open_hotkeys(void)
{
	DIR *d = opendir("/dev/input");
	if (!d) return -1;
	struct dirent *de;
	char path[512];
	int fd = -1;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, "event", 5) != 0) continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		int f = open(path, O_RDONLY | O_NONBLOCK);
		if (f < 0) continue;
		char name[256] = "";
		if (ioctl(f, EVIOCGNAME(sizeof(name)), name) >= 0 &&
		    strstr(name, "Uniwill")) {
			fd = f;
			break;
		}
		close(f);
	}
	closedir(d);
	return fd;
}

// Fiziksel fan tusu EC'de "full fan mode" bitini (0x0751 bit6) cevirebilir;
// OS'a tus olayi gelmese bile bunu yakala.
static void check_ec_fankey(uint8_t *last)
{
	uint8_t v = 0;
	if (ec_read_raw(EC_MANUAL_FAN_CTRL, &v) != 0) return;
	if ((v & 0x40) && !(*last & 0x40)) {
		printf("EVENT fankey\n");
		fflush(stdout);
		ec_write_raw(EC_MANUAL_FAN_CTRL, (uint8_t)(v & ~0x40));
		v &= (uint8_t)~0x40;
	}
	*last = v;
}

static void self_reexec(void)
{
	char path[512] = {0};
	ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (n <= 0) return;
	path[n] = '\0';
	char *suffix = strstr(path, " (deleted)");
	if (suffix) *suffix = '\0';
	char *const args[] = { path, "helper", NULL };
	execv(path, args);
}

static int helper_handle(char *line){
	char *tok[16];
	int n = 0;
	for (char *t = strtok(line, " \t\r\n"); t && n < 16; t = strtok(NULL, " \t\r\n"))
		tok[n++] = t;
	if (n == 0) return 0;
	if (strcmp(tok[0], "quit") == 0) { puts("RC 0"); fflush(stdout); return -1; }
	if (strcmp(tok[0], "reload") == 0) {
		// binary guncellendiginde sifre sormadan kendini yeniden baslat
		puts("RC 0"); fflush(stdout);
		self_reexec();
		_exit(0);
	}

	int rc = -1;
	if      (strcmp(tok[0], "fan") == 0)     rc = cmd_fan(n, tok);
	else if (strcmp(tok[0], "battery") == 0) rc = cmd_battery(n, tok);
	else if (strcmp(tok[0], "kbd") == 0)     rc = cmd_kbd(n, tok);
	else if (strcmp(tok[0], "power") == 0)   rc = cmd_power(n, tok);
	else if (strcmp(tok[0], "display") == 0) rc = cmd_display(n, tok);
	else if (strcmp(tok[0], "cpu") == 0)     rc = cmd_cpu(n, tok);
	else if (strcmp(tok[0], "sysconfig") == 0) rc = cmd_sysconfig(n, tok);
	else if (strcmp(tok[0], "cleanup") == 0) rc = cmd_cleanup(n, tok);
	else fprintf(stderr, "bilinmeyen komut: %s\n", tok[0]);
	printf("RC %d\n", rc);
	fflush(stdout);
	return 0;
}

static int cmd_helper(void)
{
	puts("helper ready");
	fflush(stdout);

	int hk = open_hotkeys();
	if (hk >= 0) {
		printf("hotkeys: Uniwill WMI tuslari dinleniyor\n");
		fflush(stdout);
	} else {
		fprintf(stderr, "hotkeys: Uniwill WMI cihazi bulunamadi\n");
	}
	struct pollfd fds[2];
	fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN; fds[0].revents = 0;
	fds[1].fd = hk;           fds[1].events = POLLIN; fds[1].revents = 0;

	char buf[65536];
	size_t blen = 0;
	uint8_t ec_last = 0;
	(void)ec_read_raw(EC_MANUAL_FAN_CTRL, &ec_last);
	while (1) {
		int pr = poll(fds, hk >= 0 ? 2 : 1, 1000);
		if (pr < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (pr == 0) { // zaman asimi: EC tuş durumunu kontrol et
			check_ec_fankey(&ec_last);
			continue;
		}
		if (hk >= 0 && (fds[1].revents & POLLIN)) {
			struct input_event ev[8];
			ssize_t r = read(hk, ev, sizeof(ev));
			for (ssize_t i = 0; i < r / (ssize_t)sizeof(ev[0]); i++) {
				if (ev[i].type != EV_KEY || ev[i].value != 1) continue;
				int code = ev[i].code;
				if (code == KEY_LEFTMETA || code == KEY_LEFTALT ||
				    code == KEY_LEFTSHIFT || code == KEY_LEFTCTRL ||
				    code == KEY_RIGHTMETA || code == KEY_RIGHTALT)
					continue;
				if (code == KEY_F6) {
					printf("EVENT fankey\n");
				} else {
					printf("EVENT key:%d\n", code);
				}
				fflush(stdout);
			}
		}
		if (fds[0].revents & (POLLIN | POLLHUP)) {
			if (blen >= sizeof(buf) - 1) blen = 0;
			ssize_t r = read(STDIN_FILENO, buf + blen, sizeof(buf) - blen - 1);
			if (r <= 0) break;
			blen += (size_t)r;
			buf[blen] = '\0';
			char *start = buf, *nl;
			while ((nl = strchr(start, '\n')) != NULL) {
				*nl = '\0';
				if (helper_handle(start) != 0) return 0;
				start = nl + 1;
			}
			blen = strlen(start);
			memmove(buf, start, blen + 1);
		}
	}
	if (hk >= 0) close(hk);
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"monster-ctl %s\n"
		"  sudo monsterctl status\n"
		"  sudo monsterctl fan <0-100> [--fan main|secondary|both]\n"
		"  sudo monsterctl fan auto | dump\n"
		"  sudo monsterctl fanprofile [--interval <sn>] [--curve <T:P,...>]\n"
		"  sudo monsterctl monitor <sn>\n"
		"  sudo monsterctl battery <10-100|off|status>\n"
		"  sudo monsterctl kbd <0-100|on|off|status>\n"
		"  sudo monsterctl kbd color <R> <G> <B>\n"
		"  sudo monsterctl power status\n"
		"  sudo monsterctl power stat        (anlik paket gucu)\n"
		"  sudo monsterctl power pl1 <W> | pl2 <W> | <pl1> <pl2>\n"
		"  sudo monsterctl power pl4 <W>\n"
		"  sudo monsterctl display [status|brightness <1-100>]\n"
		"  sudo monsterctl cpu status\n"
		"  sudo monsterctl cpu governor <performance|powersave>\n"
		"  sudo monsterctl cpu epp <default|performance|balance_performance|balance_power|power>\n"
		"  sudo monsterctl cpu max <20-100> | cores <1-N>\n"
		"  sudo monsterctl autoprofile [--interval <sn>] [--check] [--once]\n"
		"  sudo monsterctl sysconfig <base64|dosya>   (GUI tarafindan kullanilir)\n"
		"  sudo monsterctl cleanup ram | journal [MB] | pkgcache | syscache\n",
		"v1.1.0");
}

int main(int argc, char **argv)
{
	if (argc < 2) { usage(); return 2; }

	// acpi_call yolunu kontrol et
	if (strcmp(argv[1], "fan") == 0 || strcmp(argv[1], "battery") == 0 ||
	    strcmp(argv[1], "fanprofile") == 0) {
		FILE *f = fopen(ACPI_CALL_PATH, "r");
		if (!f) {
			fprintf(stderr, "HATA: %s bulunamadi.\n"
					"  Cozum: sudo modprobe acpi_call\n", ACPI_CALL_PATH);
			return 1;
		}
		fclose(f);
	}

	if (strcmp(argv[1], "status") == 0)  return cmd_status();
	if (strcmp(argv[1], "fan") == 0)     return cmd_fan(argc - 1, argv + 1);
	if (strcmp(argv[1], "fanprofile") == 0) return cmd_fanprofile(argc - 1, argv + 1);
	if (strcmp(argv[1], "battery") == 0) return cmd_battery(argc - 1, argv + 1);
	if (strcmp(argv[1], "monitor") == 0) return cmd_monitor(argc - 1, argv + 1);
	if (strcmp(argv[1], "kbd") == 0)     return cmd_kbd(argc - 1, argv + 1);
	if (strcmp(argv[1], "power") == 0)   return cmd_power(argc - 1, argv + 1);
	if (strcmp(argv[1], "display") == 0) return cmd_display(argc - 1, argv + 1);
	if (strcmp(argv[1], "cpu") == 0)     return cmd_cpu(argc - 1, argv + 1);
	if (strcmp(argv[1], "autoprofile") == 0) return cmd_autoprofile(argc - 1, argv + 1);
	if (strcmp(argv[1], "sysconfig") == 0) return cmd_sysconfig(argc - 1, argv + 1);
	if (strcmp(argv[1], "cleanup") == 0) return cmd_cleanup(argc - 1, argv + 1);
	if (strcmp(argv[1], "helper") == 0)  return cmd_helper();
	usage();
	return 2;
}
