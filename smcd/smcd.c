/* smcd: Super MemCard on the SuperStation. A program of its own beside Main: Main only passes card frames to the
 * front ports for it and tells it when a game starts and when the menu is back (main/smc_shim.cpp); the memory manager
 * (Console Mode's frontend) sends it its commands. Everything Super MemCard does is here.
 * Copyright (C) 2026 f3bandit. Licensed under the Apache License 2.0 (LICENSE, NOTICE).
 *
 *   smcd [-f <root>] [-S <saves>] [-T <state>] [-g <games dir>]... [-d <game lists>] [-l <socket>] [-v <vcard socket>]
 *
 * Its socket (/tmp/smcd.sock), one command a line; the answer's lines end with END (LAUNCH and MENU: DONE; LAUNCH's is
 * "DONE saves" when the game's saves are on the card, for Main's OSD Saves page). No card
 * (or not a Super MemCard card): nocard; a Super MemCard card that isn't genuine (docs/12): notgenuine <card> <why>.
 *   LAUNCH <rbf>\t<mgl>     (Main) a game core is about to load: Core, and its save from the card if the card's is newer
 *   MENU                    (Main) the menu is back: the game just played goes to the card
 *   ROM <rbf>\t<file>\t<0|1> (Main) a game file picked in the running core's OSD (1: the core has the card driver):
 *                           DONE same (that game runs already) | DONE bounce (no driver: Main goes through the menu
 *                           core and starts the core again with the game, LAUNCH) | DONE saves | DONE (as LAUNCH)
 *   PROBE                   what each front port holds: port <n> smc <card id> <free kb, ? not counted yet> | port <n> ps1 | port <n> empty
 *   HELLO                   the card: id, version, free space
 *   SELECT <sys>\t<game ID>  (Coreshell, before a game's core loads) the game being started: PSX, that game's own PS1
 *                           card on the bus; others, the card in that system's mode. select ok [card] | select failed
 *   READ                    the memory manager's Read card: everything both ways
 *   SYSTEMS                 the systems with saves on the card
 *   LIST <sys> [<card>]     a system's games: name \t id \t time \t files \t bytes \t icon \t save name \t folder
 *                           (files 0: a game folder with no save yet); partial \t <n> last when n reads kept failing
 *   SECTION [PSX]           every other system's saves at once, for the memory manager's screen (section() below);
 *                           each game's icon on the card is kept in <state>/card_icons/<sys>/<id>.bin to show it
 *   SAVES                   (Main's OSD) the running game's saves: save \t card \t files \t time \t playing \t label, one per
 *                           virtual card that has it, then game \t sys \t name and new \t <the first free card>
 *   USE <card>              (Main's OSD) play the running game with its save on that card: this SuperStation's saves go
 *                           to their card first; Main then starts the game again so it loads it. use ok <card> | failed
 *   PORT <1|2>              (the memory manager) the card in that front port for this connection's commands
 *   CARDS <sys>             (the memory manager's card picker) that system's cards: card \t n \t games \t newest
 *                           \t in use, then new \t <the first blank card>
 *   CARD <sys>\t<n>         that system plays from card n from now on (a blank one: its games start with no save):
 *                           card ok <n> | card busy (a game of it runs) | card failed
 *   NAME <sys>\t<card>\t<save>\t<name>   (the memory manager's Rename) a name for a game's save on that card, kept on the
 *                           card; "" clears it. name ok | name nosave | name failed (too long: 24, or not printable ASCII)
 *   PLAY <sys>\t<card>\t<save>   (the memory manager's Play) start the game with its save on that card: the switch as
 *                           USE, then Main loads the game (the MGL of its last launch here, else one made from its
 *                           ROM). play ok <card> | play notfound (no ROM, or a system started only from an MGL) |
 *                           play psx | play failed
 *   PSXMERGE <backup dir>   Super MemCard's PSX copies into each game's own PS1 card (the PSX store from now on): backed
 *                           up into <backup dir> first; a line a game, then psxmerge ok <games> | psxmerge failed
 *   PLAYPREP <sys>\t<card>\t<save>   (Coreshell, for the memory manager's Play) PLAY's card switch only: play ok
 *                           <card>\t<the game's MGL> (Coreshell starts it) | play notfound | play psx | play failed
 *   IMAGE <sys>\t<card>\t<folder>  a game's save file as it is on the card (PSX: its 128 KB card image), to open it
 *                           save by save: image <bytes>, then its bytes in hex, 128 a line | image failed
 *   EXPORT <sys>\t<card>\t<id|*>\t<dir>   a game's files on that card (or every game's) into <dir>/<id>/: export ok
 *                           <games> <files> | export failed
 *   IMPORT <sys>\t<card>\t<id|*>\t<dir>   <dir>/<id>/ (or every game folder there) onto that card: import ok
 *                           <games> <files> | import failed
 *   DELGAME <sys>\t<card>\t<id>   that game's saves off that card only: delgame ok <files> | delgame failed
 *   DELETE <sys>\t<save>    delete a save (others delete theirs at their next sync)
 *   CLEAR <sys>\t<card>     take every save off that virtual card, the card only: no SuperStation loses one (one that
 *                           still has a save puts it back when it next syncs that card). clear ok <n> | clear busy |
 *                           clear failed
 *   CONFLICTS               saves whose two copies both changed
 *   KEEP <local|card> <sys>\t<save>
 * The card bus is one user's at a time (/tmp/super_memcard.busy, as cm_modules' cmod_mc_lock): smcd holds it while it has
 * the card open; when someone else does, it waits up to 10 s, then leaves the card alone until the next time.
 * While a game runs, its save goes to the card as soon as the game saves (its save files changed and settled). */
#define _GNU_SOURCE
#include "smc.h"
#include "smc_mcformat.h"
#include "smc_sync.h"
#include "smc_vcard.h"

#include <dirent.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define SMCD_SOCK  "/tmp/smcd.sock"
#define FRONT_SOCK "/tmp/smc_frontport.sock"
#define LOG_FILE   "/tmp/smcd.log"
#ifndef SMC_SPEED
#define SMC_SPEED 2   /* Super MemCard frames at 1 MHz; a frame not answered whole steps down one speed */
#endif

static const char *root = "/media/fat", *saves, *state, *lists, *sock_path = SMCD_SOCK, *vcard_path = SMC_VCARD_SOCKET;
static const char *games[16];
static int ngames;

/* ---- log ---- */
static void lg(const char *fmt, ...) {
    char l[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l, sizeof(l), fmt, ap);
    va_end(ap);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t t = time(0);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
        fprintf(f, "%s %s\n", ts, l);
        fclose(f);
    }
}
static void lib_log(const char *l) { lg("%s", l); }

/* ---- sockets ---- */
static int unix_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *s, size_t n) {
    for (size_t o = 0; o < n;) {
        ssize_t w = send(fd, s + o, n - o, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        o += (size_t)w;
    }
    return 0;
}

static void say(int fd, const char *fmt, ...) {
    char l[1200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(l, sizeof(l), fmt, ap);
    va_end(ap);
    if (n > 0)
        send_all(fd, l, (size_t)(n < (int)sizeof(l) ? n : (int)sizeof(l) - 1));
}

/* a line from fd (without its \n), waiting up to ms. 1: a line, 0: timeout, -1: closed */
static int read_line(int fd, char *buf, size_t n, int ms) {
    size_t len = 0;
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, ms);
        if (r <= 0)
            return r < 0 && errno == EINTR ? 0 : r;
        char c;
        ssize_t g = recv(fd, &c, 1, 0);
        if (g <= 0)
            return -1;
        if (c == '\n') {
            buf[len] = 0;
            return 1;
        }
        if (len + 1 < n)
            buf[len++] = c;
    }
}

/* read_line for Main's frame socket: a line of ~270 characters a frame, read a chunk at a time rather than a byte at
 * a time (two system calls a character took ~20 ms a frame on the SS1, more than the frame on the bus) */
static char fbuf[2048];
static size_t flen;
static int fbuf_fd = -1;
static int read_line_front(int fd, char *buf, size_t n, int ms) {
    if (fd != fbuf_fd) {   /* a new connection: nothing kept from the last one */
        fbuf_fd = fd;
        flen = 0;
    }
    for (;;) {
        char *nl = memchr(fbuf, '\n', flen);
        if (nl) {
            size_t l = (size_t)(nl - fbuf), k = l < n ? l : n - 1;
            memcpy(buf, fbuf, k);
            buf[k] = 0;
            memmove(fbuf, nl + 1, flen - l - 1);
            flen -= l + 1;
            return 1;
        }
        if (flen == sizeof(fbuf))
            flen = 0;   /* a line too long: dropped */
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, ms);
        if (r <= 0)
            return r < 0 && errno == EINTR ? 0 : r;
        ssize_t g = recv(fd, fbuf + flen, sizeof(fbuf) - flen, 0);
        if (g <= 0)
            return -1;
        flen += (size_t)g;
    }
}

/* ---- the card: through Main's front ports (smc_shim) or, for testing, vcardd ---- */
struct front {
    int fd, port;
};
static int frame_speed = SMC_SPEED;
static int frame_errs;   /* frames a card never answered whole, logged (the first 200) */

/* time on the bus, for the log: frames, and the milliseconds they took (Main's relay and the card) */
static unsigned long raw_frames, raw_retries;
static double raw_ms;
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static int front_raw_(struct front *f, const uint8_t *out, uint8_t *in, int len, int speed);
static int front_raw(struct front *f, const uint8_t *out, uint8_t *in, int len, int speed) {
    double t0 = now_ms();
    int n = front_raw_(f, out, in, len, speed);
    raw_ms += now_ms() - t0;
    raw_frames++;
    return n;
}
static int front_raw_(struct front *f, const uint8_t *out, uint8_t *in, int len, int speed) {
    char l[600];
    int k = snprintf(l, sizeof(l), "FRAME %d %d %d ", f->port, speed, SMC_ACK_TIMEOUT_MS);
    for (int i = 0; i < len; i++)
        k += snprintf(l + k, sizeof(l) - k, "%02x", out[i]);
    l[k++] = '\n';
    if (send_all(f->fd, l, (size_t)k))
        return -3;
    if (read_line_front(f->fd, l, sizeof(l), 2000 + SMC_ACK_TIMEOUT_MS * 4) != 1)
        return -3;
    if (strncmp(l, "OK ", 3))
        return atoi(l + 4) ? atoi(l + 4) : -1;
    char *h;
    int n = (int)strtol(l + 3, &h, 10);
    while (*h == ' ')
        h++;
    for (int i = 0; i < len; i++) {
        unsigned v = 0;
        if (sscanf(h + 2 * i, "%2x", &v) != 1)
            v = 0;
        in[i] = (uint8_t)v;
    }
    return n;
}

static int front_frame(void *ctx, const uint8_t *out, uint8_t *in, int len) {
    struct front *f = ctx;
    /* Hello as a PS1 does it; the identity's frames (CERT, AUTH, AUTHSIG) too: they have no checksum, and a bus
     * too fast for a card can corrupt a byte without losing any (seen on the SS1 with a BitFunX Gen2 at speed 1:
     * one CERT in three wrong, so a genuine card was refused) */
    int speed = (len > 1 && (out[1] == 0x60 || (out[1] >= 0x6B && out[1] <= 0x6D))) ? 0 : frame_speed;
    int n = front_raw(f, out, in, len, speed);
    while (speed && n >= 0 && n < len) {
        frame_speed = --speed;
        lg("a fast frame wasn't answered whole: speed %d from now on", speed);
        usleep(60000);
        n = front_raw(f, out, in, len, speed);
    }
    /* not answered at all (not even its first byte: the card never took it): once more after a moment. A card misses
     * a frame that comes just after another now and then (08; on the SS1 a BitFunX card in port 1 misses the frame
     * after Hello every time, so the identity check never finished) */
    if (n == 0) {
        raw_retries++;
        usleep(60000);
        n = front_raw(f, out, in, len, speed);
    }
    /* still nothing, or cut short at the slowest speed: once more after a longer moment (a pad poll or a card busy
     * writing its flash can take that long); then log it, so a card that "stops reading" says where (frame_errs) */
    if (n >= 0 && n < len && !speed) {
        usleep(150000);
        n = front_raw(f, out, in, len, speed);
    }
    if (n < len) {
        frame_errs++;
        if (frame_errs <= 200)
            lg("frame %02x (%d bytes) on port %d: %d answered%s (speed %d)", len > 1 ? out[1] : 0, len, f->port, n,
               n == -3 ? ": Main's relay didn't answer" : n < 0 ? ": Main refused it" : "", speed);
    }
    return n;
}

