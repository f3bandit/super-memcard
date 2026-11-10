/* smcd: Super MemCard on the SuperStation. A program of its own beside Main: Main only passes card frames to the
 * front ports for it and tells it when a game starts and when the menu is back (main/smc_shim.cpp); the memory manager
 * (Console Mode's frontend) sends it its commands. Everything Super MemCard does is here.
 * Copyright (C) 2026 f3bandit. All rights reserved; licensed exclusively to Retro Remake (LICENSE, docs/11-licensing.md).
 *
 *   smcd [-f <root>] [-S <saves>] [-T <state>] [-g <games dir>]... [-d <game lists>] [-l <socket>] [-v <vcard socket>]
 *
 * Its socket (/tmp/smcd.sock), one command a line; the answer's lines end with END (LAUNCH and MENU: DONE). No card
 * (or not a Super MemCard card): nocard; a Super MemCard card that isn't genuine (docs/12): notgenuine <card> <why>.
 *   LAUNCH <rbf>\t<mgl>     (Main) a game core is about to load: Core, and its save from the card if the card's is newer
 *   MENU                    (Main) the menu is back: the game just played goes to the card
 *   HELLO                   the card: id, version, free space
 *   READ                    the memory manager's Read card: everything both ways
 *   SYSTEMS                 the systems with saves on the card
 *   LIST <sys> [<card>]     a system's games: name \t id \t time \t files \t bytes \t icon \t save name \t folder
 *   DELETE <sys>\t<save>    delete a save (others delete theirs at their next sync)
 *   CONFLICTS               saves whose two copies both changed
 *   KEEP <local|card> <sys>\t<save>
 * While a game runs, its save goes to the card as soon as the game saves (its save files changed and settled). */
#define _GNU_SOURCE
#include "smc.h"
#include "smc_sync.h"
#include "smc_vcard.h"

#include <dirent.h>
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
    int speed = (len > 1 && out[1] == 0x60) ? 0 : frame_speed;   /* Hello as a PS1 does it */
    int n = front_raw(f, out, in, len, speed);
    while (speed && n >= 0 && n < len) {
        frame_speed = --speed;
        lg("a fast frame wasn't answered whole: speed %d from now on", speed);
        usleep(60000);
        n = front_raw(f, out, in, len, speed);
    }
    return n;
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
 * 1: genuine. */
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

/* the card: vcardd if it runs, else the first front port whose card answers Hello. 1: a card. */
static int card_open(struct conn *c) {
    struct stat st;
    memset(c, 0, sizeof(*c));
    c->front.fd = -1;
    if (stat(vcard_path, &st) == 0 && smc_vcard_open(vcard_path, &c->bus) == 0) {
        c->vcard = 1;
        if (smc_hello(&c->bus, SMC_HOST_MAIN, 0, &c->card) == 1 && genuine(c))
            return 1;
        smc_vcard_close(&c->bus);
        return 0;
    }
    c->front.fd = unix_connect(FRONT_SOCK);
    if (c->front.fd < 0)
        return 0;   /* Main isn't passing frames (no shim, or not running) */
    hold(c, 1);
    c->bus.frame = front_frame;
    c->bus.ctx = &c->front;
    for (int p = 1; p <= 2; p++) {
        c->front.port = p;
        int h = smc_hello(&c->bus, SMC_HOST_MAIN, 0, &c->card);
        if (h == 1 && genuine(c))
            return 1;
        if (h == 1)
            break;   /* a Super MemCard card, not genuine: a PS1 card (Main goes on as without Super MemCard) */
        if (h < 0 && p == 1) {
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
}

/* ---- the sync's setup ---- */
static struct smc_sync_cfg cfg(void) {
    struct smc_sync_cfg k = {saves, state, smc_console_systems, SMC_HOST_MAIN, lib_log, games, lists};
    return k;
}

/* the game being played: its system and save name (empty: none known) */
static char play_sys[65], play_game[256];
static time_t seen, synced;   /* its saves' newest time: last seen, last on the card */
static int no_card;           /* its core has no front ports, or no card: stop asking until the next game */

static time_t game_saves_time(const char *sys, const char *game) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", saves, sys);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
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
        if (!stat(p, &st) && S_ISREG(st.st_mode) && st.st_mtime > newest)
            newest = st.st_mtime;
    }
    closedir(d);
    return newest;
}

