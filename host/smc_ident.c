/* Super MemCard: which game a file is. Copied from Phosphor (phosphord ident.c: port, don't rewrite; change it there
 * and copy), without libchdr: a .chd is known by the SHA-1 its header carries.
 *
 * Cartridges: CRC32 of the ROM data, the way the No-Intro sets count it (copier headers left out: iNES/FDS 16 bytes,
 * SNES 512, Atari 7800 128, Lynx 64; N64 read in big-endian .z64 order whatever the file's byte order).
 * Discs (.cue/.bin, .iso): the product code (PlayStation SYSTEM.CNF, Saturn and Mega-CD headers) for looking the game
 * up; identity is product code + data track size + CRC of its first megabyte.
 * Copyright (C) 2026 f3bandit. */
#include "smc_ident.h"   /* (to be written: struct smc_ident, smc_ident_file) */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_HASH_SIZE (1024L * 1024 * 1024)

int smc_disc_system(const char *sys) {
	return !strcmp(sys, "PSX") || !strcmp(sys, "Saturn") || !strcmp(sys, "MegaCD");
}

/* ---------- CRC32 ---------- */

static uint32_t crc_table[256];

static void crc_init(void)
{
	if (crc_table[1]) return;
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}

static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
	crc = ~crc;
	while (n--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

typedef enum { N64_NONE, N64_Z64, N64_V64, N64_N64 } n64_order_t;

static int crc_file(const char *sys, const char *path, char out[9])
{
	out[0] = 0;
	crc_init();
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || size > MAX_HASH_SIZE) { fclose(f); out[0] = 0; return size <= 0 ? -1 : 0; }

	unsigned char head[128];
	size_t hn = fread(head, 1, sizeof(head), f);
	long skip = 0;
	n64_order_t order = N64_NONE;
	if (hn >= 16 && (!memcmp(head, "NES\x1a", 4) || !memcmp(head, "FDS\x1a", 4))) skip = 16;
	else if (!strcmp(sys, "SNES") && size % 1024 == 512) skip = 512;
	else if (hn >= 10 && !memcmp(head + 1, "ATARI7800", 9)) skip = 128;
	else if (hn >= 4 && !memcmp(head, "LYNX", 4)) skip = 64;
	else if (!strcmp(sys, "N64") && hn >= 4) {
		uint32_t m = (uint32_t)head[0] << 24 | head[1] << 16 | head[2] << 8 | head[3];
		order = m == 0x80371240u ? N64_Z64 : m == 0x37804012u ? N64_V64 : m == 0x40123780u ? N64_N64 : N64_NONE;
	}
	fseek(f, skip, SEEK_SET);

	static unsigned char buf[256 * 1024];
	uint32_t crc = 0;
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (order == N64_V64) {
			for (size_t i = 0; i + 1 < n; i += 2) { unsigned char t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t; }
		} else if (order == N64_N64) {
			for (size_t i = 0; i + 3 < n; i += 4) {
				unsigned char a = buf[i], b = buf[i + 1];
				buf[i] = buf[i + 3]; buf[i + 1] = buf[i + 2]; buf[i + 2] = b; buf[i + 3] = a;
			}
		}
		crc = crc_update(crc, buf, n);
	}
	int err = ferror(f);
	fclose(f);
	if (err) return -1;
	snprintf(out, 9, "%08x", crc);
	return 0;
}

/* CRC32 of the first `max` bytes of a file, and its size */
static int crc_head(const char *path, long max, char out[9], long *size)
{
	crc_init();
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	static unsigned char buf[64 * 1024];
	uint32_t crc = 0;
	long left = max;
	size_t n;
	while (left > 0 && (n = fread(buf, 1, (size_t)(left < (long)sizeof(buf) ? left : (long)sizeof(buf)), f)) > 0) {
		crc = crc_update(crc, buf, n);
		left -= (long)n;
	}
	int err = ferror(f);
	fclose(f);
	if (err) return -1;
	snprintf(out, 9, "%08x", crc);
	return 0;
}

/* ---------- discs ---------- */

typedef struct {
	FILE *f;
	int sector;   /* bytes per sector in the file: 2352 (raw) or 2048 */
	int offset;   /* where the 2048 data bytes start in a sector */
} disc_t;

static const unsigned char SYNC[12] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };

