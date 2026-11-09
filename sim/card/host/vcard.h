/* The virtual Super MemCard: sd2psXtd's PS1 card firmware running on a PC.
 *
 * The bus side is what a PS1 or the SS1 sees on the memory card port: select, then bytes
 * clocked out on CMD with the card's reply on DAT and an ACK after each byte it wants more of.
 * The SD card is a folder. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Power up the card with `sd_root` as its SD card (it must exist). Runs the firmware's own
 * ps1_init(): the card file is opened or created like on the real card. Each power-up is a
 * fresh card process; its settings (host_settings.h) persist like the card's flash. */
int vcard_start(const char *sd_root);
/* Power off cleanly: the firmware's own exit path flushes everything to the SD folder. */
void vcard_stop(void);
/* Power lost: the card process is killed wherever it is. */
void vcard_power_cut(void);

/* SEL low (true) or high (false). Raising SEL ends a command, like the PS1 does. */
void vcard_select(bool selected);

/* Clock one byte out on CMD. Returns the byte the card put on DAT (0xFF when it drives
 * nothing) and whether it ACKed (wants another byte). `last` = the host won't wait long
 * for an ACK, as for the last byte of a command. */
uint8_t vcard_xfer(uint8_t cmd, bool last, bool *ack);
/* The same for a controller frame: `dat` is what the controller put on DAT for this byte,
 * which the card listens to for its controller combo. */
uint8_t vcard_xfer_ctrl(uint8_t cmd, uint8_t dat, bool last, bool *ack);

/* Wait for the card to finish: card switches done and every written sector flushed to SD. */
void vcard_settle(void);
/* A press of the card's buttons on its main screen: 0 left, 1 right, 2 back, 3 enter; or an item of its
 * SuperStation menu: count the free space, saves off, saves on. */
enum { VKEY_PREV, VKEY_NEXT, VKEY_BACK, VKEY_ENTER, VMENU_FREE, VMENU_SAVES_OFF, VMENU_SAVES_ON };
void vcard_key(int key);

/* The card file the firmware has open, relative to the SD root ("" when none). */
const char *vcard_card_path(void);
