/* Copyright (C) 2026 f3bandit. Super MemCard additions to sd2psXtd: GPL-3.0-or-later, as sd2psXtd.
 * This program is distributed WITHOUT ANY WARRANTY; see the GNU General Public License. */
/* Super MemCard: the card's own firmware updated through the front port (SMC_UPDATE, 81 6F), core 0.
 *
 *   Begin  the new firmware's size and CRC-32 (zlib's): SuperMemCard/update/firmware.new, empty
 *   Data   its 128-byte blocks, as Write's
 *   Check  the card reads it back in the background (Check again until it isn't PENDING): the size and CRC-32 must be
 *          Begin's, and it must be built for the resident updater (its vectors after the updater's 4 KB). Then it's
 *          SuperMemCard/update/firmware.bin
 *   Apply  the card answers, copies firmware.bin into the spare half of its flash (with the updater's header and its
 *          own CRC) and restarts. The updater (firmware/flashloader, ported from pico-flashloader) checks it and writes
 *          it over the firmware, and tries again on the next start if the power went mid-way
 *   Info   the updater is there or not, how the last update went, the running firmware's version
 *
 * The card never takes a firmware that failed a check, and the running one is only replaced by the updater, from a
 * whole, checked copy in flash. */
#include "ps1_supermemcard.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "sd.h"

#define UPD_DIR  "SuperMemCard/update"
#define UPD_NEW  UPD_DIR "/firmware.new"
#define UPD_BIN  UPD_DIR "/firmware.bin"

#ifndef SMC_BUILD
#define SMC_BUILD __DATE__ " " __TIME__
#endif

/* the resident updater's layout (firmware/flashloader/memmap_defines.ld): the updater in the first 4 KB, the
 * firmware after it, the new one staged from 2 MB */
#define LOADER_LEN   0x1000u
#define STAGE_OFF    0x200000u
#define APP_MAX      (STAGE_OFF - LOADER_LEN)
#define FLASH_MAGIC1 0x8ecd5efbu      /* pico-flashloader's flashloader.h */
#define FLASH_MAGIC2 0xc5ae52a9u
#define FLASH_APP_UPDATED 0xe3fa4ef2u

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* zlib's CRC-32 (the SuperStation's, as file_crc), and the updater's (CRC-32/MPEG-2: the RP2040 DMA sniffer's) */
static uint32_t crc_z(uint32_t crc, const uint8_t *p, int n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    for (int i = 0; i < n; i++)
        crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static uint32_t crc_m(uint32_t crc, const uint8_t *p, int n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; k++)
                c = c & 0x80000000u ? (c << 1) ^ 0x04C11DB7u : c << 1;
            t[i] = c;
        }
    for (int i = 0; i < n; i++)
        crc = (crc << 8) ^ t[((crc >> 24) ^ p[i]) & 0xFF];
    return crc;
}

/* ---- the hardware: the RP2040's flash and restart, or the virtual card's stand-ins ---- */

static int last_result;   /* SMC_UPD_RESULT_*: the last update, as this start found it */

#ifndef SMC_SIM
#include "flashmap.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"

extern char __flash_binary_start;   /* where this firmware was linked: after the updater, or at 0 (no updater) */

static bool hw_has_loader(void) { return (uint32_t)&__flash_binary_start == XIP_BASE + LOADER_LEN; }
static bool hw_room(uint32_t size) { return STAGE_OFF + FLASH_SECTOR_SIZE + size + 0x8000u <= flash_border; }

static void hw_boot_result(void) {
    if (watchdog_hw->scratch[0] == FLASH_APP_UPDATED) {
        watchdog_hw->scratch[0] = 0;
        last_result = SMC_UPD_RESULT_UPDATED;
    }
}

/* one 4 KB sector of the staged copy: core 1 (the bus) held off and interrupts off while the flash is written, as
 * keystore.c */
static void __not_in_flash_func(hw_write_sector)(uint32_t off, const uint8_t *buf) {
    if (multicore_lockout_victim_is_initialized(1))
        multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    if (multicore_lockout_victim_is_initialized(1))
        multicore_lockout_end_blocking();
}
static const uint8_t *hw_staged(void) { return (const uint8_t *)(XIP_BASE + STAGE_OFF); }

