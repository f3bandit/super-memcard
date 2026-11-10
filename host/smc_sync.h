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
    /* Games folders to find each save's game in (/media/fat/games, /media/usb0/games ...), NULL-terminated; the
     * saves then go on the card under their game's ID (docs/09). NULL: under their file names, as before. */
    const char *const *games_dirs;
    /* Folder with libretro's game lists (<list>.dat, smc_gamedb.h; tools/gamedb/fetch.sh), for each game's official
     * name in game.ini. NULL: no names. */
    const char *gamedb_dir;
};

struct smc_sync_report {
    bool card;   /* a Super MemCard card answered */
    char card_id[17];
    int pushed, pulled, unchanged, conflicts, skipped, failed;
    int switched; /* systems moved to another virtual card (the card's buttons) */
    int deleted;  /* saves deleted on another SuperStation, deleted here too (kept aside) */
    bool first;   /* nothing had been synced with this card before (a new card or a reinstall): everything was */
};

/* The 15 console cores' save folders (cores/cores.tsv; names from each core's CONF_STR). */
extern const char *const smc_console_systems[];

/* The save folder (system) of a game core about to be loaded, from its .rbf path ("_Console/SNES_20250101.rbf"
 * -> "SNES"), for Core; NULL for any other core. Not PSX: the PSX core uses the PS1 card itself, so the card's
 * buttons stay the PS1 card's. */
const char *smc_core_system(const char *rbf);

/* The default save icon of a system as icon.bin bytes (smc_icons.c, made by tools/icons/make_icons.py), or NULL.
 * The sync puts it beside a game's saves on the card the first time the game goes there. */
const uint8_t *smc_system_icon(const char *sys, size_t *len);

/* When to sync (docs/05 §3): the card is only reachable from the menu core, before and after a game.
 *   smc_sync_game()     just before a game starts: that one game (the card's newer copy comes in before the core
 *                       loads)
 *   smc_sync_changed()  back in the menu: only local saves new or changed since they were last synced (a folder read
 *                       when nothing changed: nothing is sent to the card)
 *   smc_sync()          everything: for the memory manager, or when the user asks
 * Each does everything instead the first time with a card (a new card, or a reinstall: every save comes back).
 * 0 on success (including "no Super MemCard card": report.card is false), <0 if the sync stopped. */
int smc_sync(const struct smc_bus *b, const struct smc_sync_cfg *cfg, struct smc_sync_report *rep);
/* game: the save's name, which is the game file's name without its extension (Main's rule) */
int smc_sync_game(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game,
                  struct smc_sync_report *rep);
int smc_sync_changed(const struct smc_bus *b, const struct smc_sync_cfg *cfg, struct smc_sync_report *rep);

/* The frontend deletes a save (`save`: its file name in saves/<sys>/, "Super Metroid (USA).sav"): the card's copy
 * is deleted with a tombstone, so every SuperStation that syncs deletes its copy too (unless that one was played
 * since: then it comes back). Nothing is lost: the card keeps the file in <game>/.deleted/, and each
 * SuperStation moves its local file to <state>/<card ID>/deleted/. SMC_OK or <0. */
int smc_delete_save(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *save);

/* Saves whose two copies both changed (the sync keeps both and reports them): calls cb for each, returns how many.
 * card_id: report.card_id. */
int smc_conflicts(const struct smc_sync_cfg *cfg, const char *card_id,
                  void (*cb)(const char *sys, int card, const char *save, uint32_t gen, void *ctx), void *ctx);
/* Settle one: keep the SuperStation's copy (it goes to the card) or the card's (it replaces the local file). The
 * other copy stays in <state>/<card ID>/conflicts/. SMC_OK or <0. */
enum { SMC_KEEP_LOCAL = 1, SMC_KEEP_CARD = 2 };
int smc_resolve(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *save, int keep);

/* The card's game ID for a save name: the name itself, or for names over 64 characters the first
 * 55, '~' and 8 hex digits of its CRC-32 (the full name is then kept on the card in name.txt). */
void smc_game_id(const char *game, char id[65]);
uint32_t smc_crc32(const uint8_t *p, size_t n, uint32_t crc);

/* For a frontend's memory manager (Console Mode): what the card has for one game of a system (`id` from
 * smc_list_card(), `card` 0: the system's current virtual card). */
struct smc_game_info {
    char id[65];         /* game ID: the game's own code (NSME, SLUS-00594, GM 00001009-00) or CRC-xxxxxxxx; the
                          * card folder may add the ROM's CRC (NSME-1A2B3C4D) */
    char name[128];      /* the official name ("Super Mario 64 (USA)"), from the game lists ("" if not known) */
    char title[65];      /* the name in the game's header ("" if it has none) */
    char save_name[256]; /* the save's file name it came from on a SuperStation (Main's name for the game) */
    uint32_t time;       /* when its newest save was made (Unix time, UTC; 0: not known) */
    int files;           /* save files */
    uint32_t bytes;
    bool has_icon;       /* icon.bin (05: PS1 save-icon layout) */
};
int smc_game_info(const struct smc_bus *b, const char *sys, int card, const char *id, struct smc_game_info *gi);
