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
 *   SECTION [PSX]           every other system's saves at once, for the memory manager's screen (section() below)
 *   SAVES                   (Main's OSD) the running game's saves: save \t card \t files \t time \t playing, one per
 *                           virtual card that has it, then game \t sys \t name and new \t <the first free card>
 *   USE <card>              (Main's OSD) play the running game with its save on that card: this SuperStation's saves go
 *                           to their card first; Main then starts the game again so it loads it. use ok <card> | failed
 *   DELETE <sys>\t<save>    delete a save (others delete theirs at their next sync)
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

/* the card: vcardd if it runs, else the first front port whose card answers Hello. 1: a card. */
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
    c->bus.ctx = &c->front;
    for (int p = 1; p <= 2; p++) {
        c->front.port = p;
        int h = smc_hello(&c->bus, SMC_HOST_MAIN, 0, &c->card);
        if (h == 1 && genuine(c)) {
            icons_to_card(c);
            return 1;
        }
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
            int w = snprintf(line, sizeof(line), "game\t%s\t%s\t%s\t%u\t%d\t%u\t%d\t%s\t%s\t%d\n", sys,
                             gi.name[0] ? gi.name : gi.title[0] ? gi.title : gi.save_name, gi.id, gi.time, gi.files,
                             gi.bytes, gi.has_icon ? 1 : 0, gi.save_name, id, both);
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

struct saves_list {
    int fd;
    bool used[100];
};
static void saves_cb(int card, int files, uint32_t time, bool playing, void *ctx) {
    struct saves_list *sl = ctx;
    if (card >= 1 && card <= 99)
        sl->used[card] = true;
    say(sl->fd, "save\t%d\t%d\t%u\t%d\n", card, files, time, playing ? 1 : 0);
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
