/* Super MemCard phase 1: keep the SS1's saves and the card's copies in step (docs/05-design.md §3,
 * §5). Run it from the menu (Main's start in the menu core): it compares every console save in
 * the saves folder with the card and copies whichever side changed.
 *
 *   local changed, card as last synced   -> to the card (generation + 1)
 *   card changed, local as last synced   -> from the card (another SS1, or a reinstall)
 *   only one side has it                 -> copied to the other
 *   both changed (and differ)            -> both kept: the local save stays in place, the card's
 *                                           copy is saved under <state>/conflicts/, nothing is
 *                                           overwritten; reported so the frontend can ask
 *
 * What was last synced is kept per card, in <state>/<card ID>/synced.txt (another card starts
 * from nothing: everything on either side is copied over, nothing is assumed).
 *
 * Each system has virtual cards on the card (1..99, picked with the card's buttons while that system's
 * core runs). The local saves of a system belong to one of them (<state>/<card ID>/cards.txt). When the
 * card says another one is now in use, the local saves go to the old card first, are moved aside to
 * <state>/<card ID>/cards/<sys>/Card<old>/, and the new card's saves come in; nothing is deleted. */
#pragma once
#include "smc.h"

struct smc_sync_cfg {
    const char *saves_dir;       /* /media/fat/saves */
    const char *state_dir;       /* /media/fat/supermemcard */
    const char *const *systems;  /* save folders to keep on the card, NULL-terminated */
    uint8_t host;                /* SMC_HOST_* for Hello */
    void (*log)(const char *line); /* may be NULL */
};

struct smc_sync_report {
    bool card;   /* a Super MemCard card answered */
    char card_id[17];
    int pushed, pulled, unchanged, conflicts, skipped, failed;
    int switched; /* systems moved to another virtual card (the card's buttons) */
};

/* The 15 console cores' save folders (cores/cores.tsv; names from each core's CONF_STR). */
extern const char *const smc_console_systems[];

/* The save folder (system) of a game core about to be loaded, from its .rbf path ("_Console/SNES_20250101.rbf"
 * -> "SNES"), for Core; NULL for any other core. Not PSX: the PSX core uses the PS1 card itself, so the card's
 * buttons stay the PS1 card's. */
const char *smc_core_system(const char *rbf);

/* 0 on success (including "no Super MemCard card": report.card is false), <0 if the sync stopped. */
int smc_sync(const struct smc_bus *b, const struct smc_sync_cfg *cfg, struct smc_sync_report *rep);

/* The card's game ID for a save name: the name itself, or for names over 64 characters the first
 * 55, '~' and 8 hex digits of its CRC-32 (the full name is then kept on the card in name.txt). */
void smc_game_id(const char *game, char id[65]);
uint32_t smc_crc32(const uint8_t *p, size_t n, uint32_t crc);
