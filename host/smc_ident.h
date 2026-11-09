/* Super MemCard: which game a save belongs to (docs/09-game-database.md).
 *
 * A save's game is found from its ROM or disc (Main's rule: same name in the system's games folder) and given a
 * game ID: the identifier the game itself carries where it has one, else the ROM's CRC32.
 *
 *   N64          the 4-character game code in the header             NSME
 *   GBA          the 4-character game code in the header             AMTE
 *   Genesis, MegaDrive, S32X   the product number in the header      GM 00001009-00
 *   SMS          the product code in the TMR SEGA header             SMS-7069
 *   PSX, Saturn, MegaCD        the disc's product code               SLUS-00594, T-12705H
 *   SNES, GAMEBOY  the 4-character game code of later carts          SNES-AXXE, GB-AXXE
 *   everything else (NES, TurboGrafx-16, Jaguar, older carts)        CRC-1A2B3C4D
 *
 * Copyright (C) 2026 f3bandit. */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct smc_ident {
    char crc32[9];   /* 8 hex digits, ROM data without copier headers (No-Intro's count); "" if not hashed */
    char serial[32]; /* disc product code, for lookups */
    char key[80];    /* phosphord's identity: crc:xxxxxxxx, disc:<code>:<size>:<crc of 1st MB>, chd:<sha1> */
};

/* phosphord's identification (smc_ident.c, copied from Phosphor). 0 if the file could be read. */
int smc_ident_file(const char *sys, const char *path, struct smc_ident *out);
int smc_disc_system(const char *sys);

/* The game ID of a ROM or disc file of system `sys` (Main's save folder name), as above; title: the name in its
 * header where there is one (may be ""). 0, or -1 if the file can't be read. */
int smc_game_id_of(const char *sys, const char *path, char id[65], char title[65]);
