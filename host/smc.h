/* Super MemCard host library: talks to the card over the PS1 memory card bus (docs/05-design.md).
 *
 * The bus is given as two callbacks, so the same code runs over the SS1's front ports (the Super MemCard front-port
 * driver, core/sys) and over the virtual card's socket (sim/card vcardd) while there's no real card.
 * Plain C, no allocation, for any MiSTer Main (Console Mode's, the test Main). */
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
    /* Optional, used instead of select/xfer when set: a whole frame in one go, as the SS1's front
     * ports do it (core/sys smc_port: SMC_RAW). Sends out[0..len-1] in one select, puts the card's byte for
     * each into in[k], stops after the first byte that isn't ACKed (the last one never is). Returns
     * the bytes exchanged (0: nothing answered), <0 on a transport error. */
    int (*frame)(void *ctx, const uint8_t *out, uint8_t *in, int len);
    /* Optional: a frame came back whole but its data failed its checksum (a read's, or the card's 4E for a
     * write). A bus with speeds steps down: a bus too fast for a card can corrupt bytes without losing any. */
    void (*bad_data)(void *ctx);
};
/* 167: the most the SS1's front ports wait (smc_port's ACK wait). A real card (BitFunX Gen2) took up to ~90 ms to ACK a
 * write while its SD card wrote, and with frames back to back (smcd) a commit answered past 100 ms: the card had
 * committed, the host had given up (08). */
#define SMC_ACK_TIMEOUT_MS 167

enum {
    SMC_OK = 0,
    SMC_E_NO_REPLY = -1, /* the card didn't ACK: plain card, no card, or busy */
    SMC_E_BAD = -2,      /* the card answered but refused (bad name, no space, ...) */
    SMC_E_CHECKSUM = -3, /* kept failing its checksum */
    SMC_E_NOT_FOUND = -4,
};

/* host IDs (Hello) */
enum { SMC_HOST_MAIN = 1, SMC_HOST_CONSOLE_MODE = 2, SMC_HOST_CORE = 3, SMC_HOST_TEST = 0x7F };

struct smc_card {
    uint8_t version, features;
    char id[17];      /* the card's own ID, 16 hex digits */
    uint32_t free_kb; /* 0xFFFFFFFF: unknown */
};

struct smc_file {
    char name[17];
    uint32_t size, gen;
    uint32_t time; /* when the save was made (Unix time, UTC; 0: not known) */
    bool deleted; /* a tombstone: deleted at generation gen (size 0, nothing to read) */
};

/* Hello, asked twice (the first command after power-up or a card switch can go unanswered).
 * 1: a Super MemCard card (filled in), 0: a plain PS1 card or no card, <0: error. */
int smc_hello(const struct smc_bus *b, uint8_t host, uint8_t caps, struct smc_card *card);

/* Select system (Main's save folder name, e.g. "SNES", "TGFX16") and game ID on the system's current
 * virtual card. Returns the number of save files the card has for it (0..16), or <0. */
int smc_select(const struct smc_bus *b, const char *sys, const char *game);
/* The same on virtual card `card` (1..99; 0: the system's current one); *card_out gets the card used. */
int smc_select_card(const struct smc_bus *b, const char *sys, const char *game, int card, int *card_out);
/* Core: the system's current virtual card (1..99), or <0. running: this system's core is running now,
 * so the card shows it and its buttons pick this system's card; sys NULL with running: none is (the
 * buttons are the PS1 card's again). running false only asks. */
int smc_core(const struct smc_bus *b, const char *sys, bool running);
/* Core, setting the system's virtual card to use from now on (a save picked on the SuperStation). The card, or <0. */
int smc_core_set_card(const struct smc_bus *b, const char *sys, int card);
/* Core with the card's screen for the running system (card version 3 on, SMC_PROTOCOL_GAME): its lines are the header
 * (the system's name), the game, when its save was made, the save's state and its size; NULL or "" : empty. The game
 * line takes 48 characters (it scrolls), the others 16. The system's current card, or <0. */
