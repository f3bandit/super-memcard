/* Super MemCard host library (see smc.h). */
#include "smc.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { C_HELLO = 0x60, C_SELECT, C_READ, C_WRITE, C_CREATE, C_COMMIT, C_STATUS, C_LIST, C_INFO, C_CORE, C_DELETE,
       C_CERT, C_AUTH, C_AUTHSIG, C_PROGRESS, C_UPDATE, C_SCREEN };
enum { U_BEGIN, U_DATA, U_CHECK, U_APPLY, U_INFO };
enum { GOOD = 0x47, BAD_CHECKSUM = 0x4E, NOT_FOUND = 0x4F, DELETED = 0x44, PENDING = 0x50 };

/* the card's progress bar during transfers (smc.h smc_xfer_text): every PROGRESS_BLOCKS blocks, a file's share done */
const char *smc_xfer_text;
#define PROGRESS_BLOCKS 128
static void xfer_progress(const struct smc_bus *b, uint32_t blk, uint32_t size) {
    uint32_t blocks = (size + 127) / 128;
    if (smc_xfer_text && blocks > PROGRESS_BLOCKS && blk % PROGRESS_BLOCKS == 0)
        smc_progress(b, (int)(blk * 100 / blocks), smc_xfer_text);
}

int smc_progress(const struct smc_bus *b, int pct, const char *text) {
    uint8_t req[2 + SMC_PROGRESS_TEXT], a[1];
    size_t tl = text ? strlen(text) : 0;
    if (tl > SMC_PROGRESS_TEXT)
        tl = SMC_PROGRESS_TEXT;
    req[0] = (uint8_t)(pct < 0 ? 0xFF : pct > 100 ? 100 : pct);
    req[1] = (uint8_t)tl;
    memcpy(req + 2, text, tl);
    int rc = smc_frame(b, C_PROGRESS, req, 2 + (int)tl, a, 1);
    return rc != SMC_OK ? rc : a[0] == GOOD ? SMC_OK : SMC_E_BAD;
}
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

int smc_core_screen(const struct smc_bus *b, const char *sys, const char *const lines[SMC_SCREEN_LINES]) {
    uint8_t req[3 + SMC_SYS_MAX + SMC_SCREEN_LINES * (1 + SMC_GAME_NAME_MAX)], a[2];
    if (!sys)
        return SMC_E_BAD;
    int n = put_name(req, sys, SMC_SYS_MAX);
    if (n < 0)
        return SMC_E_BAD;
    req[n++] = 0x01 | 0x04 | 0x08; /* running, SMC_CORE_SCREEN: the screen's lines follow; SMC_CORE_TZ: then the UTC offset */
    for (int l = 0; l < SMC_SCREEN_LINES; l++) {
        const char *t = lines && lines[l] ? lines[l] : "";
        size_t max = l == 1 ? SMC_GAME_NAME_MAX : 16, len = strlen(t);
        if (len > max)
            len = max;
        req[n++] = (uint8_t)len;
        for (size_t i = 0; i < len; i++)   /* printable ASCII: the card's font */
            req[n++] = (unsigned char)t[i] >= 0x20 && (unsigned char)t[i] < 0x7F ? (uint8_t)t[i] : ' ';
    }
    /* this SuperStation's UTC offset in minutes: the card shows its saves' times in local time on its own screens */
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int16_t tz = (int16_t)(lt.tm_gmtoff / 60);
    req[n++] = (uint8_t)((uint16_t)tz >> 8);
    req[n++] = (uint8_t)tz;
    int rc = SMC_E_NO_REPLY;
    for (int i = 0; i < 2 && rc == SMC_E_NO_REPLY; i++) {
        if (i)
            usleep(60 * 1000);
        rc = smc_frame(b, C_CORE, req, n, a, 2);
    }
    if (rc)
        return rc;
    return a[0] == GOOD ? a[1] : SMC_E_BAD;
}

void smc_display_name(const char *game, char *out, size_t n) {
    /* "Final Fantasy III (USA) (Rev 1).sfc" -> "Final Fantasy III": no folder, extension or tags; ASCII only */
    out[0] = 0;
    if (!game || n < 2)
        return;
    const char *s = strrchr(game, '/');
    s = s ? s + 1 : game;
    size_t len = strlen(s);
    const char *dot = strrchr(s, '.');
    if (dot && dot != s && strlen(dot) <= 5 && !strchr(dot, ' '))
        len = (size_t)(dot - s);
    for (size_t i = 0; i < len; i++)
        if ((s[i] == '(' || s[i] == '[') && i > 0) {
            len = i;
            break;
        }
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '_')
            ch = ' ';
        if (ch < 0x20 || ch >= 0x7F)
            continue;
        if (ch == ' ' && (o == 0 || out[o - 1] == ' '))
            continue;
        out[o++] = (char)ch;
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = 0;
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
        xfer_progress(b, blk, size);
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
            if (rc == SMC_OK && b->bad_data)
                b->bad_data(b->ctx);
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
        xfer_progress(b, blk, size);
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
            if (rc == SMC_OK && b->bad_data)
                b->bad_data(b->ctx);
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