/* ---- the jobs ---- */
static void do_launch(char *args) {
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
    play_sys[0] = play_game[0] = 0;
    if (!sys)
        return;
    snprintf(play_sys, sizeof(play_sys), "%s", sys);
    snprintf(play_game, sizeof(play_game), "%s", m.game);
    no_card = 0;
    struct conn c;
    if (!card_open(&c)) {
        lg("%s: no card reachable (the save is checked again while the game runs)", sys);
        return;
    }
    int card = smc_core(&c.bus, sys, true);
    lg("%s core: %s %d", sys, card > 0 ? "its card" : "the card didn't answer Core", card);
    if (card > 0 && play_game[0]) {
        struct smc_sync_cfg k = cfg();
        struct smc_sync_report rep;
        if (smc_sync_game(&c.bus, &k, sys, play_game, &rep) < 0)
            lg("game sync stopped with an error");
    }
    card_close(&c);
    seen = synced = play_game[0] ? game_saves_time(play_sys, play_game) : 0;
}

static void do_menu(void) {
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
    if (!play_game[0] || no_card || !strcmp(play_sys, "PSX"))   /* PSX uses the front ports itself */
        return;
    time_t t = game_saves_time(play_sys, play_game);
    if (t != seen) {
        seen = t;
        return;
    }
    if (!t || t == synced)
        return;
    struct conn c;
    if (!card_open(&c)) {
        no_card = 1;
        lg("in-game: no card reachable from this core (the save goes after the game)");
        return;
    }
    struct smc_sync_cfg k = cfg();
    struct smc_sync_report rep;
    if (smc_sync_game(&c.bus, &k, play_sys, play_game, &rep) < 0)
        lg("in-game sync stopped with an error");
    else
        synced = t;
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
        report(fd, "read", smc_sync(&c.bus, &k, &rep), &rep);
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
            if (smc_game_info(&c.bus, sys, card, id, &gi) == SMC_OK)
                say(fd, "%s\t%s\t%u\t%d\t%u\t%d\t%s\t%s\n", gi.name[0] ? gi.name : gi.title[0] ? gi.title : gi.save_name,
                    gi.id, gi.time, gi.files, gi.bytes, gi.has_icon ? 1 : 0, gi.save_name, id);
        }
    } else if (!strncmp(l, "DELETE ", 7)) {
        char *tab = strchr(a, '\t');
        if (tab) {
            *tab = 0;
            say(fd, "delete %d\n", smc_delete_save(&c.bus, &k, a, tab + 1));
        }
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
    while (read_line(fd, l, sizeof(l), 2000) == 1) {
        if (!strncmp(l, "LAUNCH ", 7)) {
            lg("launch %s", l + 7);
            do_launch(l + 7);
            say(fd, "DONE\n");
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

int main(int argc, char **argv) {
    static char sv[1024], st[1024], ls[1024], g[8][1024];
    int opt;
    while ((opt = getopt(argc, argv, "f:S:T:g:d:l:v:")) != -1) {
        switch (opt) {
        case 'f': root = optarg; break;
        case 'S': saves = optarg; break;
        case 'T': state = optarg; break;
        case 'd': lists = optarg; break;
        case 'l': sock_path = optarg; break;
        case 'v': vcard_path = optarg; break;
        case 'g': if (ngames < 15) games[ngames++] = optarg; break;
        default: fprintf(stderr, "usage: smcd [-f root] [-S saves] [-T state] [-g games]... [-d lists] [-l socket] [-v vcard]\n"); return 2;
        }
    }
    if (!saves) snprintf(sv, sizeof(sv), "%s/saves", root), saves = sv;
    if (!state) snprintf(st, sizeof(st), "%s/supermemcard", root), state = st;
    if (!lists) snprintf(ls, sizeof(ls), "%s/supermemcard/gamelists", root), lists = ls;
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
    }
}
