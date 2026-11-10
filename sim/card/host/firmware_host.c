/* The rest of the card the PS1 code calls into: settings (flash on the card, memory here, with
 * the firmware's defaults from src/settings.c), buttons (none pressed), logging, the game DB. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/rand.h"

#include "settings.h"
#include "input.h"
#include "host_settings.h"

/* The card keeps its settings in flash, so they outlive a power-off. Here they're in memory
 * shared with the test program, which outlives the card process (host/vcard_client.c). */
static struct host_settings fallback = HOST_SETTINGS_DEFAULTS;
static struct host_settings *flash = &fallback;
void host_set_flash(struct host_settings *f) { flash = f; }
#define s (*flash)

int settings_get_ps1_card(void) {
    if (s.ps1_card < IDX_MIN)
        return IDX_MIN;
    if (s.ps1_maxcardidx > 0 && s.ps1_card > s.ps1_maxcardidx)
        return s.ps1_maxcardidx;
    return s.ps1_card;
}
int settings_get_ps1_channel(void) { return s.ps1_channel < CHAN_MIN ? CHAN_MIN : s.ps1_channel; }
int settings_get_ps1_boot_channel(void) { return s.ps1_boot_channel < CHAN_MIN ? CHAN_MIN : s.ps1_boot_channel; }
void settings_set_ps1_card(int x) { s.ps1_card = x; }
void settings_set_ps1_channel(int x) { s.ps1_channel = x; }
void settings_set_ps1_boot_channel(int x) { s.ps1_boot_channel = x; }
bool settings_get_ps1_controllercombo(void) { return s.ps1_controllercombo; }
void settings_set_ps1_controllercombo(bool x) { s.ps1_controllercombo = x; }
uint8_t settings_get_ps1_maxcardidx(void) { return s.ps1_maxcardidx; }
void settings_set_ps1_maxcardidx(uint8_t x) { s.ps1_maxcardidx = x; }
int settings_get_mode(bool current) { (void)current; return MODE_PS1; }
void settings_set_mode(int mode) { (void)mode; }
bool settings_get_ps1_autoboot(void) { return s.ps1_autoboot; }
void settings_set_ps1_autoboot(bool x) { s.ps1_autoboot = x; }
bool settings_get_ps1_game_id(void) { return s.ps1_game_id; }
void settings_set_ps1_game_id(bool x) { s.ps1_game_id = x; }
int settings_get_ps2_variant(void) { return PS2_VARIANT_RETAIL; }

/* Super MemCard's identity: the chip ID and the identity's flash page, in the card's flash (kept across power-offs) */
void smc_hw_uid(uint8_t uid[8]) {
    static const uint8_t zero[8];
    if (!memcmp(s.smc_uid, zero, 8)) {
        uint64_t r = get_rand_64();
        memcpy(s.smc_uid, &r, 8);
    }
    memcpy(uid, s.smc_uid, 8);
}
void smc_hw_ident_read(uint8_t *buf, int len) { memcpy(buf, s.smc_ident, (size_t)len); }
void smc_hw_ident_write(const uint8_t *buf, int len) {
    memset(s.smc_ident, 0xFF, sizeof(s.smc_ident));
    memcpy(s.smc_ident, buf, (size_t)len);
}

void input_task(void) {}
int input_get_pressed(void) { return 0; }
int input_is_any_down(void) { return 0; }

const char *log_level_str[] = {"", "ERROR", "WARN", "INFO", "TRACE"};
void buffered_printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
}
void fatal(int err, const char *format, ...) {
    va_list ap;
    fprintf(stderr, "card fatal error %d: ", err);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

/* The game DB is linked in from database/ on the card. An empty one here: no parent IDs, so a
 * game's card folder is its own ID (what the card does for a game not in its DB). The _size
 * symbols are set with --defsym, as objcopy does. */
const char _binary_gamedbps1_dat_start[8], _binary_gamedbps2_dat_start[8], _binary_gamedbcoh_dat_start[8];