/* a whole frame whose data failed its checksum: one speed down from now on (as for a frame not answered whole) */
static void front_bad_data(void *ctx) {
    (void)ctx;
    if (frame_speed > 0) {
        frame_speed--;
        lg("a fast frame's data failed its checksum: speed %d from now on", frame_speed);
        usleep(60000);
    }
}

/* The fastest speed a card answers right, measured once per card: CERT (its 105-byte answer never changes for a card:
 * the chip ID, and its key and certificate or zeros) 10 times at each faster speed, compared byte for byte with CERT
 * at PS1 speed. A card that corrupts bytes at a speed without losing any (a BitFunX Gen2 at 500 kHz: about one byte in
 * 300, in status bytes and data alike, 08) is caught here before a save is sent. Hello is too short for it: 4 clean
 * Hellos at 500 kHz still let corrupted saves through. A frame not answered whole still steps down as before. */
static char speed_card[17];
static void front_calibrate(struct front *f, const char *card_id) {
    if (!strcmp(speed_card, card_id))
        return;   /* this card's speed is known (it can only have gone down since) */
    snprintf(speed_card, sizeof(speed_card), "%s", card_id);
    uint8_t out[107] = {0x81, 0x6B}, ref[107], in[107];
    int r = front_raw(f, out, ref, sizeof(out), 0);
    if (r == 0) {   /* (missed, as a frame just after another can be: once more, as front_frame) */
        usleep(60000);
        r = front_raw(f, out, ref, sizeof(out), 0);
    }
    if (r != (int)sizeof(out)) {
        frame_speed = 0;
        lg("card %s: speed 0 (no CERT to measure with)", card_id);
        return;
    }
    int sp;
    for (sp = SMC_SPEED; sp > 0; sp--) {
        int ok = 1;
        for (int t = 0; t < 10 && ok; t++) {
            usleep(10000);
            ok = front_raw(f, out, in, sizeof(out), sp) == (int)sizeof(out) && !memcmp(in + 2, ref + 2, 105);
        }
        if (ok)
            break;
        usleep(60000);   /* a card that missed a frame answers the next one after a moment (08) */
    }
    frame_speed = sp;
    lg("card %s: speed %d (the fastest it answered right)", card_id, sp);
}

struct conn {
    struct smc_bus bus;
    struct front front;
    int vcard;
    struct smc_card card;
};

static void hold(struct conn *c, int on) {
    char l[32];
    snprintf(l, sizeof(l), "HOLD %d\n", on);
    send_all(c->front.fd, l, strlen(l));
    read_line_front(c->front.fd, l, sizeof(l), 2000);
}

/* the last card that answered Hello but isn't a genuine Retro Remake card: the frontend's HELLO says so */
static char not_genuine_id[17], not_genuine_why[96];

/* Super MemCard only with a genuine Retro Remake card (docs/12-card-identity.md); any other card is a PS1 card here.
 * 1: genuine. -G (test builds only: the test key) uses a card without its identity, to test a new card before it has
 * one; a build with Retro Remake's key ignores it. */
static int allow_no_identity;
static int genuine(struct conn *c) {
    char why[96];
    int g = smc_genuine(&c->bus, &c->card, why, sizeof(why));
    for (int t = 0; g < 0 && t < 2; t++) {   /* a frame missed mid-check (the BFMC misses one now and then): again */
        usleep(200000);
        g = smc_genuine(&c->bus, &c->card, why, sizeof(why));
    }
    if (g == 1) {
        not_genuine_id[0] = 0;
        return 1;
    }
    if (g < 0) {
        lg("card %s: the identity check didn't finish (%s): not used this time", c->card.id, why[0] ? why : "no answer");
        return 0;
    }
    if (allow_no_identity && smc_genuine_test_key()) {
        static char said[17];
        if (strcmp(said, c->card.id))
            lg("TEST (-G): card %s used without its identity (%s)", c->card.id, why);
        snprintf(said, sizeof(said), "%s", c->card.id);
        return 1;
    }
    static char logged[17 + 96];   /* said once per card and reason */
    char key[sizeof(logged)];
    snprintf(key, sizeof(key), "%s %s", c->card.id, why);
    if (strcmp(logged, key)) {
        lg("card %s isn't a genuine Retro Remake card (%s): only a PS1 card here", c->card.id, why);
        snprintf(logged, sizeof(logged), "%s", key);
    }
    snprintf(not_genuine_id, sizeof(not_genuine_id), "%s", c->card.id);
    snprintf(not_genuine_why, sizeof(not_genuine_why), "%s", why);
    return 0;
}

/* ---- the front ports' card bus: one user at a time (cm_modules' cmmod.h CMOD_MC_BUSY, the same lock): a symbolic
 * link /tmp/super_memcard.busy -> "<pid> <who>", made in one step, free again when its owner is gone ---- */
#define MC_BUSY "/tmp/super_memcard.busy"

static int mc_busy(char *holder, size_t n) {
    char t[160];
    ssize_t l = readlink(MC_BUSY, t, sizeof(t) - 1);
    if (holder && n)
        holder[0] = 0;
    if (l <= 0)
        return 0;
    t[l] = 0;
    long pid = atol(t);
    if (pid <= 0 || (kill((pid_t)pid, 0) && errno == ESRCH)) {
        unlink(MC_BUSY);
        return 0;
    }
    const char *who = strchr(t, ' ');
    if (holder && n)
        snprintf(holder, n, "%s", who ? who + 1 : t);
    return 1;
}

static int mc_lock(int wait_ms, char *holder, size_t n) {
    char t[64];
    snprintf(t, sizeof(t), "%ld Super MemCard (smcd)", (long)getpid());
    for (int waited = 0;; waited += 100) {
        if (!symlink(t, MC_BUSY))
            return 0;
        if (!mc_busy(holder, n))
            continue;
        if (waited >= wait_ms)
            return 1;
        usleep(100000);
    }
}

static void mc_unlock(void) {
    char t[160];
    ssize_t l = readlink(MC_BUSY, t, sizeof(t) - 1);
    if (l > 0) {
        t[l] = 0;
        if (atol(t) == (long)getpid())
            unlink(MC_BUSY);
    }
}

static void icons_to_card(struct conn *c);
static void icon_wait(void);
static int fetch(const char *url, const char *to);

/* the card: vcardd if it runs, else the first front port whose card answers Hello (or only the port the memory manager
 * picked: PORT). 1: a card. */
static int want_port;   /* 0: whichever answers; 1 or 2: the memory manager's port (PORT), for its connection */
static int card_open_bus(struct conn *c);

static int files_card;   /* the card open is version 4 (set by card_open): PSX is each game's own PS1 card */
/* the frontend's "PSX": on a version 4 card each game's own PS1 card (what the game reads), Super MemCard's own PSX
 * folder before (PSXMERGE moves what's there) */
static const char *store_of(const char *sys) { return files_card && !strcmp(sys, "PSX") ? SMC_PS1CARD : sys; }
/* a game's card folder, not PS1 mode's own (Card1, Card2..., BOOT) */
static int ps1_game_folder(const char *id) {
    if (!strcmp(id, "BOOT"))
        return 0;
    if (!strncmp(id, "Card", 4) && id[4]) {
        for (const char *p = id + 4; *p; p++)
            if (*p < '0' || *p > '9')
                return 1;
        return 0;
    }
    return 1;
}

static int ps1card_drop_empty(struct conn *c, const char *id);
static int card_open(struct conn *c) {
    char who[120];
    memset(c, 0, sizeof(*c));
    c->front.fd = -1;
    files_card = 0;
    if (mc_lock(10000, who, sizeof(who))) {
        static char said[120];
        if (strcmp(said, who))
            lg("the card is in use (%s): not now", who);
        snprintf(said, sizeof(said), "%s", who);
        return 0;
    }
    if (card_open_bus(c)) {
        files_card = c->card.version >= SMC_PROTOCOL_FILES;
        return 1;
    }
    mc_unlock();
    return 0;
}

static int card_open_bus(struct conn *c) {
    struct stat st;
    if (stat(vcard_path, &st) == 0 && smc_vcard_open(vcard_path, &c->bus) == 0) {
        c->vcard = 1;
        if (smc_hello(&c->bus, SMC_HOST_MAIN, 0, &c->card) == 1 && genuine(c)) {
            icons_to_card(c);
            return 1;
        }
        smc_vcard_close(&c->bus);
        return 0;
    }
    c->front.fd = unix_connect(FRONT_SOCK);
    if (c->front.fd < 0)
        return 0;   /* Main isn't passing frames (no shim, or not running) */
    hold(c, 1);
    c->bus.frame = front_frame;
    c->bus.bad_data = front_bad_data;
    c->bus.ctx = &c->front;
    for (int p = want_port ? want_port : 1; p <= (want_port ? want_port : 2); p++) {
        c->front.port = p;
        int h = smc_hello(&c->bus, SMC_HOST_MAIN, 0, &c->card);
        if (h == 1 && genuine(c)) {
            front_calibrate(&c->front, c->card.id);
            icons_to_card(c);
            return 1;
        }
        if (h == 1)
            break;   /* a Super MemCard card, not genuine: a PS1 card (Main goes on as without Super MemCard) */
        if (h < 0 && p == 1 && !want_port) {
            uint8_t o[2] = {0x81, 0x60}, i[2];
            if (front_raw(&c->front, o, i, 0, 0) == -1)
                break;   /* no front-port driver in this core */
        }
    }
    usleep(100000);   /* a card that refused Hello can miss the next frame for a moment: leave it alone */
    hold(c, 0);
    close(c->front.fd);
    fbuf_fd = -1;
    return 0;
}

/* PROBE: what each front port holds, for a memory manager's status: port <n> smc <card id> | ps1 (a memory card that
 * isn't a Super MemCard one: a PS1 card, or one without its identity) | empty. Through Main, as card_open_bus. */
static void probe(int fd) {
    char who[120];
    if (mc_lock(10000, who, sizeof(who))) {
        say(fd, "busy %s\n", who);
        return;
    }
    struct conn c;
    memset(&c, 0, sizeof(c));
    c.front.fd = unix_connect(FRONT_SOCK);
    if (c.front.fd < 0) {
        say(fd, "noports\n");
        mc_unlock();
        return;
    }
    hold(&c, 1);
    c.bus.frame = front_frame;
    c.bus.bad_data = front_bad_data;
    c.bus.ctx = &c.front;
    for (int p = 1; p <= 2; p++) {
        c.front.port = p;
        int h = smc_hello(&c.bus, SMC_HOST_MAIN, 0, &c.card);
        if (h == 1 && genuine(&c)) {
            if (c.card.free_kb == 0xFFFFFFFFu) say(fd, "port %d smc %s ?\n", p, c.card.id);   /* (not counted yet: the card
                                                                                 counts it in its own menu, too slow for a frame) */
            else say(fd, "port %d smc %s %u\n", p, c.card.id, c.card.free_kb);
            continue;
        }
        if (h == 1) {
            say(fd, "port %d ps1\n", p);
            continue;
        }
        /* a PS1 card ACKs a read's first bytes (its ID); nothing in the port ACKs none */
        usleep(60000);
        uint8_t o[4] = {0x81, 0x52, 0x00, 0x00}, i[4];
        int n = front_raw(&c.front, o, i, 4, 0);
        say(fd, n >= 2 ? "port %d ps1\n" : "port %d empty\n", p);
        usleep(60000);
    }
    hold(&c, 0);
    close(c.front.fd);
    fbuf_fd = -1;
    mc_unlock();
}