/* the updater takes it from here: it finds the staged copy through the watchdog's scratch registers */
static void hw_restart_into_loader(void) {
    watchdog_hw->scratch[0] = FLASH_MAGIC1;
    watchdog_hw->scratch[1] = XIP_BASE + STAGE_OFF;
    watchdog_reboot(0, 0, 100);
    for (;;)
        tight_loop_contents();
}
#else
/* the virtual card: always has the updater; the "flash" is a file, and a restart installs it at once */
#define FLASH_SECTOR_SIZE 4096u
static uint8_t sim_flash[FLASH_SECTOR_SIZE + APP_MAX];
static bool hw_has_loader(void) { return true; }
static bool hw_room(uint32_t size) { return size <= APP_MAX; }
static void hw_boot_result(void) {}
static void hw_write_sector(uint32_t off, const uint8_t *buf) { memcpy(sim_flash + (off - STAGE_OFF), buf, FLASH_SECTOR_SIZE); }
static const uint8_t *hw_staged(void) { return sim_flash; }
static void hw_restart_into_loader(void) {
    /* the updater's checks, then "installed": what the next start reports */
    uint32_t len = be32((const uint8_t[]){sim_flash[11], sim_flash[10], sim_flash[9], sim_flash[8]});
    uint32_t crc = be32((const uint8_t[]){sim_flash[15], sim_flash[14], sim_flash[13], sim_flash[12]});
    last_result = crc_m(0xFFFFFFFFu, sim_flash + 16, (int)len) == crc ? SMC_UPD_RESULT_UPDATED : SMC_UPD_RESULT_FAILED;
    int fd = sd_open(UPD_DIR "/installed.bin", O_RDWR | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        sd_write(fd, sim_flash + 16, len);
        sd_close(fd);
    }
}
#endif

/* ---- the update ---- */

enum { ST_NONE, ST_RECEIVING, ST_CHECKING, ST_CHECKED, ST_BAD, ST_APPLY };
static int state;
static uint32_t want_size, want_crc, chk_off, chk_crc, chk_mcrc, app_mcrc;
static int chk_fd = -1, rx_fd = -1;
static uint64_t apply_at;
static bool booted;

int ps1_supermemcard_update_req_len(const uint8_t *r, int have) {
    if (have < 1)
        return 1;
    switch (r[0]) {
    case SMC_UPD_BEGIN: return 1 + 8;
    case SMC_UPD_DATA: return 1 + 2 + 128 + 1;
    case SMC_UPD_CHECK:
    case SMC_UPD_APPLY:
    case SMC_UPD_INFO: return 1;
    default: return -1;
    }
}

/* Hello's feature: the updater, and room in the flash for a new firmware beside the running one (16 MB cards) */
bool ps1_supermemcard_update_available(void) { return hw_has_loader() && hw_room(APP_MAX); }

static void close_fds(void) {
    if (rx_fd >= 0) {
        sd_flush(rx_fd);
        sd_close(rx_fd);
    }
    if (chk_fd >= 0)
        sd_close(chk_fd);
    rx_fd = chk_fd = -1;
}

int ps1_supermemcard_update_command(const uint8_t *r, int len, uint8_t *a) {
    (void)len;
    switch (r[0]) {
    case SMC_UPD_BEGIN: {
        uint32_t size = be32(r + 1);
        a[0] = SMC_FAIL;
        a[1] = SMC_UPD_WHY_OK;
        if (!hw_has_loader()) {
            a[1] = SMC_UPD_WHY_NO_LOADER;   /* this card takes a new firmware over USB only (once: with the updater) */
            return 2;
        }
        if (size <= 0x200 || size > APP_MAX || !hw_room(size)) {
            a[1] = SMC_UPD_WHY_SIZE;
            return 2;
        }
        close_fds();
        sd_mkdir("SuperMemCard");
        sd_mkdir(UPD_DIR);
        if (sd_exists(UPD_NEW))
            sd_remove(UPD_NEW);
        /* an empty file: the blocks fill it as they come (as Create; nothing here may take time) */
        rx_fd = sd_open(UPD_NEW, O_RDWR | O_CREAT | O_TRUNC);
        if (rx_fd < 0) {
            a[1] = SMC_UPD_WHY_SD;
            return 2;
        }
        want_size = size;
        want_crc = be32(r + 5);
        state = ST_RECEIVING;
        a[0] = SMC_GOOD;
        return 2;
    }

    case SMC_UPD_DATA: {
        uint32_t off = ((uint32_t)r[1] << 8 | r[2]) * 128u;
        uint8_t chk = r[1] ^ r[2];
        for (int i = 0; i < 128; i++)
            chk ^= r[3 + i];
        a[0] = SMC_FAIL;
        if (chk != r[131]) {
            a[0] = SMC_BAD_CHECKSUM;
            return 1;
        }
        if (state != ST_RECEIVING || rx_fd < 0 || off >= want_size || sd_seek(rx_fd, (int32_t)off, SEEK_SET) != 0)
            return 1;
        uint32_t n = want_size - off < 128 ? want_size - off : 128;
        if (sd_write(rx_fd, (void *)(r + 3), n) != (int)n)
            return 1;
        a[0] = SMC_GOOD;
        return 1;
    }

    case SMC_UPD_CHECK:
        a[0] = SMC_FAIL;
        if (state == ST_RECEIVING) {   /* the reading back starts: ps1_supermemcard_update_task, a piece at a time */
            close_fds();
            chk_fd = sd_open(UPD_NEW, O_RDONLY);
            if (chk_fd < 0 || sd_filesize(chk_fd) != (int)want_size) {
                state = ST_BAD;
                close_fds();
                return 1;
            }
            chk_off = 0;
            chk_crc = chk_mcrc = 0xFFFFFFFFu;
            state = ST_CHECKING;
        }
        a[0] = state == ST_CHECKING ? SMC_PENDING : state == ST_CHECKED ? SMC_GOOD
             : state == ST_BAD ? SMC_BAD_CHECKSUM : SMC_FAIL;
        return 1;

    case SMC_UPD_APPLY:
        a[0] = SMC_FAIL;
        if (state != ST_CHECKED)
            return 1;
        state = ST_APPLY;
        apply_at = time_us_64() + 300 * 1000;   /* after this answer has gone back */
        a[0] = SMC_GOOD;
        return 1;

    case SMC_UPD_INFO: {
        a[0] = SMC_GOOD;
        a[1] = hw_has_loader() ? 1 : 0;
        a[2] = (uint8_t)last_result;
        memset(a + 3, 0, SMC_UPD_VERSION_LEN);
        snprintf((char *)a + 3, SMC_UPD_VERSION_LEN, "%s", SMC_BUILD);
        return 3 + SMC_UPD_VERSION_LEN;
    }
    }
    return 0;
}

