/* Super MemCard host library (see smc.h). */
#include "smc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { C_HELLO = 0x60, C_SELECT, C_READ, C_WRITE, C_CREATE, C_COMMIT, C_STATUS, C_LIST, C_INFO, C_CORE, C_DELETE };
enum { GOOD = 0x47, BAD_CHECKSUM = 0x4E, NOT_FOUND = 0x4F, DELETED = 0x44 };
#define RETRIES 3

int smc_frame(const struct smc_bus *b, uint8_t cmd, const uint8_t *req, int rlen, uint8_t *ans, int alen) {
    if (b->frame) {
        uint8_t out[2 + 256], in[2 + 256];
        int n = 2 + rlen + alen;
        if (n > (int)sizeof(out))
            return SMC_E_BAD;
        out[0] = 0x81;
        out[1] = cmd;
        if (rlen)
            memcpy(out + 2, req, (size_t)rlen);
        memset(out + 2 + rlen, 0, (size_t)alen);
        int got = b->frame(b->ctx, out, in, n);
        if (got < n)
            return SMC_E_NO_REPLY; /* stopped early: a byte wasn't ACKed */
        memcpy(ans, in + 2 + rlen, (size_t)alen);
        return SMC_OK;
    }
    bool ack = false;
    int rc = SMC_OK;
    b->select(b->ctx, true);
    b->xfer(b->ctx, 0x81, false, &ack);
    if (!ack) {
        rc = SMC_E_NO_REPLY;
        goto out;
    }
    b->xfer(b->ctx, cmd, false, &ack); /* the card answers FLAG here */
    if (!ack) {
        rc = SMC_E_NO_REPLY;
        goto out;
    }
    for (int i = 0; i < rlen; i++) {
        b->xfer(b->ctx, req[i], false, &ack);
        if (!ack) {
            rc = SMC_E_NO_REPLY;
            goto out;
        }
    }
    for (int i = 0; i < alen; i++) {
        bool last = i + 1 == alen;
        ans[i] = b->xfer(b->ctx, 0x00, last, &ack);
        if (!last && !ack) {
            rc = SMC_E_NO_REPLY;
            goto out;
        }
    }
out:
    b->select(b->ctx, false);
    return rc;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

int smc_hello(const struct smc_bus *b, uint8_t host, uint8_t caps, struct smc_card *card) {
    const uint8_t req[5] = {'S', 'M', SMC_PROTOCOL, host, caps};
    uint8_t a[16];
    int rc = SMC_E_NO_REPLY;
    for (int i = 0; i < 2 && rc == SMC_E_NO_REPLY; i++) {
        /* A card that refuses a command (a Sony card: Hello isn't one of its commands) sometimes
         * doesn't answer the next frame at all when it comes within ~20 ms; after 50 ms it always
         * does (seen on the SS1, 08). So the second try waits. */
        if (i)
            usleep(60 * 1000);
        rc = smc_frame(b, C_HELLO, req, 5, a, 16);
    }
    if (rc == SMC_E_NO_REPLY)
        return 0;
    if (a[0] != 'S' || a[1] != 'M')
        return 0;
    if (card) {
        card->version = a[2];
        card->features = a[3];
        for (int i = 0; i < 8; i++)
            snprintf(card->id + i * 2, 3, "%02x", a[4 + i]);
        card->free_kb = be32(a + 12);
    }
    return 1;
}

static int put_name(uint8_t *p, const char *s, size_t max) {
    size_t n = strlen(s);
    if (n == 0 || n > max)
        return -1;
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return (int)n + 1;
}

int smc_select_card(const struct smc_bus *b, const char *sys, const char *game, int card, int *card_out) {
    uint8_t req[3 + SMC_SYS_MAX + SMC_GAME_MAX], a[3];
    if (card < 0 || card > SMC_CARDS_MAX)
        return SMC_E_BAD;
    int n = put_name(req, sys, SMC_SYS_MAX);
    if (n < 0)
        return SMC_E_BAD;
    int m = put_name(req + n, game, SMC_GAME_MAX);
    if (m < 0)
        return SMC_E_BAD;
    req[n + m] = (uint8_t)card;
    int rc = smc_frame(b, C_SELECT, req, n + m + 1, a, 3);
    if (rc)
        return rc;
    if (a[0] != GOOD)
        return SMC_E_BAD;
    if (card_out)
        *card_out = a[2];
    return a[1];
}

int smc_select(const struct smc_bus *b, const char *sys, const char *game) {
    return smc_select_card(b, sys, game, 0, NULL);
}

int smc_core(const struct smc_bus *b, const char *sys, bool running) {
    uint8_t req[2 + SMC_SYS_MAX], a[2];
    int n = put_name(req, sys ? sys : ".", SMC_SYS_MAX);
    if (n < 0)
        return SMC_E_BAD;
    req[n] = running ? 1 : 0;
    /* Core changes nothing twice, so a missed frame is simply asked again (as Hello is) */
    int rc = SMC_E_NO_REPLY;
    for (int i = 0; i < 2 && rc == SMC_E_NO_REPLY; i++) {
        if (i)
            usleep(60 * 1000);
        rc = smc_frame(b, C_CORE, req, n + 1, a, 2);
    }
    if (rc)
        return rc;
    return a[0] == GOOD ? a[1] : SMC_E_BAD;
}

int smc_info(const struct smc_bus *b, int idx, struct smc_file *f) {
    uint8_t req[1] = {(uint8_t)idx}, a[29];
    int rc = smc_frame(b, C_INFO, req, 1, a, 29);
    if (rc)
        return rc;
    if (a[0] != GOOD && a[0] != DELETED)
        return a[0] == NOT_FOUND ? SMC_E_NOT_FOUND : SMC_E_BAD;
    memcpy(f->name, a + 1, 16);
    f->name[16] = 0;
    f->size = be32(a + 17);
    f->gen = be32(a + 21);
    f->time = be32(a + 25);
    f->deleted = a[0] == DELETED;
    return SMC_OK;
}

int smc_delete(const struct smc_bus *b, int idx, uint32_t gen, uint32_t time) {
    uint8_t req[9] = {(uint8_t)idx}, a[1];
    put32(req + 1, gen);
    put32(req + 5, time);
    int rc = smc_frame(b, C_DELETE, req, 9, a, 1);
    if (rc)
        return rc;
    return a[0] == GOOD ? SMC_OK : SMC_E_BAD;
}

int smc_read_file(const struct smc_bus *b, int idx, uint32_t size, uint8_t *buf) {
    for (uint32_t blk = 0; blk * 128 < size; blk++) {
        uint8_t req[3] = {(uint8_t)idx, (uint8_t)(blk >> 8), (uint8_t)blk}, a[130];
        int tries = 0, rc;
        for (;;) {
            rc = smc_frame(b, C_READ, req, 3, a, 130);
            uint8_t chk = req[1] ^ req[2];
            for (int i = 0; i < 128; i++)
                chk ^= a[i];
            if (rc == SMC_OK && a[129] == GOOD && a[128] == chk)
                break;
            if (rc == SMC_OK && a[129] != GOOD)
                return SMC_E_BAD;
            if (++tries == RETRIES)
                return rc ? rc : SMC_E_CHECKSUM;
        }
        uint32_t n = size - blk * 128 < 128 ? size - blk * 128 : 128;
        memcpy(buf + blk * 128, a, n);
    }
    return SMC_OK;
}

int smc_write_file(const struct smc_bus *b, const char *name, const uint8_t *data, uint32_t size, uint32_t gen) {
    return smc_write_file_at(b, name, data, size, gen, 0);
}

int smc_write_file_at(const struct smc_bus *b, const char *name, const uint8_t *data, uint32_t size, uint32_t gen,
                      uint32_t time) {
    uint8_t req[136], a[2];
    size_t nl = strlen(name);
    if (nl == 0 || nl > 16 || size == 0)
        return SMC_E_BAD;
    put32(req, size);
    req[4] = (uint8_t)nl;
    memcpy(req + 5, name, nl);
    int rc = smc_frame(b, C_CREATE, req, 5 + (int)nl, a, 2);
    if (rc)
        return rc;
    if (a[0] != GOOD)
        return SMC_E_BAD;
    uint8_t file = a[1];

    for (uint32_t blk = 0; blk * 128 < size; blk++) {
        uint32_t n = size - blk * 128 < 128 ? size - blk * 128 : 128;
        req[0] = file;
        req[1] = (uint8_t)(blk >> 8);
        req[2] = (uint8_t)blk;
        memset(req + 3, 0, 128);
        memcpy(req + 3, data + blk * 128, n);
        uint8_t chk = req[1] ^ req[2];
        for (int i = 0; i < 128; i++)
            chk ^= req[3 + i];
        req[131] = chk;
        int tries = 0;
        for (;;) {
            rc = smc_frame(b, C_WRITE, req, 132, a, 1);
            if (rc == SMC_OK && a[0] == GOOD)
                break;
            if (rc == SMC_OK && a[0] != BAD_CHECKSUM)
                return SMC_E_BAD;
            /* 4E or no reply: send it again (never read back to check, 08 finding 3) */
            if (++tries == RETRIES)
                return rc ? rc : SMC_E_CHECKSUM;
        }
    }

    req[0] = file;
    put32(req + 1, gen);
    put32(req + 5, time);
    rc = smc_frame(b, C_COMMIT, req, 9, a, 1);
    if (rc)
        return rc;
    return a[0] == GOOD ? SMC_OK : SMC_E_BAD;
}

int smc_pending(const struct smc_bus *b) {
    uint8_t a[2];
    int rc = smc_frame(b, C_STATUS, NULL, 0, a, 2);
    return rc ? rc : (a[0] << 8 | a[1]);
}

int smc_list(const struct smc_bus *b, const char *sys, int idx, char game[65]) {
    return smc_list_card(b, sys, 0, idx, game);
}

int smc_list_card(const struct smc_bus *b, const char *sys, int card, int idx, char game[65]) {
    uint8_t req[4 + SMC_SYS_MAX], a[1 + SMC_GAME_MAX];
    if (card < 0 || card > SMC_CARDS_MAX)
        return SMC_E_BAD;
    int n = put_name(req, sys, SMC_SYS_MAX);
    if (n < 0)
        return SMC_E_BAD;
    req[n] = (uint8_t)card;
    req[n + 1] = (uint8_t)(idx >> 8);
    req[n + 2] = (uint8_t)idx;
    int rc = smc_frame(b, C_LIST, req, n + 3, a, 1 + SMC_GAME_MAX);
    if (rc)
        return rc;
    if (a[0] != GOOD)
        return SMC_E_NOT_FOUND;
    memcpy(game, a + 1, SMC_GAME_MAX);
    game[SMC_GAME_MAX] = 0;
    return SMC_OK;
}
