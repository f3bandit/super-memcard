/* Super MemCard: a game's ID from its own header (smc_ident.h). The CRC32 and disc product codes come from
 * phosphord's identification (smc_ident.c); the cartridge header codes are read here.
 * Copyright (C) 2026 f3bandit. */
#include "smc_ident.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static long file_size(FILE *f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    return n;
}

static int read_at(FILE *f, long off, unsigned char *buf, size_t n) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

/* n printable characters, upper case letters and digits only (a game code) */
static int code_ok(const unsigned char *p, int n) {
    for (int i = 0; i < n; i++)
        if (!(isupper(p[i]) || isdigit(p[i])))
            return 0;
    return 1;
}

/* a header text field: trimmed, printable */
static void text(char *out, size_t on, const unsigned char *p, int n) {
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == 0))
        n--;
    int s = 0;
    while (s < n && p[s] == ' ')
        s++;
    size_t j = 0;
    for (int i = s; i < n && j + 1 < on; i++)
        out[j++] = isprint(p[i]) ? (char)p[i] : ' ';
    out[j] = 0;
}

/* only what a folder name on the card may hold (no / \ : * ? " < > |, no dots or spaces at either end) */
static void folder_safe(char *s) {
    for (char *p = s; *p; p++)
        if (strchr("/\\:*?\"<>|", *p) || (unsigned char)*p < 0x20)
            *p = '_';
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '.'))
        s[--n] = 0;
    size_t k = 0;
    while (s[k] == ' ' || s[k] == '.')
        k++;
    memmove(s, s + k, strlen(s + k) + 1);
}

static int n64(FILE *f, char *id, char *title) {
    unsigned char h[64];
    if (read_at(f, 0, h, 64))
        return -1;
    uint32_t m = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
    if (m == 0x37804012u) /* .v64: bytes swapped in pairs */
        for (int i = 0; i < 64; i += 2) { unsigned char t = h[i]; h[i] = h[i + 1]; h[i + 1] = t; }
    else if (m == 0x40123780u) /* .n64: little-endian words */
        for (int i = 0; i < 64; i += 4) {
            unsigned char a = h[i], b = h[i + 1];
            h[i] = h[i + 3], h[i + 1] = h[i + 2], h[i + 2] = b, h[i + 3] = a;
        }
    else if (m != 0x80371240u)
        return -1;
    text(title, 65, h + 0x20, 20);
    if (!code_ok(h + 0x3B, 4))
        return -1;
    snprintf(id, 65, "%.4s", (const char *)h + 0x3B);
    return 0;
}

static int gba(FILE *f, char *id, char *title) {
    unsigned char h[0xB0];
    if (read_at(f, 0, h, sizeof(h)))
        return -1;
    text(title, 65, h + 0xA0, 12);
    if (!code_ok(h + 0xAC, 4))
        return -1;
    snprintf(id, 65, "%.4s", (const char *)h + 0xAC);
    return 0;
}

static int sega16(FILE *f, char *id, char *title) {
    unsigned char h[0x190];
    if (read_at(f, 0, h, sizeof(h)) || (memcmp(h + 0x100, "SEGA", 4) && memcmp(h + 0x101, "SEGA", 4)))
        return -1; /* not a plain .bin/.md (e.g. an interleaved .smd): the CRC then */
    text(title, 65, h + 0x150, 48);
    text(id, 65, h + 0x180, 14);
    return id[0] && strcmp(id, "GM XXXXXXXX-XX") ? 0 : -1;
}

static int bcd(unsigned v) { return (int)((v >> 4) * 10 + (v & 15)); }

static int sms(FILE *f, char *id, char *title) {
    static const long at[] = {0x7FF0, 0x3FF0, 0x1FF0};
    unsigned char h[16];
    title[0] = 0;
    for (int i = 0; i < 3; i++)
        if (!read_at(f, at[i], h, 16) && !memcmp(h, "TMR SEGA", 8)) {
            int code = (h[14] >> 4) * 10000 + bcd(h[13]) * 100 + bcd(h[12]);
            if (!code)
                return -1;
            snprintf(id, 65, "SMS-%d", code);
            return 0;
        }
    return -1;
}

/* a SNES header at `base` (LoROM 0x7FC0, HiROM 0xFFC0, + 512 with a copier header) whose checksum and its
 * complement agree; later carts (old maker code 33) have a game code at base - 0x0E */
static int snes(FILE *f, char *id, char *title) {
    long size = file_size(f), skip = size % 1024 == 512 ? 512 : 0;
    static const long bases[] = {0x7FC0, 0xFFC0, 0x40FFC0};
    for (int i = 0; i < 3; i++) {
        unsigned char h[64];
        long b = bases[i] + skip;
        if (b + 0x30 > size || read_at(f, b - 0x10, h, 0x40))
            continue;
        const unsigned char *s = h + 0x10;
        unsigned ck = s[0x1E] | s[0x1F] << 8, cc = s[0x1C] | s[0x1D] << 8;
        if ((ck ^ cc) != 0xFFFF)
            continue;
        text(title, 65, s, 21);
        if (s[0x1A] == 0x33 && code_ok(h + 0x02, 4)) {
            snprintf(id, 65, "SNES-%.4s", (const char *)h + 0x02);
            return 0;
        }
        return -1;
    }
    return -1;
}

static int gameboy(FILE *f, char *id, char *title) {
    unsigned char h[0x50];
    if (read_at(f, 0x100, h, sizeof(h)))
        return -1;
    const unsigned char *t = h + 0x34;
    bool newer = h[0x4B] == 0x33 && (h[0x43] & 0x80); /* Color carts with a new maker code: title 11 + code 4 */
    text(title, 65, t, newer ? 11 : 16);
    if (newer && code_ok(h + 0x3F, 4)) {
        snprintf(id, 65, "GB-%.4s", (const char *)h + 0x3F);
        return 0;
    }
    return -1;
}

int smc_game_id_of(const char *sys, const char *path, char id[65], char title[65]) {
    id[0] = title[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    int rc = -1;
    if (!strcmp(sys, "N64"))
        rc = n64(f, id, title);
    else if (!strcmp(sys, "GBA"))
        rc = gba(f, id, title);
    else if (!strcmp(sys, "Genesis") || !strcmp(sys, "MegaDrive") || !strcmp(sys, "S32X"))
        rc = sega16(f, id, title);
    else if (!strcmp(sys, "SMS"))
        rc = sms(f, id, title);
    else if (!strcmp(sys, "SNES"))
        rc = snes(f, id, title);
    else if (!strcmp(sys, "GAMEBOY"))
        rc = gameboy(f, id, title);
    fclose(f);
    if (rc == 0) {
        folder_safe(id);
        if (id[0])
            return 0;
    }
    /* no code in the game itself: phosphord's identity (disc product code, or the ROM's CRC32) */
    struct smc_ident x;
    if (smc_ident_file(sys, path, &x))
        return -1;
    if (x.serial[0])
        snprintf(id, 65, "%s", x.serial);
    else if (x.crc32[0])
        snprintf(id, 65, "CRC-%s", x.crc32);
    else if (!strncmp(x.key, "disc:", 5) && strrchr(x.key, ':'))
        snprintf(id, 65, "DISC-%s", strrchr(x.key, ':') + 1);
    else if (!strncmp(x.key, "chd:", 4))
        snprintf(id, 65, "CHD-%.8s", x.key + 4);
    else
        return -1;
    for (char *p = id; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    folder_safe(id);
    return id[0] ? 0 : -1;
}