static int disc_read(disc_t *d, long lba, unsigned char out[2048])
{
	if (lba < 0 || lba > 0x7fffffffL / 2448) return -1;   /* a damaged image's sector number */
	if (fseek(d->f, lba * d->sector + d->offset, SEEK_SET)) return -1;
	return fread(out, 1, 2048, d->f) == 2048 ? 0 : -1;
}

static void disc_close(disc_t *d)
{
	if (d->f) { fclose(d->f); d->f = NULL; }
}

/* the first data track of a .cue (or a .bin/.iso itself) */
static int disc_open(const char *path, disc_t *d, char *track, size_t tn)
{
	snprintf(track, tn, "%s", path);
	const char *dot = strrchr(path, '.');
	if (dot && !strcasecmp(dot, ".cue")) {
		FILE *c = fopen(path, "r");
		if (!c) return -1;
		char line[1200], name[1000] = "";
		while (fgets(line, sizeof(line), c)) {
			char *p = line;
			while (isspace((unsigned char)*p)) p++;
			if (!strncasecmp(p, "FILE", 4) && !name[0]) {
				char *q = strchr(p, '"'), *e = q ? strchr(q + 1, '"') : NULL;
				if (q && e) { *e = 0; snprintf(name, sizeof(name), "%s", q + 1); }
				else if (sscanf(p + 4, " %999s", name) != 1) name[0] = 0;
			}
		}
		fclose(c);
		if (!name[0]) return -1;
		const char *slash = strrchr(path, '/');
		if (name[0] == '/' || !slash) snprintf(track, tn, "%s", name);
		else snprintf(track, tn, "%.*s/%s", (int)(slash - path), path, name);
	} else if (dot && !strcasecmp(dot, ".chd")) {
		return -1;   /* phosphord reads it with libchdr; the library only takes its SHA-1 (smc_ident_file) */
	}
	memset(d, 0, sizeof(*d));
	d->f = fopen(track, "rb");
	if (!d->f) return -1;
	unsigned char h[16];
	if (fread(h, 1, 16, d->f) != 16) { fclose(d->f); return -1; }
	if (!memcmp(h, SYNC, 12)) {
		d->sector = 2352;
		d->offset = h[15] == 2 ? 24 : 16;   /* mode 2 form 1 has an 8-byte subheader */
	} else {
		d->sector = 2048;
		d->offset = 0;
	}
	return 0;
}

static void trim_copy(char *dst, int n, const unsigned char *src, int len)
{
	while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == 0)) len--;
	int s = 0;
	while (s < len && src[s] == ' ') s++;
	int j = 0;
	for (int i = s; i < len && j + 1 < n; i++) dst[j++] = isprint(src[i]) ? (char)src[i] : '?';
	dst[j] = 0;
}

/* SLUS_005.94 -> SLUS-00594 */
static void psx_serial(const char *raw, char *out, int n)
{
	char t[32];
	int j = 0;
	for (const char *p = raw; *p && j < 31; p++)
		if (isalnum((unsigned char)*p)) t[j++] = (char)toupper((unsigned char)*p);
	t[j] = 0;
	int a = 0;
	while (isalpha((unsigned char)t[a])) a++;
	/* some discs boot a file with an extra letter (Dragon Warrior VII: SLUSP012.06): the code is
	   the first four letters and the digits */
	if (a == 5 && isdigit((unsigned char)t[a])) { memmove(t + 4, t + 5, strlen(t + 5) + 1); a = 4; }
	if (a >= 3 && a <= 4 && t[a]) snprintf(out, (size_t)n, "%.*s-%s", a, t, t + a);
	else snprintf(out, (size_t)n, "%s", t);
}