/* a whole firmware for the updater: built to run after it (the reset vector past its 4 KB, inside the firmware's
 * space), its boot2 with a good check (the updater won't start one without) */
static bool built_for_loader(const uint8_t *head) {
    uint32_t sp = (uint32_t)head[0x100] | head[0x101] << 8 | head[0x102] << 16 | (uint32_t)head[0x103] << 24;
    uint32_t pc = (uint32_t)head[0x104] | head[0x105] << 8 | head[0x106] << 16 | (uint32_t)head[0x107] << 24;
    uint32_t b2 = (uint32_t)head[0xFC] | head[0xFD] << 8 | head[0xFE] << 16 | (uint32_t)head[0xFF] << 24;
    return (sp >> 28) == 2 && pc > 0x10000000u + LOADER_LEN && pc < 0x10000000u + STAGE_OFF &&
           crc_m(0xFFFFFFFFu, head, 252) == b2;
}

/* the new firmware into the spare half of the flash: the updater's header (magic, length, CRC), then the firmware */
static bool stage(void) {
    static uint8_t sec[4096];
    int fd = sd_open(UPD_BIN, O_RDONLY);
    if (fd < 0)
        return false;
    uint32_t hdr[4] = {FLASH_MAGIC1, FLASH_MAGIC2, want_size, app_mcrc};   /* (little-endian, as the RP2040 reads it) */
    memcpy(sec, hdr, sizeof(hdr));
    uint32_t have = sizeof(hdr), off = STAGE_OFF, left = want_size;
    bool ok = true;
    while (ok && (left || have)) {
        int n = left ? sd_read(fd, sec + have, sizeof(sec) - have < left ? sizeof(sec) - have : left) : 0;
        if (n < 0 || (left && n == 0)) {
            ok = false;
            break;
        }
        have += (uint32_t)n;
        left -= (uint32_t)n;
        if (have == sizeof(sec) || !left) {
            memset(sec + have, 0xFF, sizeof(sec) - have);
            hw_write_sector(off, sec);
            off += sizeof(sec);
            have = 0;
        }
    }
    sd_close(fd);
    /* the copy in flash as the updater will check it */
    const uint8_t *st = hw_staged();
    return ok && memcmp(st, hdr, sizeof(hdr)) == 0 && crc_m(0xFFFFFFFFu, st + 16, (int)want_size) == app_mcrc;
}

void ps1_supermemcard_update_task(void) {
    if (!booted) {
        booted = true;
        hw_boot_result();
    }
    if (state == ST_CHECKING) {
        static uint8_t buf[4096];
        for (int k = 0; k < 4 && state == ST_CHECKING; k++) {   /* 16 KB a pass: the bus's commands still answered */
            int n = sd_read(chk_fd, buf, sizeof(buf));
            if (n < 0 || (n == 0 && chk_off < want_size)) {
                state = ST_BAD;
                break;
            }
            if (chk_off == 0 && (n < 0x200 || !built_for_loader(buf))) {
                state = ST_BAD;   /* not a firmware for the updater (one linked at 0 would never start after it) */
                break;
            }
            chk_crc = crc_z(chk_crc, buf, n);
            chk_mcrc = crc_m(chk_mcrc, buf, n);
            chk_off += (uint32_t)n;
            if (chk_off >= want_size) {
                if ((chk_crc ^ 0xFFFFFFFFu) == want_crc && chk_off == want_size) {
                    close_fds();
                    if (sd_exists(UPD_BIN))
                        sd_remove(UPD_BIN);
                    state = sd_rename(UPD_NEW, UPD_BIN) == 0 ? ST_CHECKED : ST_BAD;
                    app_mcrc = chk_mcrc;
                } else
                    state = ST_BAD;
            }
        }
        if (state != ST_CHECKING)
            close_fds();
    }
    if (state == ST_APPLY && time_us_64() >= apply_at) {
        state = ST_NONE;
        if (stage())
            hw_restart_into_loader();   /* (doesn't come back on the card) */
        else
            last_result = SMC_UPD_RESULT_FAILED;
    }
}
