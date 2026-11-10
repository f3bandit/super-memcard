/* smccli: everything a frontend does with Super MemCard, from a shell (the host library, host/), against the virtual
 * card (vcardd) or anything else that serves its socket. For testing on a SuperStation and as a reference for a
 * frontend (Console Mode): each command is the library call a frontend makes at that moment (docs/10-console-mode-guide.md).
 *
 *   smccli [options] hello                       the card (ID, version, free space)
 *   smccli [options] launch <rbf> [<mgl>]        before a game core loads: Core + the game's save in from the card
 *   smccli [options] back <sys> [<game>]         back in the menu after it: its save out to the card (no game: what changed)
 *   smccli [options] read                        the memory manager's "Read card": everything both ways
 *   smccli [options] systems                     the systems the card has saves for
 *   smccli [options] list <sys> [<card>]         a system's games on the card, with name, ID, date and time
 *   smccli [options] delete <sys> <save>         delete a save (the card keeps a tombstone; nothing is lost)
 *   smccli [options] conflicts                   saves whose two copies both changed
 *   smccli [options] keep <local|card> <sys> <save>
 *   smccli [options] core <sys|.>                the card's buttons pick that system's card (".": the PS1 card's again)
 *   smccli mgl <file>                            what an MGL starts: core, save folder, save name
 * options:
 *   -s <socket>   vcardd's socket (default /tmp/supermemcard.sock)
 *   -f <dir>      the SD card's root (default /media/fat): saves, games, supermemcard state and game lists under it
 *   -S <dir>      the saves folder (default <root>/saves)     -T <dir>  state (default <root>/supermemcard)
 *   -g <dir>      a games folder (repeat; default <root>/games, /media/usb0..5/games, <root>/cifs/games)
 *   -d <dir>      the game lists (default <root>/supermemcard/gamelists)
 * Copyright (C) 2026 f3bandit. */
#include "smc.h"
#include "smc_sync.h"
#include "smc_vcard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void out(const char *l) { printf("  %s\n", l); }

static void report(const char *what, int rc, const struct smc_sync_report *r) {
    printf("%s: %s%s card %s, pushed %d, pulled %d, unchanged %d, conflicts %d, skipped %d, failed %d, switched %d, "
           "deleted %d\n",
           what, rc < 0 ? "STOPPED (error)," : "", r->card ? "" : " no Super MemCard,", r->card_id, r->pushed,
           r->pulled, r->unchanged, r->conflicts, r->skipped, r->failed, r->switched, r->deleted);
}

static void conflict_cb(const char *sys, int card, const char *save, uint32_t gen, void *ctx) {
    (void)ctx;
    printf("%s\tCard%d\t%s\tcard generation %u\n", sys, card, save, gen);
}

static const char *when(uint32_t t, char *buf, size_t n) {
    if (!t)
        return "-";
    time_t tt = t;
    strftime(buf, n, "%Y-%m-%d %H:%M UTC", gmtime(&tt));
    return buf;
}

static int list_sys(const struct smc_bus *b, const char *sys, int card) {
    char id[65], tb[40];
    int n = 0;
    for (int i = 0; smc_list_card(b, sys, card, i, id) == SMC_OK; i++, n++) {
        struct smc_game_info gi;
        if (smc_game_info(b, sys, card, id, &gi) != SMC_OK) {
            printf("%s\t(info failed)\n", id);
            continue;
        }
        /* what a memory manager shows: the name, else the header's title, else the save's name; the save's own name
         * too, which tells two saves of one game apart (two copies of a ROM, each with its own progress) */
        printf("%s | %s | %s | %d files, %u bytes | icon %s | save \"%s\" | folder %s\n",
               gi.name[0] ? gi.name : gi.title[0] ? gi.title : gi.save_name, gi.id, when(gi.time, tb, sizeof(tb)),
               gi.files, gi.bytes, gi.has_icon ? "yes" : "no", gi.save_name, id);
    }
    return n;
}