int smc_cert(const struct smc_bus *b, uint8_t chip[8], uint8_t pub[32], uint8_t cert[64]) {
    uint8_t a[105];
    int rc = smc_frame(b, C_CERT, NULL, 0, a, sizeof(a));
    if (rc)
        return rc;
    if (a[0] != GOOD && a[0] != NOT_FOUND)
        return SMC_E_BAD;
    memcpy(chip, a + 1, 8);
    if (a[0] != GOOD)
        return 0;
    memcpy(pub, a + 9, 32);
    memcpy(cert, a + 41, 64);
    return 1;
}

int smc_auth(const struct smc_bus *b, const uint8_t challenge[32]) {
    uint8_t a[1];
    int rc = smc_frame(b, C_AUTH, challenge, 32, a, 1);
    if (rc)
        return rc;
    return a[0] == GOOD ? SMC_OK : a[0] == NOT_FOUND ? SMC_E_NOT_FOUND : SMC_E_BAD;
}

int smc_authsig(const struct smc_bus *b, uint8_t sig[64]) {
    uint8_t a[65];
    int rc = smc_frame(b, C_AUTHSIG, NULL, 0, a, sizeof(a));
    if (rc)
        return rc;
    if (a[0] == PENDING)
        return 0;
    if (a[0] != GOOD)
        return a[0] == NOT_FOUND ? SMC_E_NOT_FOUND : SMC_E_BAD;
    memcpy(sig, a + 1, 64);
    return 1;
}

int smc_core_set_card(const struct smc_bus *b, const char *sys, int card) {
    uint8_t req[3 + SMC_SYS_MAX], a[2];
    if (!sys || card < 1 || card > SMC_CARDS_MAX)
        return SMC_E_BAD;
    int n = put_name(req, sys, SMC_SYS_MAX);
    if (n < 0)
        return SMC_E_BAD;
    req[n] = 0x02; /* SMC_CORE_SET_CARD: the card's number follows */
    req[n + 1] = (uint8_t)card;
    int rc = SMC_E_NO_REPLY;
    for (int i = 0; i < 2 && rc == SMC_E_NO_REPLY; i++) {
        if (i)
            usleep(60 * 1000);
        rc = smc_frame(b, C_CORE, req, n + 2, a, 2);
    }
    if (rc)
        return rc;
    return a[0] == GOOD && a[1] == card ? card : SMC_E_BAD;
}

int smc_ps1_default_card(const struct smc_bus *b) {
    /* MMCE reset (81 27 + 4): the card leaves a game's own card for its default one (sd2psXtd ps1_cardman_switch_default) */
    uint8_t out[6] = {0x81, 0x27, 0, 0, 0, 0}, in[6];
    if (b->frame) {
        int got = b->frame(b->ctx, out, in, 6);
        return got == 6 ? SMC_OK : got < 0 ? got : SMC_E_NO_REPLY;
    }
    bool ack = false;
    int rc = SMC_OK;
    b->select(b->ctx, true);
    for (int i = 0; i < 6; i++) {
        bool last = i == 5;
        b->xfer(b->ctx, out[i], last, &ack);
        if (!last && !ack) {
            rc = SMC_E_NO_REPLY;
            break;
        }
    }
    b->select(b->ctx, false);
    return rc;
}

int smc_ps1_set_game_id(const struct smc_bus *b, const char *id) {
    size_t len = id ? strlen(id) : 0;
    if (len == 0 || len > 32)
        return SMC_E_BAD;
    uint8_t out[4 + 32], in[4 + 32];
    out[0] = 0x81;
    out[1] = 0x21;
    out[2] = 0x00;           /* reserved */
    out[3] = (uint8_t)len;
    memcpy(out + 4, id, len);
    int n = 4 + (int)len;
    if (b->frame) {
        /* the card ACKs every byte but the last (its answer ends there): all n exchanged is a whole command */
        int got = b->frame(b->ctx, out, in, n);
        return got == n ? SMC_OK : got < 0 ? got : SMC_E_NO_REPLY;
    }
    bool ack = false;
    int rc = SMC_OK;
    b->select(b->ctx, true);
    for (int i = 0; i < n; i++) {
        bool last = i + 1 == n;
        b->xfer(b->ctx, out[i], last, &ack);
        if (!last && !ack) {
            rc = SMC_E_NO_REPLY;
            break;
        }
    }
    b->select(b->ctx, false);
    return rc;
}