/* the card's progress bar while this connection reads or writes files on it (a version 4 card): "Saving Zelda"... The bar
 * goes at card_close */
static char xfer_text[SMC_PROGRESS_TEXT + 1];
static void xfer_begin(struct conn *c, const char *what, const char *name) {
    if (c->card.version < SMC_PROTOCOL_FILES)
        return;
    char nm[SMC_GAME_NAME_MAX + 1];
    smc_display_name(name ? name : "", nm, sizeof(nm));
    snprintf(xfer_text, sizeof(xfer_text), "%s%s%s", what, nm[0] ? " " : "", nm);
    smc_xfer_text = xfer_text;
    smc_progress(&c->bus, 0, xfer_text);
}

static void card_close(struct conn *c) {
    if (raw_frames > 50)
        lg("bus: %lu frames, %.0f ms on the bus (%.2f ms each), %lu tried again after a miss", raw_frames, raw_ms,
           raw_ms / raw_frames, raw_retries);
    raw_frames = raw_retries = 0;
    raw_ms = 0;
    if (smc_xfer_text) {
        smc_progress(&c->bus, -1, NULL);   /* done: the card's own screen again */
        smc_xfer_text = NULL;
    }
    if (c->vcard)
        smc_vcard_close(&c->bus);
    else if (c->front.fd >= 0) {
        hold(c, 0);
        close(c->front.fd);
        fbuf_fd = -1;   /* (the next connection can get the same fd: nothing of this one kept) */
    }
    mc_unlock();
}

/* ---- the sync's setup ---- */
static struct smc_sync_cfg cfg(void) {
    struct smc_sync_cfg k = {saves, state, smc_console_systems, SMC_HOST_MAIN, lib_log, games, lists};
    return k;
}

/* the game being played: its system and save name (empty: none known) */
static char play_sys[65], play_game[256];
static time_t seen, synced;   /* its saves' newest time: last seen, last on the card */
static time_t retry_at;       /* a save that didn't reach the card: not tried again before this */
static int core_told;         /* the card was told this game's system (Core): its screen shows it */
static int no_card;           /* tries that found no card this game (a core without the front ports, the card not in
                               * yet, or the core still starting): asked again after a while, never given up on */
static time_t no_card_at;     /* when to try again */

/* a game's save files in the saves folder: the newest one's time, their bytes and count */
static void game_saves_stat(const char *sys, const char *game, time_t *newest_out, unsigned long *bytes, int *files) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", saves, sys);
    *newest_out = 0;
    *bytes = 0;
    *files = 0;
    DIR *d = opendir(dir);
    if (!d)
        return;
    size_t n = strlen(game);
    time_t newest = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *f = de->d_name;
        if (strncmp(f, game, n) || (f[n] != '.' && !(f[n] == '_' && f[n + 1] >= '1' && f[n + 1] <= '4' && f[n + 2] == '.')))
            continue;
        char p[1300];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, f);
        if (!stat(p, &st) && S_ISREG(st.st_mode)) {
            if (st.st_mtime > newest)
                newest = st.st_mtime;
            *bytes += (unsigned long)st.st_size;
            (*files)++;
        }
    }
    closedir(d);
    *newest_out = newest;
}

static time_t game_saves_time(const char *sys, const char *game) {
    time_t t;
    unsigned long b;
    int f;
    game_saves_stat(sys, game, &t, &b, &f);
    return t;
}

/* ---- a game's MGL, kept from its launch for the memory manager's Play: <state>/launch/<sys>/<save name>.mgl ---- */
static void launch_path(const char *sys, const char *game, char *out, size_t n) {
    snprintf(out, n, "%s/launch/%s/%s.mgl", state, sys, game);
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a)
        return -1;
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.new", to);
    FILE *b = fopen(tmp, "wb");
    if (!b) {
        fclose(a);
        return -1;
    }
    char t[4096];
    size_t r;
    while ((r = fread(t, 1, sizeof(t), a)) > 0)
        fwrite(t, 1, r, b);
    fclose(a);
    int bad = fclose(b);
    return bad || rename(tmp, to) ? -1 : 0;
}

static void keep_launch(const char *sys, const char *game, const char *mgl) {
    char p[1100], d[1100];
    snprintf(d, sizeof(d), "%s/launch", state);
    mkdir(d, 0755);
    snprintf(d, sizeof(d), "%s/launch/%s", state, sys);
    mkdir(d, 0755);
    launch_path(sys, game, p, sizeof(p));
    if (strcmp(p, mgl))
        copy_file(mgl, p);
}

/* the game file an MGL loads still there (an MGL kept from long ago may point at a ROM since moved) */
static bool mgl_file_there(const char *mgl) {
    FILE *f = fopen(mgl, "r");
    if (!f)
        return false;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strstr(buf, "path=\"");
    if (!p)
        return false;
    p += 6;
    char *e = strchr(p, '"');
    if (!e)
        return false;
    *e = 0;
    for (char *a; (a = strstr(p, "&amp;"));)   /* the one entity a ROM's name has */
        memmove(a + 1, a + 5, strlen(a + 5) + 1);
    if (p[0] != '/')
        return true;   /* relative to the core's games folder: Main finds it */
    return access(p, R_OK) == 0;
}

/* Main's command pipe (load_core <file>): the memory manager's Play starts a game the way Console Mode does */
static const char *mister_cmd(void) {
    const char *e = getenv("SMC_MISTER_CMD");
    return e && *e ? e : "/dev/MiSTer_cmd";
}

/* load: Main loads the game (PLAY); else (PLAYPREP, Coreshell's) only the card switch, the game's MGL in the answer:
 * Coreshell starts it, with its settings */
