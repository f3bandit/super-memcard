/* Copyright (C) 2026 f3bandit. Super MemCard additions to sd2psXtd: GPL-3.0-or-later, as sd2psXtd.
 * This program is distributed WITHOUT ANY WARRANTY; see the GNU General Public License. */
/* Super MemCard: the card's identity (docs/12-card-identity.md), core 0.
 *
 * A genuine card has its own key pair, and Retro Remake's signature of its chip ID and public key (its
 * certificate). The SuperStation checks the certificate (CERT) and has the card sign a random challenge with its key
 * (AUTH, then AUTHSIG); without an identity the card is an sd2psXtd PS1 card and the SuperStation uses it only as one.
 *
 * The identity comes as smc_key.bin on the SD card (from Retro Remake's smccert): the card checks Retro Remake
 * signed it for this chip, keeps it in its own 4 KB of flash and deletes the file, as sd2psXtd's keystore.c does with
 * a PS2 card's civ.bin. A file that isn't signed for this chip is renamed smc_key.bad and the flash isn't touched. */
#include "ps1_supermemcard.h"
#include "ps1_supermemcard_rrkey.h"

#include <fcntl.h>
#include <string.h>

#include "monocypher.h"
#include "sd.h"

#define KEY_FILE "smc_key.bin"
#define BAD_FILE "smc_key.bad"

/* the identity in flash (one 256-byte page; erased flash is FF, so no magic: no identity) */
#define REC_MAGIC "SMI1"
#define REC_LEN   (4 + 64 + 64 + 4) /* magic, the card's secret key (seed and public key), certificate, check */

#ifndef SMC_SIM
#include "flashmap.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/unique_id.h"

/* 4 KB below the splash screen's (flashmap.h): nothing else of sd2psXtd's moves */
#define FLASH_OFF_SMC (FLASH_OFF_SPLASH - 0x1000)

void smc_hw_uid(uint8_t uid[8]) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id); /* the flash chip's unique ID, read at boot */
    memcpy(uid, id.id, 8);
}

void smc_hw_ident_read(uint8_t *buf, int len) { memcpy(buf, (const uint8_t *)XIP_BASE + FLASH_OFF_SMC, (size_t)len); }

void __not_in_flash_func(smc_hw_ident_write)(const uint8_t *buf, int len) {
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, buf, (size_t)len);
    /* as keystore.c: core 1 (the bus) held off and interrupts off while the flash is written */
    if (multicore_lockout_victim_is_initialized(1))
        multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_OFF_SMC, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_OFF_SMC, page, sizeof(page));
    restore_interrupts(ints);
    if (multicore_lockout_victim_is_initialized(1))
        multicore_lockout_end_blocking();
}
#endif

static uint8_t uid[8], sk[64], cert[64];
static bool have_id, loaded;

/* auth: the challenge being signed */
enum { AUTH_NONE, AUTH_PENDING, AUTH_READY };
static int auth_state;
static uint8_t nonce[32], auth_sig[64];

static void rec_check(uint8_t out[4], const uint8_t *rec) {
    uint8_t h[32];
    crypto_blake2b(h, sizeof(h), rec, REC_LEN - 4);
    memcpy(out, h, 4);
}

static void cert_msg(uint8_t m[48], const uint8_t pk[32]) {
    memcpy(m, "SMC-CERT", 8);
    memcpy(m + 8, uid, 8);
    memcpy(m + 16, pk, 32);
}

static void load_flash(void) {
    uint8_t rec[REC_LEN], chk[4];
    smc_hw_ident_read(rec, REC_LEN);
    rec_check(chk, rec);
    if (memcmp(rec, REC_MAGIC, 4) || memcmp(rec + REC_LEN - 4, chk, 4))
        return;
    memcpy(sk, rec + 4, 64);
    memcpy(cert, rec + 68, 64);
    /* the certificate must still be Retro Remake's for this chip (flash copied from another card: not) */
    uint8_t m[48];
    cert_msg(m, sk + 32);
    have_id = crypto_eddsa_check(cert, smc_rr_pub, m, sizeof(m)) == 0;
    if (!have_id)
        crypto_wipe(sk, sizeof(sk));
    crypto_wipe(rec, sizeof(rec));
}

