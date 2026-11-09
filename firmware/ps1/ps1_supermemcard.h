/* Super MemCard for sd2psXtd's PS1 mode: commands 81 60..68 (docs/05-design.md §2).
 *
 * Other systems' saves live on the card's SD card under SuperMemCard/<system>/<game ID>/, never
 * inside a .mcd, so PS1 mode can't see or change them (rule 3). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SMC_VERSION     1

enum {
    SMC_HELLO  = 0x60, /* 'S' 'M' ver host caps                -> 'S' 'M' ver features id[8] free_kb[4] (16) */
    SMC_SELECT = 0x61, /* syslen sys idlen id                  -> status nfiles (2) */
    SMC_READ   = 0x62, /* file blk_hi blk_lo                   -> data[128] chk end (130) */
    SMC_WRITE  = 0x63, /* file blk_hi blk_lo data[128] chk     -> end (1) */
    SMC_CREATE = 0x64, /* size[4] namelen name                 -> status file (2) */
    SMC_COMMIT = 0x65, /* file gen[4]                          -> end (1) */
    SMC_STATUS = 0x66, /* (none)                               -> pending_hi pending_lo (2) */
    SMC_LIST   = 0x67, /* syslen sys idx_hi idx_lo             -> status id[32] (33) */
    SMC_INFO   = 0x68, /* file                                 -> status name[16] size[4] gen[4] (25) */
};

/* answer status / end bytes, as the PS1 commands use them */
enum { SMC_GOOD = 0x47, SMC_BAD_CHECKSUM = 0x4E, SMC_FAIL = 0xFF, SMC_NOT_FOUND = 0x4F, SMC_NO_SET = 0x53 };

/* host IDs in Hello */
enum { SMC_HOST_PHOSPHOR = 1, SMC_HOST_CONSOLE_MODE = 2, SMC_HOST_CORE = 3, SMC_HOST_TEST = 0x7F };

#define SMC_MAX_FILES   16
#define SMC_NAME_LEN    16    /* file names: save.eep, cpak1.cpk ... */
#define SMC_ID_LEN      32    /* system and game IDs */
#define SMC_MAX_FILE    (1024 * 1024)
#define SMC_MAX_REQ     (4 + 128 + 4 + SMC_ID_LEN * 2)
#define SMC_MAX_ANS     130

/* core 1 (bus) */
bool ps1_supermemcard_active(void);
/* Bytes this command's request has in total, given the first `have` bytes (-1: malformed). */
int ps1_supermemcard_req_len(uint8_t cmd, const uint8_t *req, int have);
/* Hand a complete request to core 0 and wait for the answer; gives up if *abort becomes set
 * (SEL rose). Returns the answer's length, 0 if aborted. */
int ps1_supermemcard_exec(uint8_t cmd, const uint8_t *req, int len, uint8_t *ans, volatile int *abort);

/* core 0: call from the main loop (src/ps1.c ps1_task) */
void ps1_supermemcard_task(void);