int main(int argc, char **argv) {
    const char *sock = SMC_VCARD_SOCKET, *root = "/media/fat", *saves = NULL, *state = NULL, *lists = NULL;
    const char *games[16];
    int ng = 0, a = 1;
    for (; a + 1 < argc && argv[a][0] == '-' && argv[a][2] == 0; a += 2) {
        switch (argv[a][1]) {
        case 's': sock = argv[a + 1]; break;
        case 'f': root = argv[a + 1]; break;
        case 'S': saves = argv[a + 1]; break;
        case 'T': state = argv[a + 1]; break;
        case 'd': lists = argv[a + 1]; break;
        case 'g': if (ng < 15) games[ng++] = argv[a + 1]; break;
        default: fprintf(stderr, "unknown option %s\n", argv[a]); return 2;
        }
    }
    if (a >= argc) {
        fprintf(stderr, "usage: smccli [options] hello|chip|launch|back|read|systems|list|delete|conflicts|keep|core|mgl ...\n");
        return 2;
    }
    const char *cmd = argv[a++];
    int left = argc - a;

    if (!strcmp(cmd, "mgl")) {
        struct smc_mgl m;
        if (left < 1 || smc_mgl_read(argv[a], &m)) {
            fprintf(stderr, "can't read the MGL\n");
            return 1;
        }
        const char *sys = smc_launch_system(m.rbf, m.setname, m.same_dir);
        printf("rbf=%s\nsetname=%s%s\nsave folder=%s\nsave name=%s\n", m.rbf, m.setname, m.same_dir ? " (same_dir)" : "",
               sys ? sys : "(not a system the card keeps)", m.game);
        return 0;
    }

    static char sv[1024], st[1024], ls[1024], g[8][1024];
    if (!saves) snprintf(sv, sizeof(sv), "%s/saves", root), saves = sv;
    if (!state) snprintf(st, sizeof(st), "%s/supermemcard", root), state = st;
    if (!lists) snprintf(ls, sizeof(ls), "%s/supermemcard/gamelists", root), lists = ls;
    if (!ng) {
        snprintf(g[0], sizeof(g[0]), "%s/games", root);
        games[ng++] = g[0];
        for (int u = 0; u < 6; u++) {
            snprintf(g[1 + u], sizeof(g[1 + u]), "/media/usb%d/games", u);
            games[ng++] = g[1 + u];
        }
        snprintf(g[7], sizeof(g[7]), "%s/cifs/games", root);
        games[ng++] = g[7];
    }
    games[ng] = NULL;
    struct smc_sync_cfg cfg = {saves, state, smc_console_systems, SMC_HOST_TEST, out, games, lists};

    struct smc_bus bus;
    if (smc_vcard_open(sock, &bus)) {
        fprintf(stderr, "no card at %s\n", sock);
        return 1;
    }
    struct smc_card card;
    int h = smc_hello(&bus, SMC_HOST_TEST, 0, &card);
    if (h != 1) {
        printf("no Super MemCard card answered Hello (%d)\n", h);
        smc_vcard_close(&bus);
        return 1;
    }
    struct smc_sync_report rep;
    memset(&rep, 0, sizeof(rep));
    int rc = 0;

    if (!strcmp(cmd, "hello")) {
        printf("card %s, protocol %u, features 0x%02x, free %u KB\n", card.id, card.version, card.features, card.free_kb);
    } else if (!strcmp(cmd, "chip")) {
        /* the card's chip ID (for smccert issue) and whether it's a genuine Retro Remake card */
        uint8_t chip[8], pub[32], cert[64];
        char why[96];
        int c = smc_cert(&bus, chip, pub, cert);
        if (c < 0) {
            printf("the card doesn't answer CERT (%d)\n", c);
            rc = 1;
        } else {
            printf("chip ");
            for (int i = 0; i < 8; i++)
                printf("%02x", chip[i]);
            int g = smc_genuine(&bus, &card, why, sizeof(why));
            printf("\n%s%s%s\n", g == 1 ? "genuine" : "not genuine", g == 1 ? "" : ": ", g == 1 ? "" : why);
            rc = g == 1 ? 0 : 3;
        }
    } else if (!strcmp(cmd, "launch") && left >= 1) {
        /* Main, before it loads a core (fpga_load_rbf): which save folder, then Core and the game's save */
        struct smc_mgl m = {0};
        if (left >= 2 && smc_mgl_read(argv[a + 1], &m)) {
            fprintf(stderr, "can't read the MGL\n");
            rc = -1;
        } else {
            const char *sys = smc_launch_system(argv[a], m.setname, m.same_dir);
            if (!sys) {
                printf("not a system the card keeps: the card isn't touched\n");
            } else {
                int c = smc_core(&bus, sys, true);
                printf("%s: virtual card %d\n", sys, c);
                if (c > 0 && m.game[0]) {
                    rc = smc_sync_game(&bus, &cfg, sys, m.game, &rep);
                    report("load", rc, &rep);
                } else if (c > 0) {
                    printf("no game file in the launch: its save is synced when the menu is back\n");
                }
            }
        }
    } else if (!strcmp(cmd, "back") && left >= 1) {
        /* back in the menu: the game just played goes to the card, then the buttons are the PS1 card's again */
        rc = left >= 2 ? smc_sync_game(&bus, &cfg, argv[a], argv[a + 1], &rep) : smc_sync_changed(&bus, &cfg, argv[a], &rep);
        report("save", rc, &rep);
        smc_core(&bus, NULL, true);
    } else if (!strcmp(cmd, "read")) {
        rc = smc_sync(&bus, &cfg, &rep);
        report("read card", rc, &rep);
    } else if (!strcmp(cmd, "systems")) {
        char s[65];
        for (int i = 0; smc_list(&bus, ".", i, s) == SMC_OK; i++)
            printf("%s\tcard %d\n", s, smc_core(&bus, s, false));
    } else if (!strcmp(cmd, "list") && left >= 1) {
        int n = list_sys(&bus, argv[a], left >= 2 ? atoi(argv[a + 1]) : 0);
        printf("%d games\n", n);
    } else if (!strcmp(cmd, "delete") && left >= 2) {
        rc = smc_delete_save(&bus, &cfg, argv[a], argv[a + 1]);
        printf("delete: %d\n", rc);
    } else if (!strcmp(cmd, "conflicts")) {
        printf("%d conflicts\n", smc_conflicts(&cfg, card.id, conflict_cb, NULL));
    } else if (!strcmp(cmd, "keep") && left >= 3) {
        rc = smc_resolve(&bus, &cfg, argv[a + 1], argv[a + 2], !strcmp(argv[a], "card") ? SMC_KEEP_CARD : SMC_KEEP_LOCAL);
        printf("keep %s: %d\n", argv[a], rc);
    } else if (!strcmp(cmd, "core") && left >= 1) {
        int c = smc_core(&bus, strcmp(argv[a], ".") ? argv[a] : NULL, true);
        printf("core %s: card %d\n", argv[a], c);
    } else {
        fprintf(stderr, "unknown command or missing arguments: %s\n", cmd);
        rc = -1;
    }
    smc_vcard_close(&bus);
    return rc < 0 ? 1 : 0;
}