/* smc_key.bin: the card's key seed (32), Retro Remake's signature (64) */
static void deploy(void) {
    if (!sd_exists(KEY_FILE))
        return;
    uint8_t f[96];
    int fd = sd_open(KEY_FILE, O_RDONLY), n = fd >= 0 ? sd_read(fd, f, sizeof(f)) : -1;
    if (fd >= 0)
        sd_close(fd);
    uint8_t nsk[64], pk[32], m[48];
    bool ok = n == (int)sizeof(f);
    if (ok) {
        crypto_eddsa_key_pair(nsk, pk, f); /* wipes the seed in f */
        cert_msg(m, pk);
        ok = crypto_eddsa_check(f + 32, smc_rr_pub, m, sizeof(m)) == 0;
    }
    if (!ok) {
        crypto_wipe(f, sizeof(f));
        crypto_wipe(nsk, sizeof(nsk));
        if (sd_exists(BAD_FILE))
            sd_remove(BAD_FILE);
        sd_rename(KEY_FILE, BAD_FILE);
        return;
    }
    uint8_t rec[REC_LEN], old[REC_LEN];
    memcpy(rec, REC_MAGIC, 4);
    memcpy(rec + 4, nsk, 64);
    memcpy(rec + 68, f + 32, 64);
    rec_check(rec + REC_LEN - 4, rec);
    smc_hw_ident_read(old, REC_LEN);
    if (memcmp(old, rec, REC_LEN))
        smc_hw_ident_write(rec, REC_LEN);
    smc_hw_ident_read(old, REC_LEN);
    bool written = memcmp(old, rec, REC_LEN) == 0;
    crypto_wipe(rec, sizeof(rec));
    crypto_wipe(old, sizeof(old));
    crypto_wipe(nsk, sizeof(nsk));
    crypto_wipe(f, sizeof(f));
    if (written)
        sd_remove(KEY_FILE); /* the key lives only in the card's flash from now on */
    have_id = false;
    load_flash();
}

static void load(void) {
    if (loaded)
        return;
    loaded = true;
    smc_hw_uid(uid);
    load_flash();
    deploy();
}

int ps1_supermemcard_id_req_len(uint8_t cmd) {
    switch (cmd) {
    case SMC_CERT: return 0;
    case SMC_AUTH: return 32;
    case SMC_AUTHSIG: return 0;
    }
    return -1;
}

int ps1_supermemcard_id_command(uint8_t cmd, const uint8_t *r, uint8_t *a) {
    load();
    switch (cmd) {
    case SMC_CERT:
        /* status, chip ID, the card's public key, certificate: without an identity only the chip ID (to issue one) */
        memset(a, 0, 105);
        a[0] = have_id ? SMC_GOOD : SMC_NOT_FOUND;
        memcpy(a + 1, uid, 8);
        if (have_id) {
            memcpy(a + 9, sk + 32, 32);
            memcpy(a + 41, cert, 64);
        }
        return 105;
    case SMC_AUTH:
        if (!have_id) {
            a[0] = SMC_NOT_FOUND;
            return 1;
        }
        memcpy(nonce, r, 32);
        auth_state = AUTH_PENDING; /* signed by ps1_supermemcard_id_task(), after this answer */
        a[0] = SMC_GOOD;
        return 1;
    case SMC_AUTHSIG:
        memset(a, 0, 65);
        a[0] = auth_state == AUTH_READY ? SMC_GOOD : auth_state == AUTH_PENDING ? SMC_PENDING : SMC_NOT_FOUND;
        if (auth_state == AUTH_READY) {
            memcpy(a + 1, auth_sig, 64);
            auth_state = AUTH_NONE; /* one answer per challenge */
        }
        return 65;
    }
    return 0;
}

void ps1_supermemcard_id_task(void) {
    if (auth_state != AUTH_PENDING)
        return;
    /* "SMC-AUTH" + challenge + chip ID, signed with the card's key (tens of ms on the RP2040: here, not on the bus) */
    uint8_t m[48];
    memcpy(m, "SMC-AUTH", 8);
    memcpy(m + 8, nonce, 32);
    memcpy(m + 40, uid, 8);
    crypto_eddsa_sign(auth_sig, sk, m, sizeof(m));
    auth_state = AUTH_READY;
}

bool ps1_supermemcard_has_identity(void) {
    load();
    return have_id;
}