static void play(int fd, struct conn *c, const struct smc_sync_cfg *k, char *a, struct smc_sync_report *rep, int load) {
    char *t1 = strchr(a, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (!t1 || !t2) {
        say(fd, "play failed\n");
        return;
    }
    *t1 = *t2 = 0;
    const char *sys = a, *game = t2 + 1;
    int card = atoi(t1 + 1);
    if (!strcmp(sys, "PSX")) {   /* the PSX core has its card in the port itself */
        say(fd, "play psx\n");
        return;
    }
    if (card < 1 || card > 99 || !game[0]) {
        say(fd, "play failed\n");
        return;
    }
    char mgl[1100];
    launch_path(sys, game, mgl, sizeof(mgl));
    if (!mgl_file_there(mgl)) {
        static char made[4096];
        snprintf(mgl, sizeof(mgl), "/tmp/smc_play.mgl");
        FILE *f = NULL;
        if (smc_game_mgl(k, sys, game, made, sizeof(made)) || !(f = fopen(mgl, "w"))) {
            lg("%s: %s can't be started from here (no ROM found, or the system needs its MGL)", sys, game);
            say(fd, "play notfound\n");
            return;
        }
        fputs(made, f);
        fclose(f);
    }
    if (smc_use_card(&c->bus, k, sys, game, card, rep) != 0) {
        say(fd, "play failed\n");
        return;
    }
    if (!load) {
        lg("%s: %s's save on card %d picked, for Coreshell to start it (%s)", sys, game, card, mgl);
        say(fd, "play ok %d\t%s\n", card, mgl);
        return;
    }
    FILE *m = fopen(mister_cmd(), "w");
    if (!m || fprintf(m, "load_core %s\n", mgl) < 0) {
        if (m)
            fclose(m);
        lg("%s: card %d picked, but Main didn't take the game (%s)", sys, card, mister_cmd());
        say(fd, "play failed\n");
        return;
    }
    fclose(m);
    lg("%s: playing %s with the save on card %d (%s)", sys, game, card, mgl);
    say(fd, "play ok %d\n", card);
}

/* the card's screen for the game being played (non-PSX; card firmware 3 on): the system, the game, when its save was
 * made, whether the card has it, its size. on_card: the card has the save on the SD card now. */
static void show_screen(struct conn *c, const char *sys, const char *game, int on_card) {
    if (!sys || !sys[0] || !strcmp(sys, "PSX") || c->card.version < SMC_PROTOCOL_GAME)
        return;
    char name[SMC_GAME_NAME_MAX + 1], saved[17] = "", status[17], size[17] = "";
    smc_display_name(game, name, sizeof(name));
    time_t t = 0;
    unsigned long bytes = 0;
    int files = 0;
    if (game && game[0])
        game_saves_stat(sys, game, &t, &bytes, &files);
    if (files) {
        struct tm now, at;
        time_t n = time(NULL);
        localtime_r(&n, &now);
        localtime_r(&t, &at);
        (void)now;
        strftime(saved, sizeof(saved), "%m/%d/%y %H:%M", &at);   /* the save's date and time, 24 h: the card shows it
                                                                     * in its own clock setting (12 h: "11/11/26 11:17PM") */
        snprintf(status, sizeof(status), "%s", on_card ? "On SSMC" : "New save waiting");
        unsigned long kb = (bytes + 1023) / 1024;
        snprintf(size, sizeof(size), "%lu KB, %d file%s", kb ? kb : 1, files, files == 1 ? "" : "s");
    } else
        snprintf(status, sizeof(status), "%s", game && game[0] ? "No save yet" : "");
    const char *lines[SMC_SCREEN_LINES] = {smc_system_title(sys), name, saved, status, size};
    int rc = smc_core_screen(&c->bus, sys, lines);
    if (rc < 0)
        lg("%s: the card didn't take its screen (%d)", sys, rc);
}

/* ---- the jobs ---- */
/* the game that was running, before another starts without the menu between (another core or ROM picked in the OSD):
 * its newest save to the card, if the card can be reached */
static void leave_game(void) {
    if (!play_game[0] || !strcmp(play_sys, "PSX"))
        return;
    struct conn c;
    if (!card_open(&c))
        return;
    xfer_begin(&c, "Saving", play_game);
    struct smc_sync_cfg k = cfg();
    struct smc_sync_report rep;
    memset(&rep, 0, sizeof(rep));
    if (smc_sync_game(&c.bus, &k, play_sys, play_game, &rep) < 0)
        lg("%s/%s: the save before the next game didn't reach the card", play_sys, play_game);
    card_close(&c);
}

static void launch_game(const char *sys, const char *game, const char *mgl);

static void do_launch(char *args) {
    smc_sync_hold_card(false);   /* before the core loads: the card picked now is the one it starts with */
    char *tab = strchr(args, '\t');
    const char *mgl = "";
    if (tab) {
        *tab = 0;
        mgl = tab + 1;
    }
    struct smc_mgl m;
    if (!mgl[0] || smc_mgl_read(mgl, &m))
        memset(&m, 0, sizeof(m));
    const char *sys = smc_launch_system(args, m.setname, m.same_dir);
    leave_game();
    play_sys[0] = play_game[0] = 0;
    if (!sys)
        return;
    launch_game(sys, m.game, mgl);
}

/* ROM <rbf>\t<file>\t<driver 0|1>: a game file picked in the running core's OSD. "same": it's the game running already;
 * "bounce": a core without the card driver, so the card can't be reached now: Main goes through the menu core and
 * starts the core again with this game (an MGL, LAUNCH as any other); else the old game's save to the card, the new
 * game's from it, as LAUNCH */
static const char *do_rom(char *args) {
    char *t1 = strchr(args, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (!t1)
        return "DONE\n";
    *t1 = 0;
    if (t2)
        *t2 = 0;
    const char *file = t1 + 1;
    int driver = t2 ? atoi(t2 + 1) : 0;
    char game[256];
    const char *base = strrchr(file, '/');
    snprintf(game, sizeof(game), "%s", base ? base + 1 : file);
    char *dot = strrchr(game, '.');
    if (dot && dot != game)
        *dot = 0;
    const char *sys = play_sys[0] ? play_sys : smc_launch_system(args, "", false);
    if (!sys || !strcmp(sys, "PSX"))
        return "DONE\n";   /* not a system with its saves on the card (PSX: its own card) */
    char sysbuf[65];
    snprintf(sysbuf, sizeof(sysbuf), "%s", sys);
    if (!strcmp(sysbuf, "Saturn"))
        smc_saturn_save_name(game);
    if (!strcmp(sysbuf, play_sys) && !strcmp(game, play_game))
        return "DONE same\n";
    if (!driver)
        return "DONE bounce\n";
    smc_sync_hold_card(false);
    leave_game();
    play_sys[0] = play_game[0] = 0;
    launch_game(sysbuf, game, "");
    return play_game[0] ? "DONE saves\n" : "DONE\n";
}

static void launch_game(const char *sys, const char *game, const char *mgl) {
    snprintf(play_sys, sizeof(play_sys), "%s", sys);
    snprintf(play_game, sizeof(play_game), "%s", game);
    if (!strcmp(sys, "Saturn"))
        smc_saturn_save_name(play_game);   /* Main's name for its save: every disc of the game shares it */
    if (play_game[0] && strchr(play_game, '/') == NULL && play_game[0] != '.')
        keep_launch(sys, play_game, mgl);   /* for the memory manager's Play: by the save's name */
    no_card = 0;
    no_card_at = 0;
    struct conn c;
    if (!card_open(&c)) {
        lg("%s: no card reachable (the save is checked again while the game runs)", sys);
        seen = synced = 0;   /* nothing of this game on the card yet: the game poll sends its save */
        core_told = 0;       /* and tells the card the system when it first reaches it */
        return;
    }
    int card = smc_core(&c.bus, sys, true);
    core_told = card > 0;
    lg("%s core: %s %d", sys, card > 0 ? "its card" : "the card didn't answer Core", card);
    int sync_ok = 1;
    if (card > 0 && play_game[0]) {
        struct smc_sync_cfg k = cfg();
        struct smc_sync_report rep;
        xfer_begin(&c, "Loading", play_game);
        int rc = smc_sync_game(&c.bus, &k, sys, play_game, &rep);
        if (rc < 0)
            lg("game sync stopped with an error");
        sync_ok = rc >= 0 && !rep.failed;
    }
    if (card > 0)
        show_screen(&c, sys, play_game, sync_ok);   /* after the sync: the save's state as the game starts */
    card_close(&c);
    seen = play_game[0] ? game_saves_time(play_sys, play_game) : 0;
    synced = sync_ok ? seen : 0;   /* a save that didn't reach the card stays to send (the game poll tries again) */
    smc_sync_hold_card(true);    /* the game has this card's save in memory from now on */
}

static void do_menu(void) {
    smc_sync_hold_card(false);   /* the game is over: a card picked meanwhile is used from now on */
    char sys[65], game[256];
    snprintf(sys, sizeof(sys), "%s", play_sys);
    snprintf(game, sizeof(game), "%s", play_game);
    play_sys[0] = play_game[0] = 0;
    if (!sys[0])
        return;   /* no game was played: the card isn't touched */
    struct conn c;
    if (!card_open(&c))
        return;
    xfer_begin(&c, "Saving", game);
    struct smc_sync_cfg k = cfg();
    struct smc_sync_report rep;
    memset(&rep, 0, sizeof(rep));
    int rc = game[0] ? smc_sync_game(&c.bus, &k, sys, game, &rep) : smc_sync_changed(&c.bus, &k, sys, &rep);
    if (rc < 0)
        lg("sync stopped with an error");
    smc_core(&c.bus, NULL, true);   /* no game core now: the card's buttons are the PS1 card's again */
    card_close(&c);
}

/* while a game runs: its save to the card once it changed and settled */
static void game_poll(void) {
    if (!play_game[0] || !strcmp(play_sys, "PSX") || (no_card && time(NULL) < no_card_at))   /* PSX: its own card */
        return;
    time_t t = game_saves_time(play_sys, play_game);
    if (t != seen) {
        seen = t;
        return;
    }
    int need_sync = t && t != synced && time(NULL) >= retry_at, need_core = !core_told && time(NULL) >= retry_at;
    if (!need_sync && !need_core)
        return;
    struct conn c;
    if (!card_open(&c)) {
        if (!no_card++)
            lg("in-game: no card reachable from this core yet (asked again every 10 s, 30 s after a minute)");
        no_card_at = time(NULL) + (no_card < 6 ? 10 : 30);
        return;
    }
    if (no_card)
        lg("in-game: the card reached after %d tr%s", no_card, no_card == 1 ? "y" : "ies");
    no_card = 0;
    if (need_core) {   /* the card wasn't reachable at the game's start (a menu core without the front ports) */
        int card = smc_core(&c.bus, play_sys, true);
        core_told = card > 0;
        lg("%s core (in-game): %s %d", play_sys, card > 0 ? "its card" : "the card didn't answer Core", card);
        if (!core_told)
            retry_at = time(NULL) + 30;
        else if (!need_sync)
            show_screen(&c, play_sys, play_game, !t || t == synced);
    }
    if (!need_sync) {
        card_close(&c);
        return;
    }
    struct smc_sync_cfg k = cfg();
    struct smc_sync_report rep;
    xfer_begin(&c, "Saving", play_game);
    int rc = smc_push_game(&c.bus, &k, play_sys, play_game, &rep);   /* push only: the game has its save open */
    if (rc < 0)
        lg("in-game sync stopped with an error");
    if (rc < 0 || rep.failed) {
        retry_at = time(NULL) + 30;   /* the save didn't reach the card: again in 30 s, not every pass */
        lg("in-game: the save didn't reach the card, again in 30 s");
    } else
        synced = t;
    show_screen(&c, play_sys, play_game, rc >= 0 && !rep.failed);   /* the new save on the card (our cores) */
    card_close(&c);
}

static void report(int fd, const char *what, int rc, const struct smc_sync_report *r) {
    say(fd, "%s %s card=%s pushed=%d pulled=%d unchanged=%d conflicts=%d skipped=%d failed=%d deleted=%d\n", what,
        rc < 0 ? "error" : "ok", r->card_id, r->pushed, r->pulled, r->unchanged, r->conflicts, r->skipped, r->failed,
        r->deleted);
}

static void conflict_cb(const char *sys, int card, const char *save, uint32_t gen, void *ctx) {
    say(*(int *)ctx, "%s\t%d\t%s\t%u\n", sys, card, save, gen);
}

/* SECTION: the other systems' saves as one section of the memory manager's screen, in a fixed order, so the frontend
 * can put it anywhere (below the PS1 card's saves, on a tab of its own) without asking system by system. Any number
 * of games:
 *   system  \t <folder> \t <title> \t <its current virtual card> \t <games>       then that system's games:
 *   game    \t <folder> \t <name> \t <id> \t <time> \t <files> \t <bytes> \t <icon> \t <save name> \t <card folder> \t <both changed>
 *   section \t <games> \t <systems>                                             last, the totals
 * PSX (its card images) only with SECTION PSX: the PS1 card's saves are the frontend's own section. */
struct conflict_set { char keys[64][200]; int n; };
static void conflict_add(const char *sys, int card, const char *save, uint32_t gen, void *ctx) {
    (void)gen;
    struct conflict_set *cs = ctx;
    if (cs->n < 64)
        snprintf(cs->keys[cs->n++], sizeof(cs->keys[0]), "%s\t%d\t%s", sys, card, save);
}
/* a game's icon as it is on the card (icon.bin, SMIC), kept in <state>/card_icons/<sys>/<id>.bin for frontends to show:
 * a game this SuperStation hasn't synced has no icon in the icon database (that's for icons made here, and a copy of
 * the card's there would stop the real one being looked for), but the card has one. Once per game: kept until it's
 * gone from the card's list. */
static void card_icon(struct conn *c, const char *sys, int card, const char *id) {
    char base[65], dir[1100], path[1300];
    snprintf(base, sizeof(base), "%s", id);
    if (strchr(base, '~'))
        *strchr(base, '~') = 0;   /* a second save set shows the game's icon */
    if (strchr(base, '/'))
        return;
    snprintf(dir, sizeof(dir), "%s/card_icons/%s", state, sys);
    snprintf(path, sizeof(path), "%s/%s.bin", dir, base);
    if (access(path, F_OK) == 0)
        return;
    int n = smc_select_card(&c->bus, sys, id, card, NULL);
    struct smc_file fi;
    for (int i = 0; i < n; i++) {
        if (smc_info(&c->bus, i, &fi) != SMC_OK)
            return;
        if (fi.deleted || strcmp(fi.name, "icon.bin"))
            continue;
        static uint8_t icon[2048];
        if (fi.size < 8 || fi.size > sizeof(icon) || smc_read_file(&c->bus, i, fi.size, icon) != SMC_OK || memcmp(icon, "SMIC", 4))
            return;
        char d1[1100];
        snprintf(d1, sizeof(d1), "%s/card_icons", state);
        mkdir(d1, 0755);
        mkdir(dir, 0755);
        char tmp[1310];
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (!f)
            return;
        int ok = fwrite(icon, 1, fi.size, f) == fi.size;
        ok &= fclose(f) == 0;
        if (ok)
            rename(tmp, path);
        else
            unlink(tmp);
        return;
    }
}

/* A list or a game's details off the card, tried again when a frame went missing (the BFMC misses one now and then):
 * only "no more" (NOT_FOUND) or a refusal (BAD) ends it at once. Before this a missed frame read as the end of the list,
 * and a system's games after it, or one game, silently weren't there. read_errs counts the reads that still failed, so
 * the answer can say it isn't whole ("partial") instead of looking like a card with fewer saves. */
static int read_errs;
static int list_card_r(const struct smc_bus *b, const char *sys, int card, int idx, char id[65]) {
    int rc = SMC_E_NO_REPLY;
    for (int t = 0; t < 4; t++) {
        rc = smc_list_card(b, sys, card, idx, id);
        if (rc == SMC_OK || rc == SMC_E_NOT_FOUND || rc == SMC_E_BAD)
            return rc;
        usleep(60000);
    }
    read_errs++;
    return rc;
}
static int game_info_r(const struct smc_bus *b, const char *sys, int card, const char *id, struct smc_game_info *gi) {
    int rc = SMC_E_NO_REPLY;
    for (int t = 0; t < 4; t++) {
        rc = smc_game_info(b, sys, card, id, gi);
        if (rc == SMC_OK || rc == SMC_E_NOT_FOUND || rc == SMC_E_BAD)
            return rc;
        usleep(60000);
    }
    read_errs++;
    return rc;
}

static void section(int fd, struct conn *c, const struct smc_sync_cfg *k, int with_psx) {
    static struct conflict_set cs;
    read_errs = 0;
    cs.n = 0;
    smc_conflicts(k, c->card.id, conflict_add, &cs);
    int games = 0, systems = 0;
    for (int s = 0; smc_console_systems[s]; s++) {
        const char *sys = smc_console_systems[s];
        if (!with_psx && !strcmp(sys, "PSX"))
            continue;
        int card = smc_core(&c->bus, sys, false);
        if (card < 1)
            card = 1;
        /* the system's games first (its line carries their count), however many there are */
        char *lines = NULL, id[65];
        size_t ll = 0, cap = 0;
        int n = 0;
        const char *st = store_of(sys);   /* (PSX: each game's own PS1 card) */
        for (int i = 0; list_card_r(&c->bus, st, card, i, id) == SMC_OK; i++) {
            struct smc_game_info gi;
            if (st != sys && !ps1_game_folder(id))
                continue;   /* (PSX: not PS1 mode's own cards) */
            if (st != sys && ps1card_drop_empty(c, id) == 1) {
                i--;   /* a game's card with no save: deleted (the next one is at this place now) */
                continue;
            }
            if (game_info_r(&c->bus, st, card, id, &gi) != SMC_OK)
                continue;
            if (!gi.files && !gi.has_icon && !gi.name[0])
                continue;   /* a deleted game (DELGAME, CLEAR): nothing of it left */
            if (gi.has_icon)
                card_icon(c, st, card, id);
            char key[200], line[1024];
            snprintf(key, sizeof(key), "%s\t%d\t%s", sys, card, gi.save_name);
            int both = 0;
            for (int j = 0; j < cs.n; j++)
                both |= !strcmp(cs.keys[j], key);
            int w = snprintf(line, sizeof(line), "game\t%s\t%s\t%s\t%u\t%d\t%u\t%d\t%s\t%s\t%d\t%s\n", sys,
                             gi.name[0] ? gi.name : gi.title[0] ? gi.title : gi.save_name, gi.id, gi.time, gi.files,
                             gi.bytes, gi.has_icon ? 1 : 0, gi.save_name, id, both, gi.label);
            if (w <= 0 || w >= (int)sizeof(line))
                continue;
            if (ll + (size_t)w > cap) {
                size_t nc = cap ? cap * 2 : 16384;
                while (nc < ll + (size_t)w)
                    nc *= 2;
                char *nl = realloc(lines, nc);
                if (!nl)
                    break;
                lines = nl;
                cap = nc;
            }
            memcpy(lines + ll, line, (size_t)w);
            ll += (size_t)w;
            n++;
        }
        if (n) {
            say(fd, "system\t%s\t%s\t%d\t%d\n", sys, smc_system_title(sys), card, n);
            send_all(fd, lines, ll);
            games += n;
            systems++;
        }
        free(lines);
    }
    if (read_errs)
        say(fd, "partial\t%d\n", read_errs);
    say(fd, "section\t%d\t%d\n", games, systems);
}

struct cards_list {
    int fd;
    bool used[100];
};
static void cards_cb(int card, int games, uint32_t newest, bool current, void *ctx) {
    struct cards_list *cl = ctx;
    if (card >= 1 && card <= 99 && (games || current))   /* the blank card offered is never the one in use */
        cl->used[card] = true;
    say(cl->fd, "card\t%d\t%d\t%u\t%d\n", card, games, newest, current ? 1 : 0);
}

struct saves_list {
    int fd;
    bool used[100];
};
static void saves_cb(int card, int files, uint32_t time, bool playing, const char *label, void *ctx) {
    struct saves_list *sl = ctx;
    if (card >= 1 && card <= 99)
        sl->used[card] = true;
    say(sl->fd, "save\t%d\t%d\t%u\t%d\t%s\n", card, files, time, playing ? 1 : 0, label ? label : "");
}

/* ---- moving saves around (the memory manager): a game's files on a virtual card to a folder and back ----
 * A folder per game (its ID), holding the game's files as they are on the card (its save files, name.txt, icon.bin,
 * game.ini, label.txt) and times.txt (each file's time on the card). EXPORT writes them, IMPORT writes them onto a card
 * (a new generation of each file, so other SuperStations take them at their next sync), DELGAME deletes one game's
 * files on one card (tombstones, as DELETE). Copy, move, back up and restore are made of these. */
static int mkdir_p(const char *d) {
    char t[1100];
    snprintf(t, sizeof(t), "%s", d);
    for (char *p = t + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(t, 0755);
            *p = '/';
        }
    return mkdir(t, 0755) == 0 || errno == EEXIST ? 0 : -1;
}
static int safe_part(const char *s) { return s[0] && s[0] != '.' && !strchr(s, '/') && !strstr(s, ".."); }

/* one game: its files into <dir>/<id>/. The files written, or <0 */
static int export_game(struct conn *c, const char *sys, int card, const char *id, const char *dir) {
    if (!safe_part(id))
        return -1;
    int n = smc_select_card(&c->bus, sys, id, card, NULL);
    if (n < 0)
        return n;
    char gd[1200], path[1300];
    snprintf(gd, sizeof(gd), "%s/%s", dir, id);
    if (mkdir_p(gd))
        return -1;
    snprintf(path, sizeof(path), "%s/times.txt", gd);
    FILE *tf = fopen(path, "w");
    if (!tf)
        return -1;
    int done = 0;
    for (int i = 0; i < n; i++) {
        struct smc_file fi;
        if (smc_info(&c->bus, i, &fi) != SMC_OK) {
            fclose(tf);
            return -1;
        }
        if (fi.deleted || !safe_part(fi.name) || !strcmp(fi.name, "times.txt"))
            continue;
        uint8_t *buf = malloc(fi.size ? fi.size : 1);
        if (!buf || smc_read_file(&c->bus, i, fi.size, buf) != SMC_OK) {
            free(buf);
            fclose(tf);
            return -1;
        }
        snprintf(path, sizeof(path), "%s/%s", gd, fi.name);
        FILE *f = fopen(path, "wb");
        int ok = f && fwrite(buf, 1, fi.size, f) == fi.size;
        if (f)
            ok &= fclose(f) == 0;
        free(buf);
        if (!ok) {
            fclose(tf);
            return -1;
        }
        fprintf(tf, "%s\t%u\n", fi.name, fi.time);
        done++;
    }
    fclose(tf);
    return done ? done : -1;   /* (nothing of that game on that card) */
}

/* ---- PSX: each game's own PS1 card is the store (card version 4: PS1CARD) ---- */

static int read_whole(const char *path, uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t got = fread(buf, 1, n, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return more ? -1 : (int)got;
}
static int write_whole(const char *path, const uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(buf, 1, n, f) == n;
    ok &= fclose(f) == 0;
    return ok ? 0 : -1;
}

static int export_game(struct conn *c, const char *sys, int card, const char *id, const char *dir);
static int import_game(struct conn *c, const char *sys, int card, const char *dir, const char *id);
static int delete_game(struct conn *c, const char *sys, int card, const char *id);

/* PS1 mode's default card back on the bus (it lets go of a game's own card it had open): 0 */
static int psx_free;   /* EXPORT/IMPORT PSX with no PSX game running: a busy game card may be let go */
static int psx_let_go(struct conn *c) {
    smc_ps1_default_card(&c->bus);
    usleep(500000);
    return 0;
}

/* a game's own PS1 card (MemoryCards/PS1/<id>/<id>-1.mcd) into img: 1 read, 0 there's none yet, <0 failed. The card
 * PS1 mode has open can't be read (its copy in memory is the newer one): the card goes back to its default card first */

static int ps1card_read(struct conn *c, const char *id, const char *tmp, uint8_t img[MC_SIZE]) {
    char path[1300];
    for (int tries = 0; tries < 2; tries++) {
        int n = smc_select_card(&c->bus, SMC_PS1CARD, id, 1, NULL);
        if (n == 0)
            return 0;   /* no card for that game yet */
        if (n < 0)
            return n;
        int r = export_game(c, SMC_PS1CARD, 1, id, tmp);
        snprintf(path, sizeof(path), "%s/%s/%s-1.mcd", tmp, id, id);
        if (r > 0 && read_whole(path, img, MC_SIZE) == MC_SIZE)
            return 1;
        if (r > 0)
            return 0;   /* (other files there, no channel 1 card) */
        smc_ps1_default_card(&c->bus);   /* busy: PS1 mode has it open */
        usleep(500000);
    }
    return -1;
}

/* a game's own PS1 card, from smcd's copy (<state>/ps1cards/<id>-1.mcd and .crc) while the card's CRC of it is the
 * same, else read and kept: 1, or <0 (no card for that game: <0 too) */
static int ps1card_cached(struct conn *c, const char *id, uint8_t img[MC_SIZE]) {
    if (!safe_part(id))
        return -1;
    char kd[1200], kp[1300], kc[1300], line[32] = "";
    snprintf(kd, sizeof(kd), "%s/ps1cards", state);
    snprintf(kp, sizeof(kp), "%s/%s-1.mcd", kd, id);
    snprintf(kc, sizeof(kc), "%s/%s-1.crc", kd, id);
    int n = smc_select_card(&c->bus, SMC_PS1CARD, id, 1, NULL);
    uint32_t crc = 0;
    int have = 0;
    for (int i = 0; i < n; i++) {
        struct smc_file fi;
        char want[40];
        snprintf(want, sizeof(want), "%s-1.mcd", id);
        if (smc_info(&c->bus, i, &fi) == SMC_OK && !fi.deleted && !strcasecmp(fi.name, want)) {
            crc = fi.gen;
            have = 1;
        }
    }
    if (!have)
        return -1;
    FILE *f = fopen(kc, "r");
    if (f) {
        if (!fgets(line, sizeof(line), f))
            line[0] = 0;
        fclose(f);
    }
    if (line[0] && strtoul(line, NULL, 16) == crc && read_whole(kp, img, MC_SIZE) == MC_SIZE)
        return 1;   /* unchanged since smcd read it */
    if (ps1card_read(c, id, "/tmp/smc_ps1card", img) != 1)
        return -1;
    if (!mkdir_p(kd) && !write_whole(kp, img, MC_SIZE) && (f = fopen(kc, "w"))) {
        fprintf(f, "%08x\n", crc);
        fclose(f);
    }
    return 1;
}

/* smcd's copy of a game's own PS1 card dropped (the card itself deleted) */
static void ps1card_forget(const char *id) {
    if (!safe_part(id))
        return;
    char kp[1300];
    snprintf(kp, sizeof(kp), "%s/ps1cards/%s-1.mcd", state, id);
    unlink(kp);
    snprintf(kp, sizeof(kp), "%s/ps1cards/%s-1.crc", state, id);
    unlink(kp);
}

/* a game's own PS1 card with no save on it (PS1 mode makes a blank one the first time a game is started, saves or
 * not): deleted, so the memory manager doesn't list it. 1 deleted, 0 kept (it has saves), <0 couldn't tell */
static int ps1card_drop_empty(struct conn *c, const char *id) {
    static uint8_t img[MC_SIZE];
    if (ps1card_cached(c, id, img) != 1)
        return -1;
    mc_card_t k;
    if (mc_formatted(img) && (mc_parse(img, &k), k.nsaves > 0))
        return 0;
    for (size_t i = 0; i < MC_SIZE && !mc_formatted(img); i++)
        if (img[i] != 0xFF && img[i] != 0x00)
            return 0;   /* not formatted, and not blank either: something's there, kept */
    char want[40];
    snprintf(want, sizeof(want), "%s-1.mcd", id);
    for (int tries = 0; tries < 2; tries++) {
        int n = smc_select_card(&c->bus, SMC_PS1CARD, id, 1, NULL);
        for (int i = 0; i < n; i++) {
            struct smc_file fi;
            if (smc_info(&c->bus, i, &fi) == SMC_OK && !fi.deleted && !strcasecmp(fi.name, want) &&
                smc_delete(&c->bus, i, fi.gen + 1, (uint32_t)time(NULL)) == SMC_OK) {
                lg("PSX: %s's card had no saves: deleted", id);
                ps1card_forget(id);
                return 1;
            }
        }
        smc_ps1_default_card(&c->bus);   /* (PS1 mode had it open: the default card, then once more) */
        usleep(500000);
    }
    return -1;
}

/* PSXMERGE <backup dir>: Super MemCard's own PSX copies (SuperMemCard/PSX/Card1/<id>/) into each game's own PS1 card,
 * which is the PSX store from now on (what the game reads is what the memory manager shows). Every copy goes to
 * <backup dir> first (EXPORT's layout: the memory manager's library backup). Then, game by game: the saves the game's
 * card doesn't have are added to it (where both have a save of the same name, the game's card's is kept: it's what the
 * game last read); the card written back and read again to check; only then is the copy taken off the card.
 * Answers a line a game: "merge <id> added <n> kept <n> noroom <n> <ok|failed>", then psxmerge ok <games> | failed */
static void psx_merge(int fd, struct conn *c, const char *dir) {
    char id[65];
    int games = 0, bad = 0;
    if (c->card.version < SMC_PROTOCOL_FILES) {
        say(fd, "psxmerge failed (card firmware %u: version 4 needed)\n", c->card.version);
        return;
    }
    if (dir[0] != '/' || strstr(dir, "..") || mkdir_p(dir)) {
        say(fd, "psxmerge failed\n");
        return;
    }
    /* the copies, all of them, before anything changes */
    char ids[64][65];
    int nid = 0;
    read_errs = 0;
    for (int i = 0; nid < 64 && list_card_r(&c->bus, "PSX", 1, i, id) == SMC_OK; i++) {
        struct smc_game_info gi;
        if (game_info_r(&c->bus, "PSX", 1, id, &gi) != SMC_OK || !gi.files)
            continue;
        if (export_game(c, "PSX", 1, id, dir) < 0) {
            lg("PSX merge: %s couldn't be backed up: nothing changed", id);
            say(fd, "psxmerge failed (backup of %s)\n", id);
            return;
        }
        snprintf(ids[nid++], 65, "%s", id);
    }
    if (read_errs) {
        say(fd, "psxmerge failed (the card's list)\n");
        return;
    }
    const char *tmp = "/tmp/smc_psxmerge/read";   /* (not in the backup: the library would list it) */
    static uint8_t smc_img[MC_SIZE], card_img[MC_SIZE], merged[MC_SIZE], back[MC_SIZE], mcs[128 + 15 * MC_BLOCK];
    for (int g = 0; g < nid; g++) {
        const char *gid = ids[g];
        /* the copy: its biggest file (the 128 KB card image) */
        char gd[1300], path[1400];
        snprintf(gd, sizeof(gd), "%s/%s", dir, gid);
        DIR *d = opendir(gd);
        struct dirent *e;
        int have = 0;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.' && strcmp(e->d_name, "times.txt")) {
                snprintf(path, sizeof(path), "%s/%s", gd, e->d_name);
                if (read_whole(path, smc_img, MC_SIZE) == MC_SIZE && mc_formatted(smc_img))
                    have = 1;
                if (have)
                    break;
            }
        if (d)
            closedir(d);
        if (!have) {
            say(fd, "merge %s: no card image in the copy, left as it is\n", gid);
            continue;
        }
        int r = ps1card_read(c, gid, tmp, card_img);
        if (r < 0) {
            say(fd, "merge %s: its own card couldn't be read, left as it is\n", gid);
            bad = 1;
            continue;
        }
        if (r == 1) {   /* the game's card as it was, kept in smcd's state (<state>/psxmerge/<id>-1.mcd) before it changes */
            char kd[1200], kp[1300];
            snprintf(kd, sizeof(kd), "%s/psxmerge", state);
            snprintf(kp, sizeof(kp), "%s/%s-1.mcd", kd, gid);
            if (mkdir_p(kd) || write_whole(kp, card_img, MC_SIZE)) {
                say(fd, "merge %s: its card couldn't be kept first, left as it is\n", gid);
                bad = 1;
                continue;
            }
        }
        if (r == 0 || !mc_formatted(card_img))
            mc_blank(card_img);
        memcpy(merged, card_img, MC_SIZE);
        mc_card_t sc, cc;
        mc_parse(smc_img, &sc);
        mc_parse(card_img, &cc);
        int added = 0, kept = 0, noroom = 0;
        for (int i = 0; i < sc.nsaves; i++) {
            if (sc.saves[i].broken)
                continue;
            int same = 0;
            for (int j = 0; j < cc.nsaves; j++)
                same |= !strcmp(cc.saves[j].name, sc.saves[i].name);
            if (same) {
                kept++;
                continue;
            }
            size_t n = 0;
            const char *err = NULL;
            if (mc_export(smc_img, sc.saves[i].slot, mcs, &n, &err) || mc_import(merged, mcs, n, &err)) {
                lg("PSX merge: %s %s: %s", gid, sc.saves[i].name, err ? err : "?");
                noroom++;
                continue;
            }
            added++;
        }
        int ok = 1;
        if (added) {
            const char *wdir = "/tmp/smc_psxmerge/write";
            snprintf(gd, sizeof(gd), "%s/%s", wdir, gid);
            snprintf(path, sizeof(path), "%s/%s-1.mcd", gd, gid);
            ok = !mkdir_p(gd) && !write_whole(path, merged, MC_SIZE) && import_game(c, SMC_PS1CARD, 1, wdir, gid) > 0;
            /* read back: exactly what was written */
            ok = ok && ps1card_read(c, gid, tmp, back) == 1 && !memcmp(back, merged, MC_SIZE);
        }
        /* the copy goes only when every save of it is on the game's card */
        if (ok && !noroom)
            ok = delete_game(c, "PSX", 1, gid) >= 0;
        lg("PSX merge: %s: %d added, %d kept (the game's card had them), %d without room: %s", gid, added, kept, noroom,
           ok ? (noroom ? "the copy kept" : "the copy taken off") : "failed");
        say(fd, "merge %s added %d kept %d noroom %d %s\n", gid, added, kept, noroom, ok ? "ok" : "failed");
        bad |= !ok;
        games++;
    }
    say(fd, bad ? "psxmerge failed\n" : "psxmerge ok %d\n", games);
}

/* <dir>/<id>/ onto a virtual card. The files written, or <0 */
static int import_game(struct conn *c, const char *sys, int card, const char *dir, const char *id) {
    if (!safe_part(id))
        return -1;
    char gd[1200], path[1300];
    snprintf(gd, sizeof(gd), "%s/%s", dir, id);
    DIR *d = opendir(gd);
    if (!d)
        return -1;
    int n = smc_select_card(&c->bus, sys, id, card, NULL);
    if (n < 0) {
        closedir(d);
        return n;
    }
    struct { char name[17]; uint32_t gen; } have[20];
    int nh = 0;
    for (int i = 0; i < n && nh < 20; i++) {
        struct smc_file fi;
        if (smc_info(&c->bus, i, &fi) == SMC_OK) {
            snprintf(have[nh].name, sizeof(have[nh].name), "%s", fi.name);
            have[nh++].gen = fi.gen;
        }
    }
    int done = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "times.txt") || strlen(e->d_name) > 16)
            continue;
        snprintf(path, sizeof(path), "%s/%s", gd, e->d_name);
        struct stat st;
        if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size > 2 * 1024 * 1024)
            continue;
        uint8_t *buf = malloc(st.st_size ? (size_t)st.st_size : 1);
        FILE *f = fopen(path, "rb");
        int ok = buf && f && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
        if (f)
            fclose(f);
        uint32_t t = (uint32_t)st.st_mtime, gen = 1;
        char tp[1300], line[200];
        snprintf(tp, sizeof(tp), "%s/times.txt", gd);
        FILE *tf = fopen(tp, "r");
        while (tf && fgets(line, sizeof(line), tf)) {
            char *tab = strchr(line, '\t');
            if (tab && (size_t)(tab - line) == strlen(e->d_name) && !strncmp(line, e->d_name, (size_t)(tab - line)))
                t = (uint32_t)strtoul(tab + 1, NULL, 10);
        }
        if (tf)
            fclose(tf);
        for (int i = 0; i < nh; i++)
            if (!strcmp(have[i].name, e->d_name))
                gen = have[i].gen + 1;
        if (ok && smc_select_card(&c->bus, sys, id, card, NULL) >= 0 &&
            smc_write_file_at(&c->bus, e->d_name, buf, (uint32_t)st.st_size, gen, t) == SMC_OK)
            done++;
        else
            done = -1000;
        free(buf);
    }
    closedir(d);
    return done < 0 ? -1 : done;
}

