/* Copyright (C) 2026 f3bandit. Super MemCard additions to sd2psXtd: GPL-3.0-or-later, as sd2psXtd.
 * This program is distributed WITHOUT ANY WARRANTY; see the GNU General Public License. */
/* Super MemCard for sd2psXtd's PS1 mode: commands 81 60..6A (docs/05-design.md §2).
 *
 * Other systems' saves live on the card's SD card under SuperMemCard/<system>/Card<N>/<game ID>/, never
 * inside a .mcd, so PS1 mode can't see or change them (rule 3). Each system has virtual cards 1..99, like
 * PS1 mode's Card1, Card2...: the running core names its system (Core), and while it's named the card's
 * buttons pick that system's card instead of the PS1 card. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SMC_VERSION     3   /* 2: Core; virtual cards in Select and List. 3: Core carries the screen (SMC_CORE_SCREEN) */

enum {
    SMC_HELLO  = 0x60, /* 'S' 'M' ver host caps                -> 'S' 'M' ver features id[8] free_kb[4] (16) */
    SMC_SELECT = 0x61, /* syslen sys idlen id card             -> status nfiles card (3); card 0: the system's current */
    SMC_READ   = 0x62, /* file blk_hi blk_lo                   -> data[128] chk end (130) */
    SMC_WRITE  = 0x63, /* file blk_hi blk_lo data[128] chk     -> end (1) */
    SMC_CREATE = 0x64, /* size[4] namelen name                 -> status file (2) */
    SMC_COMMIT = 0x65, /* file gen[4] time[4]                  -> end (1); time: when the save was made (Unix, UTC) */
    SMC_STATUS = 0x66, /* (none)                               -> pending_hi pending_lo (2) */
    SMC_LIST   = 0x67, /* syslen sys card idx_hi idx_lo        -> status id[64] (65); sys "." lists systems */
    SMC_INFO   = 0x68, /* file                                 -> status name[16] size[4] gen[4] time[4] (29) */
    SMC_CORE   = 0x69, /* syslen sys flags                     -> status card (2): the system's current card;
                        * flags bit 0: this system's core is running (system mode: screen, buttons); sys "."
                        * with bit 0: no system (back to the PS1 card). Bit 1: one more request byte, the
                        * system's card to use from now on (a save picked on the SuperStation). Bit 2 (v3): then
                        * gamelen game, the launched game's name, shown on the screen */
    SMC_DELETE = 0x6A, /* file gen[4] time[4]                  -> end (1): the save is deleted at generation gen; the
                        * card keeps a tombstone (Info answers 44 'D') so other SS1s delete their copy too, and
                        * the file itself in <game>/.deleted/ */
    /* the card's identity (ps1_supermemcard_id.c, docs/12-card-identity.md) */
    SMC_CERT    = 0x6B, /* (none)                             -> status chip_id[8] pub[32] cert[64] (105); status 4F:
                         * no identity (only the chip ID) */
    SMC_AUTH    = 0x6C, /* challenge[32]                      -> status (1): the card signs it after answering */
    SMC_AUTHSIG = 0x6D, /* (none)                             -> status sig[64] (65); status 50: not signed yet */
};

/* answer status / end bytes, as the PS1 commands use them */
enum { SMC_GOOD = 0x47, SMC_BAD_CHECKSUM = 0x4E, SMC_FAIL = 0xFF, SMC_NOT_FOUND = 0x4F, SMC_NO_SET = 0x53,
       SMC_DELETED = 0x44, SMC_PENDING = 0x50 };

/* host IDs in Hello */
enum { SMC_HOST_MAIN = 1, SMC_HOST_CONSOLE_MODE = 2, SMC_HOST_CORE = 3, SMC_HOST_TEST = 0x7F };

#define SMC_MAX_FILES   16
#define SMC_NAME_LEN    16    /* file names: save.eep, cpak1.cpk ... */
#define SMC_SYS_LEN     32    /* system: Main's save folder name */
#define SMC_ID_LEN      64    /* game ID: Main's save name (longer ones are shortened by the host) */
#define SMC_MAX_FILE    (1024 * 1024)
#define SMC_MAX_CARDS   99    /* virtual cards per system */
#define SMC_CORE_RUNNING 0x01
#define SMC_CORE_SET_CARD 0x02   /* Core: a byte follows, the system's card to use from now on (1..99) */
#define SMC_CORE_SCREEN   0x04   /* Core (version 3): then the screen's lines, each len text: header, game, saved,
                                  * status, size (SMC_SCREEN_LINES; len 0: empty) */
