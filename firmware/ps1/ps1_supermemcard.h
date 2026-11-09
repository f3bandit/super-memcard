/* Super MemCard for sd2psXtd's PS1 mode: commands 81 60..69 (docs/05-design.md §2).
 *
 * Other systems' saves live on the card's SD card under SuperMemCard/<system>/Card<N>/<game ID>/, never
 * inside a .mcd, so PS1 mode can't see or change them (rule 3). Each system has virtual cards 1..99, like
 * PS1 mode's Card1, Card2...: the running core names its system (Core), and while it's named the card's
 * buttons pick that system's card instead of the PS1 card. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SMC_VERSION     2   /* 2: Core; virtual cards in Select and List */

enum {
    SMC_HELLO  = 0x60, /* 'S' 'M' ver host caps                -> 'S' 'M' ver features id[8] free_kb[4] (16) */
    SMC_SELECT = 0x61, /* syslen sys idlen id card             -> status nfiles card (3); card 0: the system's current */
    SMC_READ   = 0x62, /* file blk_hi blk_lo                   -> data[128] chk end (130) */
    SMC_WRITE  = 0x63, /* file blk_hi blk_lo data[128] chk     -> end (1) */
    SMC_CREATE = 0x64, /* size[4] namelen name                 -> status file (2) */
    SMC_COMMIT = 0x65, /* file gen[4]                          -> end (1) */
    SMC_STATUS = 0x66, /* (none)                               -> pending_hi pending_lo (2) */
    SMC_LIST   = 0x67, /* syslen sys card idx_hi idx_lo        -> status id[64] (65); sys "." lists systems */
    SMC_INFO   = 0x68, /* file                                 -> status name[16] size[4] gen[4] (25) */
    SMC_CORE   = 0x69, /* syslen sys flags                     -> status card (2): the system's current card;
                        * flags bit 0: this system's core is running (system mode: screen, buttons); sys "."
                        * with bit 0: no system (back to the PS1 card) */
};

/* answer status / end bytes, as the PS1 commands use them */
enum { SMC_GOOD = 0x47, SMC_BAD_CHECKSUM = 0x4E, SMC_FAIL = 0xFF, SMC_NOT_FOUND = 0x4F, SMC_NO_SET = 0x53 };

/* host IDs in Hello */
enum { SMC_HOST_PHOSPHOR = 1, SMC_HOST_CONSOLE_MODE = 2, SMC_HOST_CORE = 3, SMC_HOST_TEST = 0x7F };

#define SMC_MAX_FILES   16
#define SMC_NAME_LEN    16    /* file names: save.eep, cpak1.cpk ... */
#define SMC_SYS_LEN     32    /* system: Main's save folder name */
#define SMC_ID_LEN      64    /* game ID: Main's save name (longer ones are shortened by the host) */
#define SMC_MAX_FILE    (1024 * 1024)
#define SMC_MAX_CARDS   99    /* virtual cards per system */
#define SMC_CORE_RUNNING 0x01
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

/* core 0, for the card's screen and buttons (src/gui.c) */
/* The system whose core is running ("" when none: the buttons pick the PS1 card as before). */
const char *ps1_supermemcard_mode_system(void);
/* That system's current virtual card (1..SMC_MAX_CARDS). */
int ps1_supermemcard_mode_card(void);
/* Changes whenever the system or its card changes (to redraw). */
uint32_t ps1_supermemcard_mode_seq(void);
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