static int psx_boot(disc_t *d, char *serial, int n)
{
	unsigned char s[2048];
	if (disc_read(d, 16, s) || s[0] != 1 || memcmp(s + 1, "CD001", 5)) return -1;
	const unsigned char *root = s + 156;
	long lba = root[2] | root[3] << 8 | root[4] << 16 | (long)root[5] << 24;
	long len = root[10] | root[11] << 8 | root[12] << 16 | (long)root[13] << 24;
	for (long sec = 0; sec * 2048 < len && sec < 16; sec++) {
		unsigned char dir[2048];
		if (disc_read(d, lba + sec, dir)) return -1;
		for (int i = 0; i < 2048;) {
			int rl = dir[i];
			if (!rl || i + rl > 2048) break;
			if (rl < 34) break;                      /* too short to be a directory record */
			int nl = dir[i + 32];
			if (i + 33 + nl > 2048) break;
			if (nl >= 10 && !strncasecmp((const char *)dir + i + 33, "SYSTEM.CNF", 10)) {
				long flba = dir[i + 2] | dir[i + 3] << 8 | dir[i + 4] << 16 | (long)dir[i + 5] << 24;
				unsigned char cnf[2049];
				if (disc_read(d, flba, cnf)) return -1;
				cnf[2048] = 0;
				char *b = strstr((char *)cnf, "BOOT");
				if (!b) return -1;
				char *st = strchr(b, '=');
				if (!st) return -1;
				char *name = st + 1, *e;
				for (char *p = name; *p && *p != '\n' && *p != '\r' && *p != ';'; p++)
					if (*p == '\\' || *p == ':' || *p == '/') name = p + 1;
				for (e = name; *e && *e != ';' && *e != '\r' && *e != '\n' && *e != ' '; e++) ;
				*e = 0;
				if (!*name) return -1;
				psx_serial(name, serial, n);
				return 0;
			}
			i += rl;
		}
	}
	return -1;
}

static int disc_serial(const char *sys, const char *path, char *serial, int n, char *track, size_t tn)
{
	disc_t d;
	int rc = disc_open(path, &d, track, tn);
	if (rc) return -3;
	unsigned char s[2048];
	rc = -1;
	if (!strcmp(sys, "PSX")) {
		rc = psx_boot(&d, serial, n);
	} else if (!disc_read(&d, 0, s)) {
		if (!memcmp(s, "SEGA SEGASATURN", 15)) { trim_copy(serial, n, s + 0x20, 10); rc = serial[0] ? 0 : -1; }
		else if (!memcmp(s, "SEGADISCSYSTEM", 14)) { trim_copy(serial, n, s + 0x180, 14); rc = serial[0] ? 0 : -1; }
	}
	disc_close(&d);
	return rc;
}

/* a .chd keeps the SHA-1 of the original data in its header: identity without decompressing */
static int chd_sha1(const char *path, char *out, int n)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	unsigned char h[124];
	size_t got = fread(h, 1, sizeof(h), f);
	fclose(f);
	out[0] = 0;
	if (got < 108 || memcmp(h, "MComprHD", 8)) return 0;
	uint32_t ver = (uint32_t)h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15];
	int off = ver == 5 ? 64 : ver == 4 ? 88 : -1;    /* raw data SHA-1 (same whatever the compression) */
	if (off < 0 || got < (size_t)off + 20) return 0;
	int zero = 1;
	for (int i = 0; i < 20; i++) if (h[off + i]) zero = 0;
	if (zero) return 0;
	for (int i = 0; i < 20 && 2 * i + 2 < n; i++) snprintf(out + 2 * i, 3, "%02x", h[off + i]);
	return 0;
}

/* ---------- a file ---------- */

int smc_ident_file(const char *sys, const char *path, struct smc_ident *out)
{
	memset(out, 0, sizeof(*out));
	const char *dot = strrchr(path, '.');
	if (smc_disc_system(sys) && dot && !strcasecmp(dot, ".chd")) {
		/* identity: the header's SHA-1 (phosphord also reads the product code inside, with libchdr) */
		char sha[48];
		if (chd_sha1(path, sha, sizeof(sha))) return -1;
		if (sha[0]) snprintf(out->key, sizeof(out->key), "chd:%s", sha);
		return 0;
	}
	if (smc_disc_system(sys) && !(dot && !strcasecmp(dot, ".exe"))) {
		/* Fast: product code + data track size + a CRC of its first megabyte, all read in well under
		 * a second (a whole-disc CRC takes ~40 s per disc over USB). Size and the sample keep a mod or
		 * patched disc, which usually keeps the original's product code, apart from the original. */
		char track[1024];
		int rc = disc_serial(sys, path, out->serial, sizeof(out->serial), track, sizeof(track));
		if (rc == -3) return -1;             /* can't open the disc image right now */
		if (rc) out->serial[0] = 0;          /* no product code on the disc (e.g. PC Engine CD) */
		long size;
		char sample[9];
		if (crc_head(track, 1024L * 1024, sample, &size)) return -1;
		snprintf(out->key, sizeof(out->key), "disc:%.20s:%ld:%s", out->serial[0] ? out->serial : "-", size, sample);
		return 0;
	}
	if (crc_file(sys, path, out->crc32)) return -1;
	if (out->crc32[0]) snprintf(out->key, sizeof(out->key), "crc:%s", out->crc32);
	return 0;
}

