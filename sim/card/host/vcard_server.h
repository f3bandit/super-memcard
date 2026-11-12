/* Inside the card process: the card itself (host/vcard.c). The test side uses vcard.h. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
int vcs_start(const char *sd_root);
void vcs_stop(void);
void vcs_select(bool selected);
uint8_t vcs_xfer(uint8_t cmd, bool last, bool *ack);
uint8_t vcs_xfer_ctrl(uint8_t cmd, uint8_t dat, bool last, bool *ack);
void vcs_settle(void);
const char *vcs_card_path(void);
uint8_t vcs_peek(void); /* the byte queued for the next transfer, FF when none */
const char *vcs_browse(void);   /* the card menu's Saves on card: "<system>=<games>;..." (the systems it shows) */
const char *vcs_screen(void);   /* the system mode the screen shows: "<system>|<header>|<game>|<saved>|<status>|<size>", "" : the PS1 card */
void vcs_key(int key);   /* the card's buttons on its main screen (vcard_wire.h OP_KEY) */
