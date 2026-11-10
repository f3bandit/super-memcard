/* The card's settings, set by a test before vcard_start() (the card reads them at boot). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct host_settings {
    int ps1_card, ps1_channel, ps1_boot_channel;
    bool ps1_autoboot, ps1_game_id, ps1_controllercombo;
    uint8_t ps1_maxcardidx;
    /* Super MemCard's identity (ps1_supermemcard_id.c): the chip's unique ID (all zero: one is made at the first
     * start, as each RP2040's flash chip has its own) and the identity's flash page */
    uint8_t smc_uid[8];
    uint8_t smc_ident[256];
};
/* src/settings.c settings_reset(): GameID and the controller combo on, autoboot off */
#define HOST_SETTINGS_DEFAULTS { .ps1_card = 1, .ps1_channel = 1, .ps1_boot_channel = 1, \
    .ps1_autoboot = false, .ps1_game_id = true, .ps1_controllercombo = true, .ps1_maxcardidx = 0, .smc_ident = {[0 ... 255] = 0xFF} }
/* the card's flash: owned by the test program, kept across power-offs */
struct host_settings *host_settings(void);
