/* Super MemCard host library: talks to the card over the PS1 memory card bus (docs/05-design.md).
 *
 * The bus is given as two callbacks, so the same code runs over the SS1's front port (Phosphor's
 * PH_CARD) and over the virtual card's socket (sim/card vcardd) while there's no real card.
 * Plain C, no allocation, for Phosphor's Main and Console Mode alike. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct smc_bus {
    void *ctx;
    /* SEL low (true) / high (false) */
    void (*select)(void *ctx, bool selected);
    /* clock one byte out; returns the card's byte, *ack = it ACKed. `last`: the host won't wait
     * for an ACK. The ACK after a Super MemCard request can be milliseconds late (SD access):
     * the bus must wait at least SMC_ACK_TIMEOUT_MS for it. */
    uint8_t (*xfer)(void *ctx, uint8_t out, bool last, bool *ack);
};
#define SMC_ACK_TIMEOUT_MS 100

enum {
    SMC_OK = 0,
    SMC_E_NO_REPLY = -1, /* the card didn't ACK: plain card, no card, or busy */
    SMC_E_BAD = -2,      /* the card answered but refused (bad name, no space, ...) */
    SMC_E_CHECKSUM = -3, /* kept failing its checksum */
    SMC_E_NOT_FOUND = -4,
};

/* host IDs (Hello) */
enum { SMC_HOST_PHOSPHOR = 1, SMC_HOST_CONSOLE_MODE = 2, SMC_HOST_CORE = 3, SMC_HOST_TEST = 0x7F };

struct smc_card {
    uint8_t version, features;
    char id[17];      /* the card's own ID, 16 hex digits */
    uint32_t free_kb; /* 0xFFFFFFFF: unknown */
};

struct smc_file {
    char name[17];
    uint32_t size, gen;
};

/* Hello, asked twice (the first command after power-up or a card switch can go unanswered).
 * 1: a Super MemCard card (filled in), 0: a plain PS1 card or no card, <0: error. */
int smc_hello(const struct smc_bus *b, uint8_t host, uint8_t caps, struct smc_card *card);

/* Select system (Main's save folder name, e.g. "SNES", "TGFX16") and game ID. Returns the number
 * of save files the card has for it (0..16), or <0. */
int smc_select(const struct smc_bus *b, const char *sys, const char *game);
/* File `idx` (0..n-1) of the selected game. */
int smc_info(const struct smc_bus *b, int idx, struct smc_file *f);
/* Read file `idx` whole into buf (at least f.size bytes). */
int smc_read_file(const struct smc_bus *b, int idx, uint32_t size, uint8_t *buf);
/* Write a whole save file and commit it with generation `gen`. The card keeps the previous copy
 * until the commit, so a failure here leaves the card's last good save in place. */
int smc_write_file(const struct smc_bus *b, const char *name, const uint8_t *data, uint32_t size, uint32_t gen);
/* Blocks still waiting for the card's SD card (0: safe to remove). */
int smc_pending(const struct smc_bus *b);
/* The idx-th game ID the card has for a system; SMC_E_NOT_FOUND past the end. */
int smc_list(const struct smc_bus *b, const char *sys, int idx, char game[33]);

/* A whole command frame (81 cmd, request, answer); exposed for tests. Returns SMC_OK or
 * SMC_E_NO_REPLY. */
int smc_frame(const struct smc_bus *b, uint8_t cmd, const uint8_t *req, int rlen, uint8_t *ans, int alen);
