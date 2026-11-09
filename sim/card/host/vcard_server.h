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
