/* smcd: Super MemCard on the SuperStation. A program of its own beside Main: Main only passes card frames to the
 * front ports for it and tells it when a game starts and when the menu is back (main/smc_shim.cpp); the memory manager
 * (Console Mode's frontend) sends it its commands. Everything Super MemCard does is here.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed exclusively to Retro Remake (LICENSE, docs/11-licensing.md).
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
 *   HELLO                   the card: id, version, free space
 *   READ                    the memory manager's Read card: everything both ways
 *   SYSTEMS                 the systems with saves on the card
 *   LIST <sys> [<card>]     a system's games: name \t id \t time \t files \t bytes \t icon \t save name \t folder
 *   SECTION [PSX]           every other system's saves at once, for the memory manager's screen (section() below)
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

/* ---- the card: through Main's front ports (smc_shim) or, for testing, vcardd ---- */
struct front {
    int fd, port;
};
static int frame_speed = SMC_SPEED;

static int front_raw(struct front *f, const uint8_t *out, uint8_t *in, int len, int speed) {
    char l[600];
    int k = snprintf(l, sizeof(l), "FRAME %d %d %d ", f->port, speed, SMC_ACK_TIMEOUT_MS);
    for (int i = 0; i < len; i++)
        k += snprintf(l + k, sizeof(l) - k, "%02x", out[i]);
    l[k++] = '\n';
    if (send_all(f->fd, l, (size_t)k))
        return -3;
    if (read_line(f->fd, l, sizeof(l), 2000 + SMC_ACK_TIMEOUT_MS * 4) != 1)
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
    if (front_raw(f, out, ref, sizeof(out), 0) != (int)sizeof(out)) {
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
    read_line(c->front.fd, l, sizeof(l), 2000);
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

static int card_open(struct conn *c) {
    char who[120];
    memset(c, 0, sizeof(*c));
    c->front.fd = -1;
    if (mc_lock(10000, who, sizeof(who))) {
        static char said[120];
        if (strcmp(said, who))
            lg("the card is in use (%s): not now", who);
        snprintf(said, sizeof(said), "%s", who);
        return 0;
    }
    if (card_open_bus(c))
        return 1;
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
    return 0;
}

static void card_close(struct conn *c) {
    if (c->vcard)
        smc_vcard_close(&c->bus);
    else if (c->front.fd >= 0) {
        hold(c, 0);
        close(c->front.fd);
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

static void play(int fd, struct conn *c, const struct smc_sync_cfg *k, char *a, struct smc_sync_report *rep) {
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
    int rc = smc_sync_game(&c.bus, &k, play_sys, play_game, &rep);
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
static void section(int fd, struct conn *c, const struct smc_sync_cfg *k, int with_psx) {
    static struct conflict_set cs;
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
        for (int i = 0; smc_list_card(&c->bus, sys, card, i, id) == SMC_OK; i++) {
            struct smc_game_info gi;
            if (smc_game_info(&c->bus, sys, card, id, &gi) != SMC_OK)
                continue;
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
    if (!strcmp(l, "HELLO")) {
        say(fd, "card %s version %u features 0x%02x free_kb %u\n", c.card.id, c.card.version, c.card.features,
            c.card.free_kb);
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
        for (int i = 0; smc_list_card(&c.bus, sys, card, i, id) == SMC_OK; i++) {
            struct smc_game_info gi;
            if (smc_game_info(&c.bus, sys, card, id, &gi) == SMC_OK && gi.files) /* not a set whose saves are gone */
                say(fd, "%s\t%s\t%u\t%d\t%u\t%d\t%s\t%s\t%s\n", gi.name[0] ? gi.name : gi.title[0] ? gi.title : gi.save_name,
                    gi.id, gi.time, gi.files, gi.bytes, gi.has_icon ? 1 : 0, gi.save_name, id, gi.label);
        }
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
        play(fd, &c, &k, a, &rep);
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
