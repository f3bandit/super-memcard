/* Super MemCard: the official names of games, from libretro-database's game lists (smc_gamedb.c,
 * docs/09-game-database.md). Copyright (C) 2026 f3bandit. */
#pragma once

/* The libretro lists of a system (Main's save folder name): up to 3 names, as in libretro-database's metadat/no-intro
 * (or metadat/redump when *disc), "<name>.dat". Returns how many (0: none for that system). */
int smc_gamedb_lists(const char *sys, const char *names[3], int *disc);

/* The official name of a game ("Super Mario 64 (USA)") by its ROM's CRC32 (8 hex digits) or, failing that, its
 * product code, from the lists in `dir` (<dir>/<list>.dat); NULL when not known. Loads one system's lists at a time
 * and keeps them until another system is asked for or smc_gamedb_close(). */
const char *smc_gamedb_name(const char *dir, const char *sys, const char *crc, const char *serial);
void smc_gamedb_close(void);
