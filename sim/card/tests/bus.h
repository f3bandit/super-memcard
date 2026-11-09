/* The host end of the memory card port: whole commands, as a PS1 (or the SS1) sends them. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One select..deselect frame. out[i] is clocked out; in[i] gets the card's byte and ack[i]
 * whether it ACKed. The host stops early at the first byte without an ACK, like the PS1.
 * Returns the number of bytes sent. */
size_t bus_frame(const uint8_t *out, uint8_t *in, bool *ack, size_t n);

/* A first frame after power-up, so the card has seen SEL rise once. */
void bus_wake(void);

enum { MC_GOOD = 0x47, MC_BAD_CHECKSUM = 0x4E, MC_BAD_SECTOR = 0xFF, MC_NO_REPLY = -1 };

/* 81 53: the card's ID. Returns the 10 reply bytes after the address byte (FLAG 5A 5D 5C 5D 04 00 00 80 ..). */
int mc_get_id(uint8_t reply[9]);
/* 81 52: one 128-byte sector. Returns the end byte (MC_GOOD) or MC_NO_REPLY; checks the
 * confirmed address and checksum (-2 if they're wrong). */
int mc_read(uint16_t sector, uint8_t data[128]);
/* 81 57: write one sector. `chk_xor` is XORed into the checksum (to send a bad one). */
int mc_write(uint16_t sector, const uint8_t data[128], uint8_t chk_xor);
/* 81 57 cut off after `bytes` data bytes (card pulled, or the host gave up). */
void mc_write_cut(uint16_t sector, const uint8_t data[128], int bytes);
/* 81 21: MMCE game ID, e.g. "cdrom:\\SLUS_012.34;1". Returns bytes ACKed. */
int mmce_game_id(const char *id);
/* 81 20: MMCE ping. Returns 0 on the expected reply. */
int mmce_ping(void);
/* The FLAG byte from the last command. */
extern uint8_t mc_flag;