#define SMC_SCREEN_LINES  5
#define SMC_SCREEN_HEADER 0      /* the system's name ("Game Boy Color") */
#define SMC_SCREEN_GAME   1      /* the launched game (scrolls when long) */
#define SMC_SCREEN_SAVED  2      /* when its save was made ("11/11/26 11:17PM": the whole row) */
#define SMC_SCREEN_STATUS 3      /* "On SSMC", "New save waiting", "No save yet" */
#define SMC_SCREEN_SIZE   4      /* "8 KB, 1 file" */
#define SMC_SCREEN_LEN    48     /* the game line (it scrolls) */
#define SMC_SCREEN_COLS   16     /* the other lines: the screen's width */
#define SMC_MAX_REQ     (4 + 128 + 4 + SMC_SYS_LEN + SMC_ID_LEN)
#define SMC_MAX_ANS     130

/* core 1 (bus) */
bool ps1_supermemcard_active(void);
/* Bytes this command's request has in total, given the first `have` bytes (-1: malformed). */
int ps1_supermemcard_req_len(uint8_t cmd, const uint8_t *req, int have);
/* Hand a complete request to core 0 and wait for the answer; gives up if *abort becomes set
 * (SEL rose). Returns the answer's length, 0 if aborted. */
int ps1_supermemcard_exec(uint8_t cmd, const uint8_t *req, int len, uint8_t *ans, volatile int *abort);

/* core 1: a PS1 read or write came (a PS1 game has the card): system mode ends */
void ps1_supermemcard_ps1_used(void);

/* core 0: call from the main loop (src/ps1.c ps1_task) */
void ps1_supermemcard_task(void);

/* core 0: the card's identity (ps1_supermemcard_id.c) */
int ps1_supermemcard_id_req_len(uint8_t cmd);
int ps1_supermemcard_id_command(uint8_t cmd, const uint8_t *req, uint8_t *ans);
void ps1_supermemcard_id_task(void);
bool ps1_supermemcard_has_identity(void);
/* the hardware under it (ps1_supermemcard_id.c on the RP2040; sim/card for the virtual card): the chip's unique ID,
 * and the identity's place in flash */
void smc_hw_uid(uint8_t uid[8]);
void smc_hw_ident_read(uint8_t *buf, int len);
void smc_hw_ident_write(const uint8_t *buf, int len);

/* core 0, for the card's screen and buttons (src/gui.c) */
/* The system whose core is running ("" when none: the buttons pick the PS1 card as before). */
const char *ps1_supermemcard_mode_system(void);
/* That system's current virtual card (1..SMC_MAX_CARDS). */
int ps1_supermemcard_mode_card(void);
/* Changes whenever the system or its card changes (to redraw). */
uint32_t ps1_supermemcard_mode_seq(void);
/* a line of the system mode's screen (SMC_SCREEN_*), sent by the SuperStation with Core; "" : not sent */
const char *ps1_supermemcard_mode_line(int line);
/* A card button: next (+1) / previous (-1) virtual card of the running system. Returns false when no
 * system is running (the key then does what it always did). The choice is saved after 1.5 s without
 * presses, as PS1 mode waits before switching. */
bool ps1_supermemcard_card_key(int delta);

/* The card's SuperStation menu (src/gui.c). "Saves on card": off, the card ignores Hello and is only a PS1/PS2
 * card, exactly like stock firmware (SuperMemCard/settings.ini, saves=0). */
bool ps1_supermemcard_enabled(void);
void ps1_supermemcard_set_enabled(bool on);
/* Free space on the SD card in KB: counts it (seconds on a big card: only from the menu, never on the bus) and
 * keeps it for Hello's answer, which says FFFFFFFF until it's been counted. */
uint32_t ps1_supermemcard_count_free(void);
/* A Super MemCard command was carried out in the last 1.5 s (the screen shows "saving"). */
bool ps1_supermemcard_busy(void);
/* Browse: the systems with saves on the card (up to max), and a system's games on its current virtual card: their
 * folders (game IDs) and names (game.ini's official name, else the header's, else the folder). */
#define SMC_BROWSE_NAME 48
int ps1_supermemcard_browse_systems(char (*out)[SMC_ID_LEN + 1], int max);
int ps1_supermemcard_browse_games(const char *sys, int *card, char (*ids)[SMC_ID_LEN + 1],
                                  char (*names)[SMC_BROWSE_NAME], int max);