#define SMC_SCREEN_LINES 5
#define SMC_GAME_NAME_MAX 48
int smc_core_screen(const struct smc_bus *b, const char *sys, const char *const lines[SMC_SCREEN_LINES]);
/* "Final Fantasy III (USA).sfc" -> "Final Fantasy III" (no folder, extension or bracketed tags; printable ASCII) */
void smc_display_name(const char *game, char *out, size_t n);
/* File `idx` (0..n-1) of the selected game (deleted ones too: f->deleted). */
int smc_info(const struct smc_bus *b, int idx, struct smc_file *f);
/* Delete file `idx` of the selected game at generation `gen` (its generation + 1): the card keeps a tombstone so
 * other SuperStations delete their copy too, and the file itself aside. */
int smc_delete(const struct smc_bus *b, int idx, uint32_t gen, uint32_t time);
/* Read file `idx` whole into buf (at least f.size bytes). */
int smc_read_file(const struct smc_bus *b, int idx, uint32_t size, uint8_t *buf);
/* Write a whole save file and commit it with generation `gen`. The card keeps the previous copy
 * until the commit, so a failure here leaves the card's last good save in place. */
int smc_write_file(const struct smc_bus *b, const char *name, const uint8_t *data, uint32_t size, uint32_t gen);
/* The same with the time the save was made (Unix time, UTC), kept on the card beside it. */
int smc_write_file_at(const struct smc_bus *b, const char *name, const uint8_t *data, uint32_t size, uint32_t gen,
                      uint32_t time);
/* Blocks still waiting for the card's SD card (0: safe to remove). */
int smc_pending(const struct smc_bus *b);
/* The idx-th game ID the card has for a system (sys "." : the idx-th system); SMC_E_NOT_FOUND past
 * the end. */
int smc_list(const struct smc_bus *b, const char *sys, int idx, char game[65]);
/* The same on virtual card `card` (0: the system's current one). */
int smc_list_card(const struct smc_bus *b, const char *sys, int card, int idx, char game[65]);
#define SMC_SYS_MAX 32
#define SMC_GAME_MAX 64
#define SMC_CARDS_MAX 99
#define SMC_PROTOCOL 2 /* 2: Core and virtual cards (the least a card needs) */
#define SMC_PROTOCOL_GAME 3 /* the card takes its screen in Core (smc_core_screen) */

/* ---- the card's identity (docs/12-card-identity.md) ---- */
#define SMC_FEATURE_IDENTITY 0x10 /* Hello's features: the card has CERT, AUTH and AUTHSIG */
/* CERT: the card's chip ID, and its public key and Retro Remake's certificate. 1: the card has an identity (all
 * filled in), 0: none (only chip filled in: what Retro Remake's smccert issues an identity for), <0: error. */
int smc_cert(const struct smc_bus *b, uint8_t chip[8], uint8_t pub[32], uint8_t cert[64]);
/* AUTH: the card signs this challenge (after answering: AUTHSIG gets the signature). SMC_OK, or <0
 * (SMC_E_NOT_FOUND: no identity). */
int smc_auth(const struct smc_bus *b, const uint8_t challenge[32]);
/* AUTHSIG: 1 the signature (sig filled in), 0 not signed yet, SMC_E_NOT_FOUND no challenge, <0 error. */
int smc_authsig(const struct smc_bus *b, uint8_t sig[64]);
/* Is this a genuine Retro Remake card? Checks its certificate against Retro Remake's key (host/smc_rrkey.h) and
 * that it holds the key the certificate names (a random challenge, signed). 1: genuine, 0: not (why says why),
 * <0: the card stopped answering. A card that isn't genuine is used as a PS1 card only. */
int smc_genuine(const struct smc_bus *b, const struct smc_card *card, char *why, size_t why_len);
/* smcd built with the test key (tools/smccert/test-rr.key) instead of Retro Remake's */
bool smc_genuine_test_key(void);

/* A whole command frame (81 cmd, request, answer); exposed for tests. Returns SMC_OK or
 * SMC_E_NO_REPLY. */
int smc_frame(const struct smc_bus *b, uint8_t cmd, const uint8_t *req, int rlen, uint8_t *ans, int alen);
