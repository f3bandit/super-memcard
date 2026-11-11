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
};

/* The console cores' save folders (cores/cores.tsv; names from each core's CONF_STR), and Console Mode's GBC. */
extern const char *const smc_console_systems[];
/* A save folder's name for a screen ("N64" -> "Nintendo 64"; unknown: the folder's own). */
const char *smc_system_title(const char *sys);

/* The save folder (system) of a game core about to be loaded, from its .rbf path ("_Console/SNES_20250101.rbf"
 * -> "SNES"), for Core; NULL for any other core. Every console core, PSX too: its per-game card images in saves/PSX
 * are that game's saves. (If the PSX core uses the PS1 card itself, the card's first PS1 read or write ends system
 * mode: its buttons are the PS1 card's again.) */
const char *smc_core_system(const char *rbf);

/* The save folder of a launch (Main's CoreName2): an MGL's <setname> names it unless same_dir="1"
 * (Console Mode's GameboyColor.mgl starts the Gameboy core with saves/GBC). NULL: not a system the card keeps. */
const char *smc_launch_system(const char *rbf, const char *setname, bool same_dir);

/* An MGL file: the core it starts, its <setname>, and the game file's name without folder or extension (Main names
 * the save after it; "" when it starts no file). 0 if it could be read. */
struct smc_mgl {
    char rbf[256], setname[64], game[256];
    bool same_dir;
};
int smc_mgl_read(const char *path, struct smc_mgl *m);
/* A Saturn game's save name from its disc's name, as Main makes it: "<game> (Disc 1)" -> "<game>" (in place) */
void smc_saturn_save_name(char *name);
/* An MGL that starts a game (its save's name in sys) the way Console Mode's own MGLs do, made from its ROM in the games
 * folders: 0, or -1 when the ROM isn't found or the system's MGL isn't known (CD systems, arcade: an earlier launch's MGL
 * only) */
int smc_game_mgl(const struct smc_sync_cfg *cfg, const char *sys, const char *game, char *out, size_t n);

/* The default save icon of a system as icon.bin bytes (smc_icons.c, made by tools/icons/make_icons.py), or NULL.
 * The sync puts it beside a game's saves on the card the first time the game goes there. */
const uint8_t *smc_system_icon(const char *sys, size_t *len);
/* A PS1 card image's own save icon (its first save's) as icon.bin: length, 0 if none (smc_icons.c). */
size_t smc_ps1_icon(const uint8_t *img, size_t len, uint8_t *out, size_t out_max);
/* A 32x32 icon.bin made from a game's box art (PNG or JPEG; smc_art_icon.c): length, 0 if it can't be read. */
size_t smc_art_icon(const char *path, uint8_t *out, size_t out_max);
/* Save icons for games whose saves carry none (smc_sync.c, docs/05-design.md): the download list made by the syncs is
 * worked through with fetch() (url, file to write: 0 on success; smcd: curl), each game's picture made into its icon in
 * the icon database. Returns how many icons were made. Run it where waiting on the net is fine (not in a sync). */
int smc_fetch_icons(const struct smc_sync_cfg *cfg, int (*fetch)(const char *url, const char *to));
/* The icons made since: onto the card, over the system's placeholder only. Returns how many. */
int smc_apply_icons(const struct smc_bus *b, const struct smc_sync_cfg *cfg);
/* A manual card read: games with no picture found are asked for again (else once in 30 days). */
void smc_icons_retry(bool on);
/* While a game runs (on): a virtual card picked with the card's buttons meanwhile is used from the next game; the running
 * game's saves keep going to the card it started with (it has that save in memory). */
void smc_sync_hold_card(bool on);
/* The saves a game has on the card, one per virtual card that has it, for a menu to pick from: cb(card, how many save
 * files, the newest one's time, whether it's the card this SuperStation's save is from). Returns how many, <0 error. */
int smc_game_saves(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game,
                   void (*cb)(int card, int files, uint32_t time, bool playing, const char *label, void *ctx),
                   void *ctx);
/* The name the user gave a game's save on a card (the memory manager's Rename; Console Mode's on-screen keyboard), kept
 * on the card beside it (label.txt; it goes wherever the card goes). "" clears it. Up to SMC_LABEL_MAX characters the
 * OSD can show (printable ASCII). 0 named, -2 that card has no save of the game, -1 otherwise */
#define SMC_LABEL_MAX 24
int smc_set_label(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game, int card,
                  const char *label);
/* Use a game's save from virtual card `card` (the whole system moves to that card, as the buttons do): this
 * SuperStation's saves go to the card they came from first, then the chosen card's come in. 0 done, <0 not (nothing
 * changed then). The game must be started again to load it (it has the old save in memory). */
int smc_use_card(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game, int card,
                 struct smc_sync_report *rep);
/* The game lists (libretro's, "<name>" as in <gamedb_dir>/<name>.dat) the games waiting for an icon need and that
 * aren't there: for whoever downloads lists (cm_modules' gamedb module). Returns how many (names filled). */
int smc_icon_lists_needed(const struct smc_sync_cfg *cfg, char names[][96], int max);

/* When to sync (docs/05 §3): like a PS1, the card is only read or written when a save is loaded or saved, and the card
 * is only reachable from the menu core, so around a game:
 *   smc_sync_game()     just before a game starts ("load": the card's newer copy comes in before the core loads), and
 *                       back in the menu after it ("save": its save goes to the card if it changed). One game.
 *   smc_sync_changed()  back in the menu when the frontend doesn't know which game was played (started from the core's
 *                       own file browser): that system's saves changed since they were last synced. Reads the folder;
 *                       nothing is sent to the card when nothing changed
 *   smc_sync()          everything: the memory manager's "read card" (Console Mode), or a "restore everything"
 * Nothing runs by itself at menu start. A reinstall gets each game's save back the first time that game is started.
 * 0 on success (including "no Super MemCard card": report.card is false), <0 if the sync stopped. */
int smc_sync(const struct smc_bus *b, const struct smc_sync_cfg *cfg, struct smc_sync_report *rep);
/* game: the save's name, which is the game file's name without its extension (Main's rule) */
int smc_sync_game(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys, const char *game,
                  struct smc_sync_report *rep);
/* sys: one system (Main's save folder), or NULL for all */
int smc_sync_changed(const struct smc_bus *b, const struct smc_sync_cfg *cfg, const char *sys,
                     struct smc_sync_report *rep);

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
    char label[25];      /* the name the user gave this save ("" if none; smc_set_label) */
};
int smc_game_info(const struct smc_bus *b, const char *sys, int card, const char *id, struct smc_game_info *gi);