/* one game's files off one virtual card (tombstones). The files deleted, or <0 */
static int delete_game(struct conn *c, const char *sys, int card, const char *id) {
    int n = smc_select_card(&c->bus, sys, id, card, NULL), done = 0;
    if (n < 0)
        return n;
    for (int i = 0; i < n; i++) {
        struct smc_file fi;
        if (smc_info(&c->bus, i, &fi) != SMC_OK)
            return -1;
        if (fi.deleted)
            continue;
        if (smc_delete(&c->bus, i, fi.gen + 1, (uint32_t)time(NULL)) != SMC_OK)
            return -1;
        done++;
    }
    return done;
}

/* "<sys>\t<card>\t<id or *>\t<dir>" and the like: split on tabs */
static int split_tabs(char *a, char **f, int max) {
    int n = 0;
    while (n < max) {
        f[n++] = a;
        char *t = strchr(a, '\t');
        if (!t)
            break;
        *t = 0;
        a = t + 1;
    }
    return n;
}

/* a frontend's command; the answer ends with END */
static void do_command(int fd, char *l) {
    struct conn c;
    not_genuine_id[0] = 0;
    if (!card_open(&c)) {
        if (not_genuine_id[0])
            say(fd, "notgenuine %s %s\nEND\n", not_genuine_id, not_genuine_why);
        else
            say(fd, "nocard\nEND\n");
        return;
    }
    struct smc_sync_cfg k = cfg();
    struct smc_sync_report rep;
    memset(&rep, 0, sizeof(rep));
    char *a = strchr(l, ' ');
    a = a ? a + 1 : l + strlen(l);
    /* the card's progress bar for the commands that move saves */
    static const struct { const char *verb, *what; } bars[] = {
        {"READ", "Reading card"}, {"EXPORT ", "Copying"}, {"IMPORT ", "Writing"}, {"IMAGE ", "Reading"},
        {"PLAY ", "Loading"}, {"PLAYPREP ", "Loading"}, {"USE ", "Loading"}, {"DELGAME ", "Deleting"},
        {"CLEAR ", "Clearing card"}, {"DELETE ", "Deleting"}, {"PSXMERGE ", "Moving PSX saves"}};
    for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); i++)
        if (!strncmp(l, bars[i].verb, strlen(bars[i].verb)) && (bars[i].verb[strlen(bars[i].verb) - 1] == ' ' || !l[strlen(bars[i].verb)])) {
            /* the game, when the command names one: its last field (a save, an ID...) */
            const char *nm = NULL;
            if (strcmp(bars[i].verb, "READ") && strcmp(bars[i].verb, "CLEAR ") && strcmp(bars[i].verb, "PSXMERGE ")) {
                nm = strrchr(a, '\t');
                nm = nm ? nm + 1 : a;
                if (!strcmp(bars[i].verb, "EXPORT ") || !strcmp(bars[i].verb, "IMPORT ")) {
                    /* EXPORT/IMPORT <sys>\t<card>\t<id>\t<dir>: the id */
                    char *f2 = strchr(a, '\t'), *f3 = f2 ? strchr(f2 + 1, '\t') : NULL, *f4 = f3 ? strchr(f3 + 1, '\t') : NULL;
                    static char id[80];
                    snprintf(id, sizeof(id), "%.*s", f3 && f4 ? (int)(f4 - f3 - 1) : 0, f3 ? f3 + 1 : "");
                    nm = strcmp(id, "*") ? id : "saves";
                }
            }
            xfer_begin(&c, bars[i].what, nm);
            break;
        }
    if (!strcmp(l, "HELLO")) {
        say(fd, "card %s version %u features 0x%02x free_kb %u\n", c.card.id, c.card.version, c.card.features,
            c.card.free_kb);
    } else if (!strncmp(l, "SELECT ", 7)) {
        /* (Coreshell, just before a game's core loads) the game being started: <sys> \t <game ID>. PSX: that game's own
         * PS1 card on the bus (set game ID); other systems: Core, the card in that system's mode */
        char *f[2];
        if (split_tabs(a, f, 2) != 2 || !f[0][0] || !f[1][0])
            say(fd, "select failed\n");
        else if (!strcmp(f[0], "PSX")) {
            int r = smc_ps1_set_game_id(&c.bus, f[1]);
            lg("PSX: game %s %s", f[1], r == SMC_OK ? "selected on the card" : "not taken by the card");
            say(fd, r == SMC_OK ? "select ok\n" : "select failed\n");
        } else {
            int card = smc_core(&c.bus, f[0], true);
            lg("%s: game %s, %s %d", f[0], f[1], card > 0 ? "card" : "the card didn't answer Core", card);
            if (card > 0)
                say(fd, "select ok %d\n", card);
            else
                say(fd, "select failed\n");
        }
    } else if (!strcmp(l, "READ")) {
        smc_icons_retry(true); /* a manual read: games still on their system's icon are looked for again */
        int rr = smc_sync(&c.bus, &k, &rep);
        smc_icons_retry(false);
        icon_wait();
        int made = smc_fetch_icons(&k, fetch);
        int put = smc_apply_icons(&c.bus, &k);
        if (made || put)
            lg("icons: %d made from pictures, %d put on the card", made, put);
        report(fd, "read", rr, &rep);
    } else if (!strcmp(l, "SYSTEMS")) {
        char s[65];
        for (int i = 0; smc_list(&c.bus, ".", i, s) == SMC_OK; i++)
            say(fd, "%s\t%d\n", s, smc_core(&c.bus, s, false));
    } else if (!strncmp(l, "LIST ", 5)) {
        char sys[65] = "";
        int card = 0;
        sscanf(a, "%64s %d", sys, &card);
        char id[65];
        read_errs = 0;
        const char *st = store_of(sys);   /* (PSX: each game's own PS1 card) */
        for (int i = 0; list_card_r(&c.bus, st, card, i, id) == SMC_OK; i++) {
            struct smc_game_info gi;
            if (strcmp(st, sys) && !ps1_game_folder(id))
                continue;   /* (PSX: not PS1 mode's own cards) */
            if (strcmp(st, sys) && ps1card_drop_empty(&c, id) == 1) {
                i--;   /* a game's card with no save: deleted (the next one is at this place now) */
                continue;
            }
            if (game_info_r(&c.bus, st, card, id, &gi) != SMC_OK)
                continue;
            if (gi.has_icon)
                card_icon(&c, st, card, id);
            if (gi.files || gi.has_icon || gi.name[0]) /* as SECTION: a game folder with no save yet too, not a deleted one */
                say(fd, "%s\t%s\t%u\t%d\t%u\t%d\t%s\t%s\t%s\n", gi.name[0] ? gi.name : gi.title[0] ? gi.title : gi.save_name,
                    gi.id, gi.time, gi.files, gi.bytes, gi.has_icon ? 1 : 0, gi.save_name, id, gi.label);
            if (!strcmp(st, sys) && (gi.files || gi.has_icon)) /* the menus show its picture big (PSX: the library's) */
                smc_icon_want_cover(&k, sys, id, gi.name[0] ? gi.name : gi.save_name);
        }
        if (read_errs)
            say(fd, "partial\t%d\n", read_errs);
    } else if (!strncmp(l, "DELETE ", 7)) {
        char *tab = strchr(a, '\t');
        if (tab) {
            *tab = 0;
            say(fd, "delete %d\n", smc_delete_save(&c.bus, &k, a, tab + 1));
        }
    } else if (!strncmp(l, "CLEAR ", 6)) {
        char *tab = strchr(a, '\t');
        int card = tab ? atoi(tab + 1) : 0, n = -1;
        if (tab)
            *tab = 0;
        if (tab && play_sys[0] && !strcmp(play_sys, a))
            say(fd, "clear busy\n");
        else if (tab && strcmp(a, "PSX") && (n = smc_clear_card(&c.bus, &k, a, card)) >= 0) {
            lg("%s: card %d cleared (the card only): %d save(s)", a, card, n);
            say(fd, "clear ok %d\n", n);
        } else
            say(fd, "clear failed\n");
    } else if (!strncmp(l, "EXPORT ", 7) || !strncmp(l, "IMPORT ", 7)) {
        /* EXPORT <sys>\t<card>\t<id | *>\t<dir>: a game (or every game on that card) into <dir>/<id>/
         * IMPORT <sys>\t<card>\t<id | *>\t<dir>: <dir>/<id>/ (or every game folder in <dir>) onto that card
         * answer: export ok <games> <files> | import ok <games> <files> | ... failed */
        char *f[4];
        int imp = l[0] == 'I', games = 0, files = 0, bad = 0;
        if (split_tabs(a, f, 4) != 4 || f[3][0] != '/' || strstr(f[3], "..") || atoi(f[1]) < 1 || atoi(f[1]) > 99) {
            say(fd, "%s failed\n", imp ? "import" : "export");
        } else {
            /* PSX: each game's own PS1 card; one PS1 mode still has open (from the last game played) is let go first,
             * unless a PSX game is running now */
            psx_free = !strcmp(f[0], "PSX") && strcmp(play_sys, "PSX");
            f[0] = (char *)store_of(f[0]);
            int card = atoi(f[1]);
            if (psx_free && !strcmp(f[0], SMC_PS1CARD))
                psx_let_go(&c);     /* (a game's card still open in PS1 mode answers "busy": let go of first) */
            if (!imp && mkdir_p(f[3]))
                bad = 1;
            if (strcmp(f[2], "*")) {
                int r = bad ? -1 : imp ? import_game(&c, f[0], card, f[3], f[2]) : export_game(&c, f[0], card, f[2], f[3]);
                if (r < 0 && !bad && psx_free && !psx_let_go(&c))
                    r = imp ? import_game(&c, f[0], card, f[3], f[2]) : export_game(&c, f[0], card, f[2], f[3]);
                if (r < 0)
                    bad = 1;
                else {
                    games++;
                    files += r;
                }
            } else if (!imp) {
                char id[65];
                read_errs = 0;
                for (int i = 0; !bad && list_card_r(&c.bus, f[0], card, i, id) == SMC_OK; i++) {
                    struct smc_game_info gi;
                    if (!strcmp(f[0], SMC_PS1CARD) && !ps1_game_folder(id))
                        continue;   /* (PSX: not PS1 mode's own cards, Card1...) */
                    if (game_info_r(&c.bus, f[0], card, id, &gi) != SMC_OK || !gi.files)
                        continue;   /* (a game whose saves are gone) */
                    int r = export_game(&c, f[0], card, id, f[3]);
                    if (r < 0 && psx_free && !psx_let_go(&c))
                        r = export_game(&c, f[0], card, id, f[3]);
                    if (r < 0) {
                        lg("export: %s card %d: %s failed (%d)", f[0], card, id, r);
                        bad = 1;
                    } else {
                        games++;
                        files += r;
                    }
                }
                if (read_errs)
                    bad = 1;   /* a backup that missed a game isn't one */
            } else {
                DIR *d = opendir(f[3]);
                struct dirent *e;
                while (d && !bad && (e = readdir(d))) {
                    if (e->d_name[0] == '.' || (!strcmp(f[0], SMC_PS1CARD) && !ps1_game_folder(e->d_name)))
                        continue;   /* (PSX: never onto PS1 mode's own cards) */
                    int r = import_game(&c, f[0], card, f[3], e->d_name);
                    if (r < 0 && psx_free && !psx_let_go(&c))
                        r = import_game(&c, f[0], card, f[3], e->d_name);
                    if (r < 0)
                        bad = 1;
                    else {
                        games++;
                        files += r;
                    }
                }
                if (d)
                    closedir(d);
                else
                    bad = 1;
            }
            if (!bad)
                lg("%s: %s card %d: %d game(s), %d file(s) %s %s", imp ? "import" : "export", f[0], card, games, files,
                   imp ? "from" : "to", f[3]);
            say(fd, bad ? "%s failed\n" : "%s ok %d %d\n", imp ? "import" : "export", games, files);
        }
    } else if (!strncmp(l, "PSXMERGE ", 9)) {
        psx_merge(fd, &c, a);
    } else if (!strncmp(l, "DELGAME ", 8)) {
        /* DELGAME <sys>\t<card>\t<id>: that game's saves off that card only: delgame ok <files> | delgame failed */
        char *f[3];
        int r = split_tabs(a, f, 3) == 3 && !(play_sys[0] && !strcmp(play_sys, f[0])) ? delete_game(&c, store_of(f[0]), atoi(f[1]), f[2]) : -1;
        if (r >= 0)
            lg("%s card %s: %s deleted (%d file(s))", f[0], f[1], f[2], r);
        if (r >= 0 && !strcmp(store_of(f[0]), SMC_PS1CARD))
            ps1card_forget(f[2]);
        say(fd, r >= 0 ? "delgame ok %d\n" : "delgame failed\n", r);
    } else if (!strncmp(l, "IMAGE ", 6)) {
        /* a save's file as it is on the card (PSX: its 128 KB card image), for a frontend to open save by save */
        char *f1 = strchr(a, '\t'), *f2 = f1 ? strchr(f1 + 1, '\t') : NULL;
        if (!f2) {
            say(fd, "image failed\n");
        } else if (*f1 = *f2 = 0, store_of(a) != a) {
            /* PSX: the game's own PS1 card, from smcd's copy while its CRC (the card's generation for it) is the same */
            static uint8_t img[MC_SIZE];
            int r = ps1card_cached(&c, f2 + 1, img);
            if (r != 1) {
                say(fd, "image failed\n");
            } else {
                say(fd, "image %u\n", MC_SIZE);
                char line[300];
                for (uint32_t o = 0; o < MC_SIZE; o += 128) {
                    int k = 0;
                    for (uint32_t j = o; j < o + 128; j++)
                        k += snprintf(line + k, sizeof(line) - k, "%02x", img[j]);
                    line[k++] = '\n';
                    send_all(fd, line, (size_t)k);
                }
            }
        } else {
            int card = atoi(f1 + 1), n = smc_select_card(&c.bus, a, f2 + 1, card, NULL), best = -1;
            struct smc_file fi;
            uint32_t size = 0;
            for (int i = 0; i < n; i++) {
                if (smc_info(&c.bus, i, &fi) != SMC_OK) {
                    best = -1;
                    break;
                }
                bool meta = !strcmp(fi.name, "name.txt") || !strcmp(fi.name, "icon.bin") || !strcmp(fi.name, "game.ini") ||
                            !strcmp(fi.name, "label.txt");
                if (!fi.deleted && !meta && fi.size > size && fi.size <= 1024 * 1024) {
                    best = i;
                    size = fi.size;
                }
            }
            uint8_t *buf = best >= 0 ? malloc(size) : NULL;
            if (!buf || smc_read_file(&c.bus, best, size, buf) != SMC_OK) {
                say(fd, "image failed\n");
            } else {
                say(fd, "image %u\n", size);
                char line[300];
                for (uint32_t o = 0; o < size; o += 128) {
                    int k = 0;
                    for (uint32_t j = o; j < o + 128 && j < size; j++)
                        k += snprintf(line + k, sizeof(line) - k, "%02x", buf[j]);
                    line[k++] = '\n';
                    send_all(fd, line, (size_t)k);
                }
            }
            free(buf);
        }
    } else if (!strcmp(l, "SECTION") || !strcmp(l, "SECTION PSX")) {
        section(fd, &c, &k, l[7] != 0);
    } else if (!strcmp(l, "SAVES")) {
        /* the OSD's Saves page: the running game's saves, one per virtual card, and a new one on the first free card */
        if (!play_sys[0] || !play_game[0]) {
            say(fd, "nogame\n");
        } else {
            struct saves_list sl = {fd, {0}};
            int n = smc_game_saves(&c.bus, &k, play_sys, play_game, saves_cb, &sl);
            if (n >= 0) {
                int free_card = 0;
                for (int i = 1; i <= 99 && !free_card; i++)
                    if (!sl.used[i])
                        free_card = i;
                say(fd, "game\t%s\t%s\n", play_sys, play_game);
                if (free_card)
                    say(fd, "new\t%d\n", free_card);
            }
        }
    } else if (!strncmp(l, "CARDS ", 6)) {
        /* the memory manager's card picker: this system's cards on the card in that port, and a blank one */
        struct cards_list cl = {fd, {0}};
        int n = smc_system_cards(&c.bus, &k, a, cards_cb, &cl);
        if (n >= 0) {
            int free_card = 0;
            for (int i = 1; i <= 99 && !free_card; i++)
                if (!cl.used[i])
                    free_card = i;
            if (free_card)
                say(fd, "new\t%d\n", free_card);
        }
    } else if (!strncmp(l, "CARD ", 5)) {
        /* that system plays from this card from now on (a blank one: its games start with no save) */
        char *tab = strchr(a, '\t');
        int card = tab ? atoi(tab + 1) : 0;
        if (tab)
            *tab = 0;
        if (!tab || card < 1 || card > 99 || !strcmp(a, "PSX"))
            say(fd, "card failed\n");
        else if (play_sys[0] && !strcmp(play_sys, a))
            say(fd, "card busy\n");   /* a game of that system runs: the OSD's Saves page, not this */
        else if (smc_use_card(&c.bus, &k, a, "", card, &rep) == 0) {
            lg("%s: card %d picked in the memory manager", a, card);
            say(fd, "card ok %d\n", card);
        } else
            say(fd, "card failed\n");
    } else if (!strncmp(l, "NAME ", 5)) {
        /* the memory manager's Rename (Console Mode's on-screen keyboard): <sys> \t <card> \t <save name> \t <name> */
        char *t1 = strchr(a, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL, *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if (!t3)
            say(fd, "name failed\n");
        else {
            *t1 = *t2 = *t3 = 0;
            char *nm = t3 + 1;
            while (*nm == ' ')
                nm++;
            for (size_t e = strlen(nm); e && nm[e - 1] == ' '; e--)
                nm[e - 1] = 0;
            int r = smc_set_label(&c.bus, &k, a, t2 + 1, atoi(t1 + 1), nm);
            say(fd, r == 0 ? "name ok\n" : r == -2 ? "name nosave\n" : "name failed\n");
        }
    } else if (!strncmp(l, "PLAY ", 5)) {
        play(fd, &c, &k, a, &rep, 1);
    } else if (!strncmp(l, "PLAYPREP ", 9)) {
        play(fd, &c, &k, a, &rep, 0);
    } else if (!strncmp(l, "USE ", 4)) {
        /* a save picked in the OSD: this SuperStation's saves to their card, the picked card's in. Main then starts the
         * game again, so it loads that save */
        int card = atoi(a);
        if (!play_sys[0] || !play_game[0])
            say(fd, "nogame\n");
        else if (smc_use_card(&c.bus, &k, play_sys, play_game, card, &rep) == 0) {
            lg("%s: the save on card %d picked (%s)", play_sys, card, play_game);
            say(fd, "use ok %d\n", card);
            seen = synced = game_saves_time(play_sys, play_game);
        } else
            say(fd, "use failed\n");
    } else if (!strcmp(l, "CONFLICTS")) {
        smc_conflicts(&k, c.card.id, conflict_cb, &fd);
    } else if (!strncmp(l, "KEEP ", 5)) {
        char *sp = strchr(a, ' '), *tab = sp ? strchr(sp + 1, '\t') : NULL;
        if (sp && tab) {
            *sp = 0;
            *tab = 0;
            say(fd, "keep %d\n", smc_resolve(&c.bus, &k, sp + 1, tab + 1, !strcmp(a, "card") ? SMC_KEEP_CARD : SMC_KEEP_LOCAL));
        }
    } else {
        say(fd, "unknown\n");
    }
    card_close(&c);
    say(fd, "END\n");
}