/* ---- the card's own firmware ---- */

/* zlib's CRC-32, as the card checks a firmware with (smc_sync's smc_crc32 with a table: 1.3 MB) */
static uint32_t crc_z(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
        crc = t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int smc_update_info(const struct smc_bus *b, int *loader, int *result, char version[25]) {
    uint8_t req[1] = {U_INFO}, a[27];
    int rc = smc_frame(b, C_UPDATE, req, 1, a, sizeof(a));
    if (rc)
        return rc;
    if (a[0] != GOOD)
        return SMC_E_BAD;
    if (loader)
        *loader = a[1];
    if (result)
        *result = a[2];
    if (version) {
        memcpy(version, a + 3, 24);
        version[24] = 0;
    }
    return SMC_OK;
}

int smc_update_send(const struct smc_bus *b, const uint8_t *fw, uint32_t size, int *why,
                    void (*progress)(void *ctx, int pct), void *ctx) {
    uint8_t req[132], a[2];
    if (why)
        *why = 0;
    req[0] = U_BEGIN;
    put32(req + 1, size);
    put32(req + 5, crc_z(0, fw, size));
    int rc = smc_frame(b, C_UPDATE, req, 9, a, 2);
    if (rc)
        return rc;
    if (a[0] != GOOD) {
        if (why)
            *why = a[1];
        return SMC_E_BAD;
    }
    uint32_t blocks = (size + 127) / 128;
    for (uint32_t blk = 0; blk < blocks; blk++) {
        uint32_t n = size - blk * 128 < 128 ? size - blk * 128 : 128;
        if (progress && blk % 64 == 0)
            progress(ctx, (int)(blk * 100 / blocks));
        req[0] = U_DATA;
        req[1] = (uint8_t)(blk >> 8);
        req[2] = (uint8_t)blk;
        memset(req + 3, 0, 128);
        memcpy(req + 3, fw + blk * 128, n);
        uint8_t chk = req[1] ^ req[2];
        for (int i = 0; i < 128; i++)
            chk ^= req[3 + i];
        req[131] = chk;
        for (int tries = 0;;) {
            rc = smc_frame(b, C_UPDATE, req, 132, a, 1);
            if (rc == SMC_OK && a[0] == GOOD)
                break;
            if (rc == SMC_OK && a[0] != BAD_CHECKSUM)
                return SMC_E_BAD;
            if (rc == SMC_OK && b->bad_data)
                b->bad_data(b->ctx);
            if (++tries == 6)   /* (a firmware is ten thousand blocks: a few more tries than a save's) */
                return rc ? rc : SMC_E_CHECKSUM;
        }
    }
    if (progress)
        progress(ctx, 100);
    /* the card reads it back on its own: about 2 s for a whole firmware */
    req[0] = U_CHECK;
    for (int t = 0; t < 300; t++) {
        rc = smc_frame(b, C_UPDATE, req, 1, a, 1);
        if (rc == SMC_OK && a[0] == GOOD)
            return SMC_OK;
        if (rc == SMC_OK && a[0] != PENDING)
            return SMC_E_BAD;   /* (*why 0: the check failed) */
        usleep(100 * 1000);
    }
    return SMC_E_NO_REPLY;
}

int smc_update_apply(const struct smc_bus *b) {
    uint8_t req[1] = {U_APPLY}, a[1];
    int rc = smc_frame(b, C_UPDATE, req, 1, a, 1);
    return rc ? rc : a[0] == GOOD ? SMC_OK : SMC_E_BAD;
}

int smc_screen(const struct smc_bus *b, const char *text) {
    uint8_t req[1 + SMC_SCREEN_TEXT], a[1];
    size_t n = text ? strlen(text) : 0;
    if (n > SMC_SCREEN_TEXT)
        n = SMC_SCREEN_TEXT;
    req[0] = (uint8_t)n;
    memcpy(req + 1, text, n);
    int rc = smc_frame(b, C_SCREEN, req, 1 + (int)n, a, 1);
    return rc ? rc : a[0] == GOOD ? SMC_OK : SMC_E_BAD;
}