static void handle(int fd) {
    char l[1200];
    want_port = 0;   /* a new connection: whichever port answers, until it says PORT */
    while (read_line(fd, l, sizeof(l), 2000) == 1) {
        if (!strncmp(l, "PORT ", 5)) {
            int p = atoi(l + 5);
            want_port = p == 1 || p == 2 ? p : 0;
            say(fd, want_port ? "port %d\nEND\n" : "port any\nEND\n", want_port);
            continue;
        }
        if (!strcmp(l, "PROBE")) {
            probe(fd);
            say(fd, "END\n");
            return;
        }
        if (!strncmp(l, "LAUNCH ", 7)) {
            lg("launch %s", l + 7);
            do_launch(l + 7);
            /* "saves": a game whose saves are on the card, for Main's OSD Saves page (PSX keeps its own card) */
            say(fd, play_game[0] && strcmp(play_sys, "PSX") ? "DONE saves\n" : "DONE\n");
            return;
        }
        if (!strncmp(l, "ROM ", 4)) {
            lg("rom %s", l + 4);
            say(fd, "%s", do_rom(l + 4));
            return;
        }
        if (!strcmp(l, "MENU")) {
            say(fd, "DONE\n");
            do_menu();
            return;
        }
        do_command(fd, l);
    }
}

/* ---- save icons (host/smc_sync.c): the download list, worked through in a child process (smcd keeps answering) ---- */
#include <sys/wait.h>

static pid_t icon_child;

static int fetch(const char *url, const char *to) {
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        /* MiSTer's curl has no certificate bundle of its own: Console Mode's (kept current with it), the MiSTer
         * downloader's, else Linux's (2021). Always checked: never without verifying. */
        static const char *const ca[] = {"/media/fat/ConsoleMode/cacert.pem", "/media/fat/Scripts/.config/downloader/cacert.pem",
                                         "/etc/ssl/certs/cacert.pem"};
        for (size_t i = 0; i < sizeof(ca) / sizeof(ca[0]); i++)
            if (access(ca[i], R_OK) == 0)
                execlp("curl", "curl", "-fsSL", "--max-time", "30", "--cacert", ca[i], "-o", to, url, (char *)NULL);
        execlp("curl", "curl", "-fsSL", "--max-time", "30", "-o", to, url, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    if (p < 0 || waitpid(p, &st, 0) < 0)
        return -1;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int icons_waiting(const char *file) {
    char p[1100];
    snprintf(p, sizeof(p), "%s/icons/%s", state, file);
    return access(p, F_OK) == 0;
}

static void icon_wait(void) {
    if (icon_child > 0) {
        waitpid(icon_child, NULL, 0);
        icon_child = 0;
    }
}

/* the game lists the listed games need (their official names: libretro's covers are filed by them), from cm_modules'
 * gamedb module, which keeps them in Console Mode's gamedb folder; without it (or offline) icons come from the other
 * places */
#define GAMEDB_MODULE "/media/fat/ConsoleMode/tools/modules/gamedb/gamedb.sh"
static void lists_for_icons(const struct smc_sync_cfg *k) {
    char names[16][96];
    int n = smc_icon_lists_needed(k, names, 16);
    if (n <= 0 || access(GAMEDB_MODULE, X_OK))
        return;
    pid_t p = fork();
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        char *argv[20];
        int a = 0;
        argv[a++] = (char *)"bash";
        argv[a++] = (char *)GAMEDB_MODULE;
        argv[a++] = (char *)"update";
        for (int i = 0; i < n; i++)
            argv[a++] = names[i];
        argv[a] = NULL;
        setenv("GAMEDB_DIR", k->gamedb_dir, 1);
        execvp("bash", argv);
        _exit(127);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

/* from the main loop: collect a finished download run; start one when games are listed */
static void icon_poll(void) {
    if (icon_child > 0) {
        if (waitpid(icon_child, NULL, WNOHANG) != icon_child)
            return;
        icon_child = 0;
    }
    if (!icons_waiting("queue.txt"))
        return;
    pid_t p = fork();
    if (p == 0) {
        struct smc_sync_cfg k = cfg();
        lists_for_icons(&k);
        int made = smc_fetch_icons(&k, fetch);
        if (made)
            lg("icons: %d made from pictures (on the card the next time it's opened)", made);
        _exit(0);
    }
    if (p > 0)
        icon_child = p;
}

/* icons made since the card was last opened go on it now */
static void icons_to_card(struct conn *c) {
    if (!icons_waiting("ready.txt"))
        return;
    struct smc_sync_cfg k = cfg();
    int put = smc_apply_icons(&c->bus, &k);
    if (put)
        lg("icons: %d put on the card", put);
}

int main(int argc, char **argv) {
    static char sv[1024], st[1024], ls[1024], g[8][1024];
    int opt;
    while ((opt = getopt(argc, argv, "f:S:T:g:d:l:v:G")) != -1) {
        switch (opt) {
        case 'f': root = optarg; break;
        case 'S': saves = optarg; break;
        case 'T': state = optarg; break;
        case 'd': lists = optarg; break;
        case 'l': sock_path = optarg; break;
        case 'v': vcard_path = optarg; break;
        case 'G': allow_no_identity = 1; break;
        case 'g': if (ngames < 15) games[ngames++] = optarg; break;
        default: fprintf(stderr, "usage: smcd [-f root] [-S saves] [-T state] [-g games]... [-d lists] [-l socket] [-v vcard] [-G (test builds: cards without their identity)]\n"); return 2;
        }
    }
    if (!saves) snprintf(sv, sizeof(sv), "%s/saves", root), saves = sv;
    /* Console Mode's folders: Super MemCard's own, and the game lists of cm_modules' gamedb module */
    if (!state) snprintf(st, sizeof(st), "%s/ConsoleMode/supermemcard", root), state = st;
    if (!lists) snprintf(ls, sizeof(ls), "%s/ConsoleMode/gamedb", root), lists = ls;
    if (!ngames) {
        snprintf(g[0], sizeof(g[0]), "%s/games", root);
        games[ngames++] = g[0];
        for (int u = 0; u < 6; u++) {
            snprintf(g[1 + u], sizeof(g[1 + u]), "/media/usb%d/games", u);
            games[ngames++] = g[1 + u];
        }
        snprintf(g[7], sizeof(g[7]), "%s/cifs/games", root);
        games[ngames++] = g[7];
    }
    games[ngames] = NULL;
    signal(SIGPIPE, SIG_IGN);

    unlink(sock_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) || listen(lfd, 4)) {
        perror("smcd: socket");
        return 1;
    }
    lg("smcd started");
    if (smc_genuine_test_key())
        lg("built with the TEST Retro Remake key (tools/smccert/test-rr.key): not for a release");
    if (allow_no_identity)
        lg(smc_genuine_test_key() ? "TEST (-G): cards without their identity are used" : "-G ignored: not a test build");
    for (;;) {
        struct pollfd p = {lfd, POLLIN, 0};
        if (poll(&p, 1, 1000) > 0) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0) {
                handle(fd);
                close(fd);
            }
        }
        game_poll();
        icon_poll();
    }
}
